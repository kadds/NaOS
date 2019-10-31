#include "kernel/syscall.hpp"
#include "kernel/fs/vfs/vfs.hpp"
#include "kernel/task.hpp"
#include "kernel/trace.hpp"

#define SYSCALL(name) ((void *)&name),

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

/// exit thread with return value
void exit(u64 ret_value) { task::do_exit(ret_value); }

void fork_kernel_thread(u64 args) {}

u64 fork(u64 flags, u64 args) { return task::do_fork(fork_kernel_thread, args, flags); }

void exec(const char *filename, const char *args, u64 flags) { task::do_exec(filename, 0, 0, flags); }

void new_process(const char *filename, const char *args, u64 flags) {}

/// sleep current thread
void sleep(u64 milliseconds) { task::do_sleep(milliseconds); }

task::file_desc open(const char *filename, u64 mode) { return 0; }

bool close(task::file_desc file_desc) { return 0; }

u64 write(task::file_desc file_desc, byte *buffer, u64 max_len) { return 0; }

u64 read(task::file_desc file_desc, byte *buffer, u64 max_len) { return 0; }

u64 lseek(task::file_desc file_desc, u64 offset, u64 type) { return 0; }

u64 brk(u64 offset, u64 mode) { return 0; }

void *system_call_table[] = {SYSCALL(none) SYSCALL(print) SYSCALL(exit) SYSCALL(fork) SYSCALL(exec) SYSCALL(sleep)};

} // namespace syscall
