#include "kernel/syscall.hpp"
#include "kernel/fs/vfs/vfs.hpp"
#include "kernel/task.hpp"
#include "kernel/trace.hpp"

namespace syscall
{
/// none system call, just print a warning
u64 none()
{
    trace::warning("This system call isn't implement!");
    return 1;
}

void *system_call_table[128];

BEGIN_SYSCALL
SYSCALL(0, none)
SYSCALL(1, none)

END_SYSCALL

} // namespace syscall
