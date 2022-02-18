#include "kernel/task.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/fs/vfs/file.hpp"
#include "kernel/fs/vfs/vfs.hpp"
#include "kernel/syscall.hpp"

namespace naos::syscall
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
void sleep(timeclock::millisecond_t milliseconds) { task::do_sleep(milliseconds); }

struct sig_info_t
{
    u64 error;
    u64 code;
    u64 status;
    u64 tid;
    u64 pid;
};

i64 raise(task::signal_num_t num, sig_info_t *info)
{
    if (info != nullptr && !is_user_space_pointer(info))
        return EPARAM;
    auto proc = task::current_process();

    if (info)
        proc->signal_pack.send(proc, num, info->error, info->code, info->status);
    else
        proc->signal_pack.send(proc, num, 0, 0, 0);

    return OK;
}

struct target_t
{
    u64 id;
    flag_t flags;
};

enum target_flags : flag_t
{
    send_to_process = 1,
    send_to_group = 2,
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
        if (info)
            proc->signal_pack.send(proc, num, info->error, info->code, info->status);
        else
            proc->signal_pack.send(proc, num, 0, 0, 0);
    }
    else if (target->flags & send_to_group)
    {
        /// TODO: send signal to group
    }

    return OK;
}

u64 sigwait(task::signal_num_t *num, sig_info_t *info)
{
    if (num != nullptr && !is_user_space_pointer(num))
        return EPARAM;
    if (info != nullptr && !is_user_space_pointer(info))
        return EPARAM;
    task::signal_info_t inf;
    task::current_process()->signal_pack.wait(&inf);
    if (num)
        *num = inf.number;

    if (info)
    {
        info->code = inf.code;
        info->error = inf.error;
        info->status = inf.status;
        info->pid = inf.pid;
        info->tid = inf.tid;
    }
    return OK;
}

#define SIGOPT_GET 1
#define SIGOPT_SET 2
#define SIGOPT_OR 3
#define SIGOPT_AND 4
#define SIGOPT_XOR 5
#define SIGOPT_INVALID_ALL 6

i64 sigmask(int opt, u64 *valid_mask, u64 *block_mask, u64 *ignore_mask)
{
    if (valid_mask && !is_user_space_pointer(valid_mask))
        return EFAILED;
    if (block_mask && !is_user_space_pointer(block_mask))
        return EFAILED;
    if (ignore_mask && !is_user_space_pointer(ignore_mask))
        return EFAILED;
    auto &sigpack = task::current_process()->signal_pack;

    auto &valid = sigpack.get_mask().get_valid_set();
    auto &block = sigpack.get_mask().get_block_set();
    auto &ignore = sigpack.get_mask().get_ignore_set();

    if (opt == SIGOPT_GET)
    {
        if (valid_mask)
            *valid_mask = valid.get();
        if (block_mask)
            *block_mask = block.get();
        if (ignore_mask)
            *ignore_mask = ignore.get();
    }
    else if (opt == SIGOPT_SET)
    {
        if (valid_mask)
            valid.set(*valid_mask);
        if (block_mask)
            block.set(*block_mask);
        if (ignore_mask)
            ignore.set(*ignore_mask);
    }
    else if (opt == SIGOPT_OR)
    {
        if (valid_mask)
            valid.set(*valid_mask | valid.get());
        if (block_mask)
            block.set(*block_mask | block.get());
        if (ignore_mask)
            ignore.set(*ignore_mask | ignore.get());
    }
    else if (opt == SIGOPT_AND)
    {
        if (valid_mask)
            valid.set(*valid_mask & valid.get());
        if (block_mask)
            block.set(*block_mask & block.get());
        if (ignore_mask)
            ignore.set(*ignore_mask & ignore.get());
    }
    else if (opt == SIGOPT_XOR)
    {
        if (valid_mask)
            valid.set(*valid_mask ^ valid.get());
        if (block_mask)
            block.set(*block_mask ^ block.get());
        if (ignore_mask)
            ignore.set(*ignore_mask ^ ignore.get());
    }
    else if (opt == SIGOPT_INVALID_ALL)
    {
        valid.set(0);
    }
    else
    {
        return EPARAM;
    }
    return OK;
}

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
SYSCALL(40, raise)
SYSCALL(41, sigsend)
SYSCALL(42, sigwait)
SYSCALL(43, sigmask)

SYSCALL(47, getcpu_running)
SYSCALL(48, setcpu_mask)
SYSCALL(49, getcpu_mask)
END_SYSCALL

} // namespace syscall
