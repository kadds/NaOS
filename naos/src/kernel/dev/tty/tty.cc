#include "kernel/dev/tty/tty.hpp"

#include "freelibcxx/string.hpp"
#include "kernel/errno.hpp"
#include "kernel/fs/vfs/defines.hpp"
#include "kernel/task.hpp"
#include "kernel/terminal.hpp"

namespace dev::tty
{
using namespace fs;

bool tty_read_func(u64 data)
{
    auto *tty = reinterpret_cast<tty_pseudo_t *>(data);
    return tty != nullptr && tty->readable();
}

i64 tty_pseudo_t::write(const byte *data, u64 size, flag_t flags)
{
    const auto result = core_.write_output(data, size, flags);
    render_master_output();
    return result;
}

i64 tty_pseudo_t::read(byte *data, u64 max_size, flag_t flags) { return core_.read_input(data, max_size, flags); }

i64 tty_pseudo_t::write_to_buffer(const byte *data, u64 size, flag_t flags)
{
    const auto result = master_.write(data, size, flags);
    render_master_output();
    return result;
}

void tty_pseudo_t::send_EOF()
{
    core_.send_eof();
    render_master_output();
}

bool tty_pseudo_t::native_tty_get_attributes(termios_t &attributes)
{
    attributes = core_.get_termios();
    return true;
}

bool tty_pseudo_t::native_tty_set_attributes(const termios_t &attributes)
{
    core_.set_termios(attributes);
    return true;
}

bool tty_pseudo_t::native_tty_get_winsize(winsize_t &size)
{
    size = core_.get_winsize();
    return true;
}

bool tty_pseudo_t::native_tty_set_winsize(const winsize_t &size)
{
    core_.set_winsize(size);
    return true;
}

i64 tty_pseudo_t::native_tty_flush(i32 queue)
{
    if (queue == tty_flush::input || queue == tty_flush::both)
        core_.flush_input();
    if (queue == tty_flush::output || queue == tty_flush::both)
        core_.flush_output();
    return queue >= tty_flush::input && queue <= tty_flush::both ? 0 : EINVAL;
}

i64 tty_pseudo_t::native_tty_attach(bool force)
{
    auto *process = task::current_process();
    const auto result = task::attach_controlling_tty(process, &core_, force);
    if (result == 0 && process != nullptr)
    {
        core_.set_session_id(process->session_id);
        core_.set_foreground_process_group(process->process_group_id);
    }
    return result;
}

i64 tty_pseudo_t::native_tty_get_pgrp(u32 &group)
{
    group = static_cast<u32>(core_.foreground_process_group());
    return 0;
}

i64 tty_pseudo_t::native_tty_set_pgrp(u32 group)
{
    return task::set_foreground_process_group(task::current_process(), &core_, static_cast<group_id>(group));
}

i64 tty_pseudo_t::native_tty_get_sid(u32 &session)
{
    session = static_cast<u32>(core_.session_id());
    return 0;
}

i64 tty_pseudo_t::native_tty_detach()
{
    auto *process = task::current_process();
    task::detach_controlling_tty(process);
    if (process != nullptr && process->pid == process->session_id && process->controlling_tty == nullptr)
    {
        core_.set_foreground_process_group(0);
        core_.set_session_id(0);
    }
    return 0;
}

i64 tty_pseudo_t::native_tty_get_input(u64 &count)
{
    count = readable() ? 1 : 0;
    return 0;
}

void tty_pseudo_t::render_master_output()
{
    byte buffer[128];
    tty_output_source sources[128];
    while (true)
    {
        const auto count = master_.read(buffer, sizeof(buffer), fs::rw_flags::no_block, sources);
        if (count <= 0)
            break;

        u64 offset = 0;
        while (offset < static_cast<u64>(count))
        {
            const auto source = sources[offset];
            u64 end = offset + 1;
            while (end < static_cast<u64>(count) && sources[end] == source)
                end++;

            term::write_to(freelibcxx::const_string_view(reinterpret_cast<const char *>(buffer + offset), end - offset),
                           term_index_);
            bool ends_line = false;
            for (u64 index = offset; index < end; index++)
            {
                if (static_cast<u8>(buffer[index]) == '\n')
                {
                    ends_line = true;
                    break;
                }
            }
            if (source == tty_output_source::normal || ends_line)
                term::commit_changes(term_index_);
            offset = end;
        }
    }

    // Console output must become visible before the syscall returns. The
    // periodic terminal flush remains useful for kernel-side changes, but it
    // is not a reliable completion boundary while the scheduler is busy
    // during process startup.
    term::flush_active_terminal();
}

// The framebuffer console is a persistent device node. Closing the temporary
// setup handle (or the last inherited fd) must not permanently hang up the
// console core; PTY endpoints own the actual hangup transition.
void tty_pseudo_t::close() {}

} // namespace dev::tty
