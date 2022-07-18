#include "kernel/arch/klib.hpp"
#include "kernel/clock.hpp"
#include "kernel/fs/vfs/file.hpp"
#include "kernel/syscall.hpp"
#include "kernel/task.hpp"
#include "kernel/time.hpp"
#include "kernel/util/str.hpp"
namespace naos::syscall
{
void log(const char *message)
{
    auto &res = task::current_process()->res_table;
    auto file = res.get_file(2);
    if (file)
    {
        file->write(reinterpret_cast<const byte *>(message), util::strlen(message), 0);
    }
}

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