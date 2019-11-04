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

/// print a string to stdio, which can replace by 'write'
/// \param str the string to print
/// \param normal set true if don't need SGR format casting
void print(const char *str, bool normal)
{ // trace::print(str);
    if (!normal)
        trace::print_SGR(str, trace::kernel_console_attribute);
    else
        trace::print_inner(str, trace::kernel_console_attribute);
}

void *system_call_table[128];

BEGIN_SYSCALL
SYSCALL(0, none)
SYSCALL(1, print)
END_SYSCALL

} // namespace syscall
