#include "kernel/task.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/fs/vfs/file.hpp"
#include "kernel/fs/vfs/vfs.hpp"
#include "kernel/syscall.hpp"

namespace syscall
{

/// exit process with return value
void exit(i64 ret_value) { task::do_exit(ret_value); }

thread_id current_tid() { return task::current()->tid; }

process_id current_pid() { return task::current_process()->pid; }

void user_process_thread(u64 arg0, u64 arg1, u64 arg2, u64 arg3)
{
    /// the arg3 is entry address
    arch::task::enter_userland(task::current(), (void *)arg3, arg1, arg2);
}

process_id create_process(const char *filename, const char *argv[], flag_t flags)
{
    if (filename == nullptr || !is_user_space_pointer(filename))
    {
        return EPARAM;
    }
    if (!is_user_space_pointer(argv))
    {
        return EPARAM;
    }
    if (argv != nullptr)
    {
        int i = 0;
        while (argv[i] != nullptr)
        {
            if (!is_user_space_pointer(argv[i]))
                return EPARAM;
            i++;
            if (i > 255)
                return EPARAM;
        }
    }

    auto ft = task::current_process()->res_table.get_file_table();
    auto file =
        fs::vfs::open(filename, ft->root, ft->current, fs::mode::read | fs::mode::bin, fs::path_walk_flags::file);
    if (!file)
    {
        return ENOEXIST;
    }
    auto p = task::create_process(file, user_process_thread, argv, flags);
    if (p)
    {
        return p->pid;
    }
    return EFAILED;
}

void user_thread(u64 arg0, u64 arg1, u64 arg2, u64 arg3)
{
    arch::task::enter_userland(task::current(), (void *)arg1, arg0, 0);
}

thread_id create_thread(void *entry, u64 arg, flag_t flags)
{
    if (entry == nullptr || !is_user_space_pointer(entry))
    {
        return EPARAM;
    }

    auto t = task::create_thread(task::current_process(), user_thread, arg, (u64)entry, 0, flags);
    if (t)
    {
        return t->tid;
    }
    return EFAILED;
}

void exit_thread(i64 ret)
{
    if (task::current()->attributes & task::thread_attributes::main)
    {
        task::do_exit(ret);
    }
    else
    {
        task::do_exit_thread(ret);
    }
}

int detach(thread_id tid) { return task::detach_thread(task::find_tid(task::current_process(), tid)); }

int join(thread_id tid, i64 *ret)
{
    i64 r;
    auto rt = task::join_thread(task::find_tid(task::current_process(), tid), r);
    if (ret != nullptr && is_user_space_pointer(ret))
    {
        *ret = r;
    }
    return rt;
}

long wait_process(process_id pid, i64 *ret)
{
    i64 r;
    u64 retv = task::wait_process(task::find_pid(pid), r);
    if (ret != nullptr && is_user_space_pointer(ret))
    {
        *ret = r;
    }
    return retv;
}

/// sleep current thread
void sleep(time::millisecond_t milliseconds) { task::do_sleep(milliseconds); }

u64 sigaction(task::signal_num_t num, task::signal_func_t handler, u64 mask, flag_t flags)
{
    if (handler == nullptr || !is_user_space_pointer(handler))
        return EPARAM;
    task::current_process()->signal_actions->set_action(num, handler, mask, flags);
    return OK;
}

struct sig_info_t
{
    u64 error;
    u64 code;
    u64 status;
};

i64 raise(task::signal_num_t num, sig_info_t *info)
{
    if (info != nullptr && !is_user_space_pointer(info))
        return EPARAM;
    if (info)
        task::current()->signal_pack.set(num, info->error, info->code, info->status);
    else
        task::current()->signal_pack.set(num, 0, 0, 0);

    return OK;
}

struct target_t
{
    u64 id;
    flag_t flags;
};

enum target_flags : flag_t
{
    send_to_group = 1,
    send_to_process = 2,
};

u64 sigsend(target_t *target, task::signal_num_t num, sig_info_t *info)
{
    if (target == nullptr || !is_user_space_pointer(target))
        return EPARAM;
    if (info != nullptr && !is_user_space_pointer(info))
        return EPARAM;

    if (target->flags & send_to_process)
    {
        auto proc = task::find_pid(target->id);
        if (proc == nullptr)
            return ENOEXIST;
    }
    else if (target->flags & send_to_group)
    {
        /// TODO: send group
    }

    return OK;
}

u64 sigwait(task::signal_num_t *num, sig_info_t *info) {}

void sigreturn() { task::signal_return(); }

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
SYSCALL(43, sigwait)
SYSCALL(44, sigreturn)

SYSCALL(47, getcpu_running)
SYSCALL(48, setcpu_mask)
SYSCALL(49, getcpu_mask)
END_SYSCALL

} // namespace syscall
