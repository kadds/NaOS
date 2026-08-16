#include "kernel/arch/klib.hpp"
#include "kernel/clock.hpp"
#include "kernel/errno.hpp"
#include "kernel/fs/vfs/file.hpp"
#include "kernel/kobject.hpp"
#include "kernel/log.hpp"
#include "kernel/syscall.hpp"
#include "kernel/task.hpp"
#include "kernel/time.hpp"
#include "kernel/timer.hpp"
#include "kernel/usercopy.hpp"

namespace naos::syscall
{
namespace
{
constexpr int clock_realtime = 0;
constexpr int clock_monotonic = 1;
} // namespace

void log(const char *message)
{
    if (message == nullptr)
        return;

    const char *process_name = "process";
    if (task::has_init())
    {
        auto *process = task::current_process();
        if (process != nullptr && process->name[0] != '\0')
            process_name = process->name;
    }

    const auto emit = [process_name](const char *value, u64 length) {
        log::detail::emit_user_message(process_name, __FILE__, __LINE__, true, value, length);
    };

    char buffer[128];
    bool ends_with_newline = false;
    for (u64 index = 0; index < 4096; index++)
    {
        char value = 0;
        if (naos::usercopy::copy_from(&value, reinterpret_cast<u64>(message) + index, sizeof(value)) != NA_STATUS_OK)
        {
            static constexpr char invalid_log[] = "<invalid user log>";
            emit(invalid_log, sizeof(invalid_log) - 1);
            break;
        }
        buffer[index % (sizeof(buffer) - 1)] = value;
        if (value == '\0')
        {
            buffer[index % (sizeof(buffer) - 1)] = '\0';
            emit(buffer, index % (sizeof(buffer) - 1));
            break;
        }
        ends_with_newline = value == '\n';
        if (index % (sizeof(buffer) - 1) == sizeof(buffer) - 2)
        {
            buffer[sizeof(buffer) - 1] = '\0';
            emit(buffer, sizeof(buffer) - 1);
        }
    }
    if (!ends_with_newline)
    {
        static constexpr char newline[] = "\n";
        emit(newline, sizeof(newline) - 1);
    }
}

int clock_get(int clock_index, timeclock::time *time)
{
    if (!is_user_space_range(time, sizeof(*time)))
    {
        return EPARAM;
    }
    if (clock_index != clock_realtime && clock_index != clock_monotonic)
        return EINVAL;

    timeclock::time value(0, 0);
    const auto us = clock_index == clock_monotonic ? timer::get_high_resolution_time() : timeclock::get_current_clock();
    value.tv_nsec = static_cast<int64_t>(us % 1'000'000) * 1000;
    value.tv_sec = us / 1000000;
    return naos::usercopy::copy_to(reinterpret_cast<u64>(time), &value, sizeof(value)) == NA_STATUS_OK ? 0 : EFAULT;
}

BEGIN_SYSCALL
SYSCALL(NA_SYSCALL_LOG, log)
SYSCALL(NA_SYSCALL_CLOCK_GET, clock_get)
END_SYSCALL
} // namespace naos::syscall
