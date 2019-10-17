#include "kernel/syscall.hpp"
#include "kernel/task.hpp"

#define SYSCALL(name) ((void *)&name),

namespace syscall
{
u64 none()
{
    trace::info("This system call isn't implement!");
    return 1;
}

void print(const char *str)
{ // trace::print(str);
}

void exit(u64 ret_value) { task::do_exit(ret_value); }

void fork_subtask(u64 args) {}

u64 fork(u64 flags, u64 args) { return task::do_fork(fork_subtask, args, flags); }

void exec(const char *filename, const char *args, u64 flags) { task::do_exec(filename, 0, 0, flags); }

void sleep(u64 milliseconds) { task::do_sleep(milliseconds); }

void *system_call_table[] = {SYSCALL(none) SYSCALL(print) SYSCALL(exit) SYSCALL(fork) SYSCALL(exec) SYSCALL(sleep)};

} // namespace syscall
