#include "kernel/errno.hpp"
#include "kernel/fs/vfs/file.hpp"
#include "kernel/syscall.hpp"
#include "kernel/task.hpp"

namespace naos::syscall
{
namespace
{
struct pollfd_t
{
    file_desc fd;
    i16 events;
    i16 revents;
};

constexpr i16 poll_in = 0x001;
constexpr i16 poll_out = 0x004;
constexpr i16 poll_err = 0x008;
constexpr i16 poll_hup = 0x010;
constexpr i16 poll_nval = 0x020;
} // namespace

i64 poll(pollfd_t *fds, u64 count, i64 timeout)
{
    if (count != 0 && (fds == nullptr || count > 0x100000 || !is_user_space_range(fds, count * sizeof(pollfd_t))))
        return EBUFFER;

    (void)timeout;
    auto &resources = task::current_process()->resource;
    i64 ready = 0;
    for (u64 index = 0; index < count; index++)
    {
        auto &poll_fd = fds[index];
        poll_fd.revents = 0;
        auto object = resources.get_kobject(poll_fd.fd);
        auto *file = object ? object->get<fs::vfs::file>() : nullptr;
        auto *pseudo = file == nullptr ? nullptr : file->get_pseudo();
        if (pseudo == nullptr)
        {
            poll_fd.revents = poll_nval;
            ready++;
            continue;
        }

        const u32 available = pseudo->poll_events();
        if ((poll_fd.events & poll_in) && (available & 0x001))
            poll_fd.revents |= poll_in;
        if ((poll_fd.events & poll_out) && (available & 0x004))
            poll_fd.revents |= poll_out;
        if (available & 0x008)
            poll_fd.revents |= poll_err;
        if (available & 0x010)
            poll_fd.revents |= poll_hup;
        if (poll_fd.revents != 0)
            ready++;
    }
    return ready;
}

i64 setsid() { return task::setsid(task::current_process()); }

i64 getpgid(i64 pid) { return task::getpgid(task::current_process(), static_cast<process_id>(pid)); }

i64 setpgid(i64 pid, i64 pgid)
{
    return task::setpgid(task::current_process(), static_cast<process_id>(pid), static_cast<group_id>(pgid));
}

i64 getsid(i64 pid) { return task::getsid(task::current_process(), static_cast<process_id>(pid)); }

BEGIN_SYSCALL
SYSCALL(68, poll)
SYSCALL(69, setsid)
SYSCALL(70, getpgid)
SYSCALL(71, setpgid)
SYSCALL(72, getsid)
END_SYSCALL
} // namespace naos::syscall
