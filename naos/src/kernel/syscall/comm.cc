#include "kernel/arch/klib.hpp"
#include "kernel/clock.hpp"
#include "kernel/errno.hpp"
#include "kernel/fs/vfs/file.hpp"
#include "kernel/kobject.hpp"
#include "kernel/syscall.hpp"
#include "kernel/task.hpp"
#include "kernel/time.hpp"
#include "kernel/usercopy.hpp"
namespace naos::syscall
{
void log(const char *message)
{
#ifdef _DEBUG
    if (message == nullptr)
        return;

    char buffer[128];
    for (u64 index = 0; index < 4096; index++)
    {
        char value = 0;
        if (naos::usercopy::copy_from(&value, reinterpret_cast<u64>(message) + index, sizeof(value)) != NA_STATUS_OK)
        {
            trace::print("<invalid user log>");
            break;
        }
        buffer[index % (sizeof(buffer) - 1)] = value;
        if (value == '\0')
        {
            buffer[index % (sizeof(buffer) - 1)] = '\0';
            trace::print(buffer);
            break;
        }
        if (index % (sizeof(buffer) - 1) == sizeof(buffer) - 2)
        {
            buffer[sizeof(buffer) - 1] = '\0';
            trace::print(buffer);
        }
    }
    trace::print("\n");
#endif
}

int clock_get(int clock_index, timeclock::time *time)
{
    if (!is_user_space_range(time, sizeof(*time)))
    {
        return EPARAM;
    }

    timeclock::time value(0, 0);
    auto us = timeclock::get_current_clock();
    value.tv_nsec = us / 1000 % 1000;
    value.tv_sec = us / 1000000;
    return naos::usercopy::copy_to(reinterpret_cast<u64>(time), &value, sizeof(value)) == NA_STATUS_OK ? 0 : EFAULT;
}

BEGIN_SYSCALL
SYSCALL(NA_SYSCALL_LOG, log)
SYSCALL(NA_SYSCALL_CLOCK_GET, clock_get)
END_SYSCALL
} // namespace naos::syscall
