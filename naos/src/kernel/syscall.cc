#include "kernel/syscall.hpp"
#include "kernel/fs/vfs/vfs.hpp"
#include "kernel/task.hpp"
#include "kernel/trace.hpp"

namespace naos::syscall
{
/// none system call, just print a warning
u64 none()
{
    trace::warning("This system call isn't implement!");
    return 1;
}

void *system_call_table[256];

BEGIN_SYSCALL
SYSCALL(NA_SYSCALL_NONE, none)
END_SYSCALL

} // namespace naos::syscall
