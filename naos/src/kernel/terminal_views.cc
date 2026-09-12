#include "kernel/terminal_views.hpp"

#include "kernel/errno.hpp"
#include "kernel/terminal.hpp"
#include "kernel/task.hpp"

namespace dev::tty
{
i64 console_stream::read(byte *data, u64 size)
{
    (void)data;
    (void)size;
    return 0;
}

i64 console_stream::write(const byte *data, u64 size)
{
    if (data == nullptr)
        return -EINVAL;
    term::write_to(freelibcxx::const_string_view(reinterpret_cast<const char *>(data), size), terminal_index_);
    return static_cast<i64>(size);
}

void terminal_driver_control::on_capability_acquire(capability::location where)
{
    (void)where;
    capability_references_.fetch_add(1);
}

void terminal_driver_control::release_capability()
{
    auto *identity = identity_.operator&();
    group_id foreground = 0;
    if (identity == nullptr || !identity->revoke_and_take_foreground(&foreground))
        return;

    if (foreground > 0)
    {
        (void)task::send_signal_to_process_group(static_cast<group_id>(foreground), task::signal::sighup);
        (void)task::send_signal_to_process_group(static_cast<group_id>(foreground), task::signal::sigcont);
    }
    task::detach_session_terminal(identity);
}

void terminal_driver_control::on_capability_release(capability::location where)
{
    (void)where;
    const auto previous = capability_references_.fetch_sub(1);
    if (previous == 1)
        release_capability();
}

void terminal_driver_control::on_capability_handoff(capability::location from, capability::location to)
{
    (void)from;
    (void)to;
    on_capability_release(from);
}
} // namespace dev::tty
