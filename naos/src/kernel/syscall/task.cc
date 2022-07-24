#include "kernel/task.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/errno.hpp"
#include "kernel/fs/vfs/file.hpp"
#include "kernel/fs/vfs/vfs.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/syscall.hpp"
#include "kernel/time.hpp"
#include <atomic>

namespace naos::syscall
{

enum futex_op
{
    futex_wake = 1,
    futex_wait = 2,
};

int futex(int *ptr, int op, int val, const timeclock::time *timeout, int val2)
{
    if (op & futex_op::futex_wait)
    {
        return 0;
    }
    else if (op & futex_op::futex_wake)
    {
        return 0;
    }
    else
    {
        return EPARAM;
    }
}

/// exit process with return value
void exit(i64 ret_value) { task::do_exit(ret_value); }

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

thread_id current_tid() { return task::current()->tid; }

process_id current_pid() { return task::current_process()->pid; }

void before_user_thread(task::thread_start_info_t *info)
{
    u64 offset = info->userland_stack_offset;
    void *entry = info->userland_entry;
    u64 args = reinterpret_cast<u64>(info->args);
    memory::Delete(memory::KernelCommonAllocatorV, info);

    arch::task::enter_userland(task::current(), offset, entry, args, 0);
}

process_id create_process(const char *path, const char *argv[], const char *envp[], flag_t flags)
{
    if (!is_user_space_pointer_or_null(path))
    {
        return EPARAM;
    }
    if (!is_user_space_pointer_or_null(argv))
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

    auto &res = task::current_process()->resource;
    auto file =
        fs::vfs::open(path, res.root(), res.current(), fs::mode::read | fs::mode::bin, fs::path_walk_flags::file);
    if (!file)
    {
        return ENOEXIST;
    }
    auto p = task::create_process(file, path, before_user_thread, argv, envp, flags);
    if (p)
    {
        return p->pid;
    }
    return EFAILED;
}

thread_id create_thread(void *entry, void *arg, flag_t flags)
{
    if (!is_user_space_pointer_or_null(entry))
    {
        return EPARAM;
    }

    auto t = task::create_thread(task::current_process(), before_user_thread, entry, arg, flags);
    if (t)
    {
        return t->tid;
    }
    return EFAILED;
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
void sleep(const timeclock::time *time) { task::do_sleep(*time); }

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
    if (!is_user_space_pointer(info))
    {
        return EPARAM;
    }
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
    if (!is_user_space_pointer_or_null(target))
        return EPARAM;
    if (!is_user_space_pointer(info))
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
    if (!is_user_space_pointer(num))
        return EPARAM;
    if (!is_user_space_pointer(info))
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

int getcpu_mask(u64 *mask0, u64 *mask1)
{
    if (!is_user_space_pointer_or_null(mask0))
        return EPARAM;
    *mask0 = task::current()->cpumask.mask;
    return 0;
}

int set_tcb(void *p)
{
    if (!is_user_space_pointer(p))
        return EPARAM;
    auto t = task::current();
    task::set_tcb(t, p);

    return 0;
}

int chdir(const char *path)
{
    if (!is_user_space_pointer_or_null(path))
    {
        return EPARAM;
    }
    auto &res = task::current_process()->resource;
    auto entry = fs::vfs::path_walk(path, res.root(), res.current(), fs::path_walk_flags::directory);
    if (entry != nullptr)
    {
        res.set_current(entry);
        return OK;
    }
    return EFAILED;
}

int current_dir(char *path, u64 max_len)
{
    if (!is_user_space_pointer_or_null(path) || !is_user_space_pointer_or_null(path + max_len))
    {
        return EBUFFER;
    }

    auto &res = task::current_process()->resource;
    return fs::vfs::pathname(res.root(), res.current(), path, max_len);
}

int chroot(const char *path)
{
    if (!is_user_space_pointer_or_null(path))
    {
        return EPARAM;
    }
    auto &res = task::current_process()->resource;

    auto entry = fs::vfs::path_walk(path, res.root(), res.current(),
                                    fs::path_walk_flags::directory | fs::path_walk_flags::cross_root);
    if (entry != nullptr)
    {
        res.set_root(entry);
        return OK;
    }
    return EFAILED;
}

int fork() { return task::fork(); }

int clone(void *entry, void *arg, void *tcb)
{
    if (!is_user_space_pointer_or_null(entry))
    {
        return EPARAM;
    }

    auto t = task::create_thread(task::current_process(), before_user_thread, entry, arg, 0);
    if (t)
    {
        return t->tid;
    }
    return EFAILED;
}

int execve(const char *path, char *const argv[], char *const envp[])
{
    if (!is_user_space_pointer_or_null(argv))
    {
        return EPARAM;
    }
    if (!is_user_space_pointer(envp))
    {
        return EPARAM;
    }
    auto &res = task::current_process()->resource;
    auto file =
        fs::vfs::open(path, res.root(), res.current(), fs::mode::read | fs::mode::bin, fs::path_walk_flags::file);
    if (!file)
    {
        return EACCES;
    }
    return task::execve(file, path, before_user_thread, argv, envp);
}
int yield() { return 0; }

BEGIN_SYSCALL

SYSCALL(31, futex)
SYSCALL(32, exit)
SYSCALL(33, exit_thread)
SYSCALL(34, sleep)
SYSCALL(35, current_pid)
SYSCALL(36, current_tid)
SYSCALL(37, create_process)
SYSCALL(38, create_thread)
SYSCALL(39, detach)
SYSCALL(40, join)
SYSCALL(41, wait_process)
SYSCALL(42, raise)
SYSCALL(43, sigsend)
SYSCALL(44, sigwait)
SYSCALL(45, sigmask)
SYSCALL(46, chdir)
SYSCALL(47, current_dir)
SYSCALL(48, chroot)
SYSCALL(49, getcpu_running)
SYSCALL(50, setcpu_mask)
SYSCALL(51, getcpu_mask)
SYSCALL(52, set_tcb)
SYSCALL(53, fork)
SYSCALL(54, execve)
SYSCALL(55, clone)
SYSCALL(56, yield)

END_SYSCALL

} // namespace syscall
