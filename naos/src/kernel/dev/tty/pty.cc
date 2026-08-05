#include "kernel/dev/tty/pty.hpp"

#include "kernel/errno.hpp"
#include "kernel/task.hpp"

namespace dev::tty
{
namespace
{
void pty_control_event(dev::tty::control_event event, group_id foreground_group, u64 user_data)
{
    (void)user_data;
    if (foreground_group == 0)
        return;

    task::signal::signal signal_number = task::signal::sigint;
    switch (event)
    {
        case control_event::interrupt:
            signal_number = task::signal::sigint;
            break;
        case control_event::quit:
            signal_number = task::signal::sigquit;
            break;
        case control_event::suspend:
            signal_number = task::signal::sigtstp;
            break;
    }
    task::send_signal_to_process_group(foreground_group, signal_number);
}
} // namespace

pty_pair::pty_pair(u32 index, u64 input_buffer_size, u64 output_buffer_size, freelibcxx::Allocator *allocator)
    : index_(index)
    , core_(input_buffer_size, output_buffer_size, allocator)
    , master_(this)
    , slave_(this)
    , master_open_(false)
    , slave_open_(false)
    , slave_locked_(true)
    , master_references_(0)
    , slave_references_(0)
{
    core_.set_control_event_handler(pty_control_event);
}

int pty_endpoint::open(flag_t flags)
{
    (void)flags;
    if (pair_ == nullptr)
        return EIO;
    const bool opened =
        role_ == pty_endpoint_role::master ? pair_->open_master_reference() : pair_->open_slave_reference();
    return opened ? 0 : EIO;
}

bool pty_endpoint::can_operate() const
{
    if (pair_ == nullptr)
        return false;
    if (role_ == pty_endpoint_role::master)
        return pair_->master_open();
    return pair_->slave_open();
}

i64 pty_endpoint::read(byte *data, u64 max_size, flag_t flags)
{
    if (!can_operate())
        return EIO;
    if (role_ == pty_endpoint_role::master)
        return pair_->core().read_output(data, max_size, flags);
    const auto job_control = task::check_tty_job_control(task::current_process(), &pair_->core(), true);
    if (job_control != 0)
        return job_control;
    return pair_->core().read_input(data, max_size, flags);
}

i64 pty_endpoint::write(const byte *data, u64 size, flag_t flags)
{
    if (!can_operate())
        return EIO;
    if (role_ == pty_endpoint_role::master)
        return pair_->core().receive_input(data, size, flags);
    const auto job_control = task::check_tty_job_control(
        task::current_process(), &pair_->core(), false,
        (pair_->core().get_termios().c_lflag & termios_lflag::tostop) != 0);
    if (job_control != 0)
        return job_control;
    return pair_->core().write_output(data, size, flags);
}

bool pty_endpoint::native_tty_get_attributes(termios_t &attributes)
{
    if (!can_operate())
        return false;
    attributes = pair_->core().get_termios();
    return true;
}

bool pty_endpoint::native_tty_set_attributes(const termios_t &attributes)
{
    if (!can_operate())
        return false;
    pair_->core().set_termios(attributes);
    return true;
}

bool pty_endpoint::native_tty_get_winsize(winsize_t &size)
{
    if (!can_operate())
        return false;
    size = pair_->core().get_winsize();
    return true;
}

bool pty_endpoint::native_tty_set_winsize(const winsize_t &size)
{
    if (!can_operate())
        return false;
    pair_->core().set_winsize(size);
    return true;
}

i64 pty_endpoint::native_tty_flush(i32 queue)
{
    if (!can_operate())
        return EIO;
    if (queue == tty_flush::input || queue == tty_flush::both)
        pair_->core().flush_input();
    if (queue == tty_flush::output || queue == tty_flush::both)
        pair_->core().flush_output();
    return queue >= tty_flush::input && queue <= tty_flush::both ? 0 : EINVAL;
}

i64 pty_endpoint::native_tty_attach(bool force)
{
    if (!can_operate() || role_ != pty_endpoint_role::slave)
        return ENOTTY;
    auto *process = task::current_process();
    const auto result = task::attach_controlling_tty(process, &pair_->core(), force);
    if (result == 0 && process != nullptr)
    {
        pair_->core().set_session_id(process->session_id);
        pair_->core().set_foreground_process_group(process->process_group_id);
    }
    return result;
}

i64 pty_endpoint::native_tty_get_pgrp(u32 &group)
{
    if (!can_operate())
        return EIO;
    group = static_cast<u32>(pair_->core().foreground_process_group());
    return 0;
}

i64 pty_endpoint::native_tty_set_pgrp(u32 group)
{
    if (!can_operate())
        return EIO;
    return task::set_foreground_process_group(task::current_process(), &pair_->core(), static_cast<group_id>(group));
}

i64 pty_endpoint::native_tty_get_sid(u32 &session)
{
    if (!can_operate())
        return EIO;
    session = static_cast<u32>(pair_->core().session_id());
    return 0;
}

i64 pty_endpoint::native_tty_detach()
{
    if (!can_operate() || role_ != pty_endpoint_role::slave)
        return ENOTTY;
    auto *process = task::current_process();
    task::detach_controlling_tty(process);
    if (process != nullptr && process->pid == process->session_id && process->controlling_tty == nullptr)
    {
        pair_->core().set_foreground_process_group(0);
        pair_->core().set_session_id(0);
    }
    return 0;
}

i64 pty_endpoint::native_tty_get_input(u64 &count)
{
    if (!can_operate())
        return EIO;
    count = pair_->core().input_readable() ? 1 : 0;
    return 0;
}

bool pty_endpoint::native_pty_get_number(u32 &number)
{
    if (!can_operate() || role_ != pty_endpoint_role::master)
        return false;
    number = pair_->index();
    return true;
}

bool pty_endpoint::native_pty_set_locked(bool locked)
{
    if (!can_operate() || role_ != pty_endpoint_role::master)
        return false;
    pair_->set_slave_locked(locked);
    return true;
}

void pty_endpoint::close()
{
    if (pair_ == nullptr)
        return;
    if (role_ == pty_endpoint_role::master)
        pair_->close_master();
    else
        pair_->close_slave();
}

u32 pty_endpoint::poll_events() const
{
    if (pair_ == nullptr)
        return tty_poll::error | tty_poll::hangup;
    if (role_ == pty_endpoint_role::master)
        return pair_->core().output_poll_events();
    return pair_->core().input_poll_events();
}

void pty_pair::close_master()
{
    const auto references = master_references_.load();
    if (references == 0)
        return;
    if (master_references_.fetch_sub(1) == 1)
    {
        master_open_.store(false);
        core_.hangup_master();
    }
}

void pty_pair::close_slave()
{
    const auto references = slave_references_.load();
    if (references == 0)
        return;
    if (slave_references_.fetch_sub(1) == 1)
    {
        slave_open_.store(false);
        core_.hangup_slave();
    }
}

bool pty_pair::activate_slave() { return open_slave_reference(); }

bool pty_pair::open_master_reference()
{
    if (core_.master_hung_up())
        return false;
    master_references_.fetch_add(1);
    master_open_.store(true);
    return true;
}

bool pty_pair::open_slave_reference()
{
    if (slave_locked_.load() || core_.slave_hung_up())
        return false;
    slave_references_.fetch_add(1);
    slave_open_.store(true);
    return true;
}

} // namespace dev::tty
