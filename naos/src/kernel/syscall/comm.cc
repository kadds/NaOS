#include "kernel/arch/klib.hpp"
#include "kernel/clock.hpp"
#include "kernel/fs/vfs/file.hpp"
#include "kernel/kobject.hpp"
#include "kernel/syscall.hpp"
#include "kernel/task.hpp"
#include "kernel/time.hpp"
#include "kernel/util/str.hpp"
namespace naos::syscall
{
void log(const char *message) { trace::print(message); }

int clock_get(int clock_index, timeclock::time *time)
{
    *time = timeclock::to_time(timeclock::get_current_clock());
    return 0;
}

BEGIN_SYSCALL
SYSCALL(1, log)
SYSCALL(2, clock_get)
END_SYSCALL
} // namespace naos::syscall