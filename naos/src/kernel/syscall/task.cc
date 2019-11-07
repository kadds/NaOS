#include "kernel/syscall.hpp"

namespace syscall
{

/// exit thread with return value
void exit(u64 ret_value) { task::do_exit(ret_value); }

void new_process(const char *filename, const char *args, u64 flags) {}

/// sleep current thread
void sleep(u64 milliseconds) { task::do_sleep(milliseconds); }

u64 brk(u64 offset, u64 mode) { return 0; }

BEGIN_SYSCALL
SYSCALL(30, exit)
SYSCALL(31, sleep)
SYSCALL(34, new_process)
END_SYSCALL

} // namespace syscall
