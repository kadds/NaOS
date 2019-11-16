#include "kernel/task.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/fs/vfs/file.hpp"
#include "kernel/fs/vfs/vfs.hpp"
#include "kernel/syscall.hpp"

namespace syscall
{

/// exit process with return value
void exit(u64 ret_value) { task::do_exit(ret_value); }

thread_id current_tid() { return task::current()->tid; }

process_id current_pid() { return task::current_process()->pid; }

void user_process_thread(u64 arg0, u64 arg1, u64 arg2, u64 arg3)
{
    arch::task::enter_userland(task::current(), (void *)arg3, arg1);
}

process_id create_process(const char *filename, const char *args, u64 flags)
{
    if (!is_user_space_pointer(filename))
    {
        return -1;
    }
    if (!is_user_space_pointer(args))
    {
        return -1;
    }

    auto file = fs::vfs::open(filename, task::current_process()->res_table.get_file_table()->get_path_root(filename),
                              fs::mode::read | fs::mode::bin, flags);
    if (!file)
    {
        return -2;
    }
    auto p = task::create_process(file, user_process_thread, 0, args, 0, 0);
    if (p)
    {
        return p->pid;
    }
    return -1;
}

void user_thread(u64 arg0, u64 arg1, u64 arg2, u64 arg3)
{
    arch::task::enter_userland(task::current(), (void *)arg1, arg0);
}

thread_id create_thread(void *entry, u64 arg, u64 flags)
{
    if (!is_user_space_pointer(entry))
    {
        return -1;
    }

    auto t = task::create_thread(task::current_process(), user_thread, arg, (u64)entry, 0, flags);
    if (t)
    {
        return t->tid;
    }
    return -1;
}

void exit_thread(u64 ret)
{
    if (task::current()->attributes & task::thread_attributes::main)
    {
        exit(ret);
    }
    else
    {
        task::exit_thread(ret);
    }
}

int detach(thread_id tid) { return task::detach_thread(task::find_tid(task::current_process(), tid)); }

int join(thread_id tid, u64 *ret)
{
    u64 r;
    auto rt = task::join_thread(task::find_tid(task::current_process(), tid), r);
    if (ret != nullptr && is_user_space_pointer(ret))
    {
        *ret = r;
    }
    return rt;
}

long wait_process(process_id pid, u64 *ret)
{
    u64 r;
    u64 retv = task::wait_process(task::find_pid(pid), r);
    if (ret != nullptr && is_user_space_pointer(ret))
    {
        *ret = r;
    }
    return retv;
}

/// sleep current thread
void sleep(time::millisecond_t milliseconds) { task::do_sleep(milliseconds); }

BEGIN_SYSCALL
SYSCALL(30, exit)
SYSCALL(31, sleep)
SYSCALL(32, current_tid)
SYSCALL(33, current_pid)
SYSCALL(34, create_process)
SYSCALL(35, create_thread)
SYSCALL(36, detach)
SYSCALL(37, join)
SYSCALL(38, wait_process)
SYSCALL(39, exit_thread)
END_SYSCALL

} // namespace syscall
