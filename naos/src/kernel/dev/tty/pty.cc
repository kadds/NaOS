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
            signal_number = task::signal::sigstop;
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
    return pair_->core().read_input(data, max_size, flags);
}

i64 pty_endpoint::write(const byte *data, u64 size, flag_t flags)
{
    if (!can_operate())
        return EIO;
    if (role_ == pty_endpoint_role::master)
        return pair_->core().receive_input(data, size, flags);
    return pair_->core().write_output(data, size, flags);
}

i64 pty_endpoint::ioctl(fs::vfs::ioctl_context &context)
{
    if (!can_operate())
        return EIO;

    switch (context.request())
    {
        case tty_ioctl::tiocgptn:
            if (role_ != pty_endpoint_role::master)
                return ENOTTY;
            {
                const u32 index = pair_->index();
                return context.write_user(index);
            }
        case tty_ioctl::tiocsptlck:
            if (role_ != pty_endpoint_role::master)
                return ENOTTY;
            {
                u32 locked;
                const auto result = context.read_user(locked);
                if (result != OK)
                    return result;
                pair_->set_slave_locked(locked != 0);
            }
            return 0;
        case tty_ioctl::tiocgptlck:
            if (role_ != pty_endpoint_role::master)
                return ENOTTY;
            {
                const u32 locked = pair_->slave_locked() ? 1 : 0;
                return context.write_user(locked);
            }
        case tty_ioctl::tiocsctty: {
            if (role_ != pty_endpoint_role::slave)
                return ENOTTY;
            auto *process = task::current_process();
            const auto result = task::attach_controlling_tty(process, &pair_->core(), context.value() != 0);
            if (result == 0 && process != nullptr)
            {
                pair_->core().set_session_id(process->session_id);
                pair_->core().set_foreground_process_group(process->process_group_id);
            }
            return result;
        }
        case tty_ioctl::tiocnotty: {
            if (role_ != pty_endpoint_role::slave)
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
        case tty_ioctl::tiocgpgrp: {
            const u32 group = static_cast<u32>(pair_->core().foreground_process_group());
            return context.write_user(group);
        }
        case tty_ioctl::tiocspgrp: {
            u32 pgid;
            const auto result = context.read_user(pgid);
            if (result != OK)
                return result;
            return task::set_foreground_process_group(task::current_process(), &pair_->core(),
                                                      static_cast<group_id>(pgid));
        }
        case tty_ioctl::tiocgsid: {
            const u32 session = static_cast<u32>(pair_->core().session_id());
            return context.write_user(session);
        }
        default:
            return pair_->core().ioctl(context);
    }
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
