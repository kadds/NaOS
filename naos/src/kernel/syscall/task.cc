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

process_id create_process(const char *filename, const char *args, flag_t flags)
{
    if (!is_user_space_pointer(filename))
    {
        return -1;
    }
    if (!is_user_space_pointer(args))
    {
        return -1;
    }
    auto ft = task::current_process()->res_table.get_file_table();
    auto file =
        fs::vfs::open(filename, ft->root, ft->current, fs::mode::read | fs::mode::bin, fs::path_walk_flags::file);
    if (!file)
    {
        return -2;
    }
    auto p = task::create_process(file, user_process_thread, 0, args, 0, flags);
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

thread_id create_thread(void *entry, u64 arg, flag_t flags)
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

int sigaction(task::signal_num_t num, task::signal_func_t handler, u64 mask, flag_t flags)
{
    if (!is_user_space_pointer(handler))
        return -1;
    task::current_process()->signal_actions->set_action(num, handler, mask, flags);
    return 0;
}

int raise(task::signal_num_t num, u64 error, u64 code, u64 status)
{
    task::current()->signal_pack.set(num, error, code, status);
    return 0;
}

int sigsend(thread_id tid, task::signal_num_t num, u64 error, u64 code, u64 status)
{
    auto t = task::find_tid(task::current_process(), tid);
    if (t == nullptr)
        return -2;
    t->signal_pack.set(num, error, code, status);
    return 0;
}

struct target
{
    process_id pid;
    thread_id tid;
    group_id gid;
    flag_t flags;
};

enum target_flags : flag_t
{
    send_to_group = 1,
    send_to_process = 2,
    send_to_thread = 4,
};

int sigput(target *target, task::signal_num_t num, u64 error, u64 code, u64 status)
{
    if (!is_user_space_pointer(target))
        return -1;
    if (target->flags & send_to_process)
    {
        auto proc = task::find_pid(target->pid);
        if (proc == nullptr)
            return -2;
    }
    else if (target->flags & send_to_thread)
    {
        auto proc = task::find_pid(target->pid);
        if (proc == nullptr)
            proc = task::current_process();
    }
    else if (target->flags & send_to_group)
    {
        /// TODO: send group
    }

    return 0;
}

void sigreturn(u64 code) { task::signal_return(code); }

u64 getcpu_running() { return task::current()->cpuid; }

void setcpu_mask(u64 mask0, u64 mask1)
{
    task::current()->cpumask.mask = mask0;
    task::current()->attributes |= task::thread_attributes::need_schedule;
}

void getcpu_mask(u64 *mask0, u64 *mask1)
{
    if (!is_user_space_pointer(mask0))
        return;
    *mask0 = task::current()->cpumask.mask;
}

BEGIN_SYSCALL
SYSCALL(30, exit)
SYSCALL(31, sleep)
SYSCALL(32, current_pid)
SYSCALL(33, current_tid)
SYSCALL(34, create_process)
SYSCALL(35, create_thread)
SYSCALL(36, detach)
SYSCALL(37, join)
SYSCALL(38, wait_process)
SYSCALL(39, exit_thread)
SYSCALL(40, sigaction)
SYSCALL(41, raise)
SYSCALL(42, sigsend)
SYSCALL(43, sigput)
SYSCALL(44, sigreturn)
SYSCALL(45, getcpu_running)
SYSCALL(46, setcpu_mask)
SYSCALL(47, getcpu_mask)
END_SYSCALL

} // namespace syscall
