#include "kernel/syscall.hpp"
#include "kernel/log.hpp"
#include "kernel/task.hpp"

KLOG_MODULE(io);
namespace naos::syscall
{
/// none system call, just print a warning
u64 none()
{
    KLOG_WARN("This system call isn't implement!");
    return 1;
}

void *system_call_table[256];

BEGIN_SYSCALL
SYSCALL(NA_SYSCALL_NONE, none)
END_SYSCALL

} // namespace naos::syscall
