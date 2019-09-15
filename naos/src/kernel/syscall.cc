#include "kernel/syscall.hpp"
namespace syscall
{
u64 none()
{
    trace::info("This system call isn't implement!");
    return 1;
}
void print(const char *str) { trace::print(str); }

void *system_call_table[] = {(void *)&none, (void *)print};

} // namespace syscall
