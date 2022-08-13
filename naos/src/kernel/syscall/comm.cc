#include "kernel/arch/klib.hpp"
#include "kernel/clock.hpp"
#include "kernel/errno.hpp"
#include "kernel/fs/vfs/file.hpp"
#include "kernel/kobject.hpp"
#include "kernel/syscall.hpp"
#include "kernel/task.hpp"
#include "kernel/time.hpp"
namespace naos::syscall
{
void log(const char *message)
{
#ifdef _DEBUG
    trace::print(message);
    trace::print("\n");
#endif
}

int clock_get(int clock_index, timeclock::time *time)
{
    if (!is_user_space_pointer_or_null(time))
    {
        return EPARAM;
    }

    auto us = timeclock::get_current_clock();
    time->tv_nsec = us / 1000 % 1000;
    time->tv_sec = us / 1000000;
    return 0;
}

BEGIN_SYSCALL
SYSCALL(1, log)
SYSCALL(2, clock_get)
END_SYSCALL
} // namespace naos::syscall