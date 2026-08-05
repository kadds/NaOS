#include "kernel/task.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/errno.hpp"
#include "kernel/fs/vfs/file.hpp"
#include "kernel/fs/vfs/vfs.hpp"
#include "kernel/ipc/channel.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/syscall.hpp"
#include "kernel/time.hpp"
#include "kernel/usercopy.hpp"
#include "naos/abi.h"
#include "naos/bootstrap.hpp"
#include "naos/generated/system_uapi.h"
#include <atomic>
#include <utility>

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

i64 process_handle_open(i64 pid, na_handle_t *output)
{
    if (output == nullptr || !is_user_space_range(output, sizeof(*output)))
        return EFAULT;

    khandle object;
    const auto open_status = task::open_process_handle(task::current_process(), pid, object);
    if (open_status != 0)
        return open_status;

    capability::metadata metadata;
    metadata.binding = NA_BINDING_KERNEL_VIEW;
    metadata.scope = NA_SCOPE_PROCESS;
    metadata.revision = 1;
    metadata.meta_rights = NA_RIGHT_DUPLICATE | NA_RIGHT_TRANSFER | NA_RIGHT_WAIT | NA_RIGHT_INSPECT;
    metadata.protocol_rights = static_cast<u64>(NA_PROTOCOL_RIGHT_INVOKE) | static_cast<u64>(NA_PROCESS_RIGHT_WAIT) |
                               static_cast<u64>(NA_PROCESS_RIGHT_INSPECT) |
                               static_cast<u64>(NA_PROCESS_RIGHT_JOB_CONTROL);
    const auto handle = task::current_process()->resource.install_native(std::move(object), metadata);
    if (handle == NA_HANDLE_INVALID)
        return EFAILED;
    const auto copy_status = naos::usercopy::copy_to(reinterpret_cast<u64>(output), &handle, sizeof(handle));
    if (copy_status != NA_STATUS_OK)
        task::current_process()->resource.close_native(handle);
    return copy_status == NA_STATUS_OK ? 0 : EFAULT;
}

namespace
{
capability::metadata process_capability_metadata()
{
    capability::metadata metadata;
    metadata.binding = NA_BINDING_KERNEL_VIEW;
    metadata.scope = NA_SCOPE_PROCESS;
    metadata.revision = 1;
    metadata.meta_rights = NA_RIGHT_DUPLICATE | NA_RIGHT_TRANSFER | NA_RIGHT_WAIT | NA_RIGHT_INSPECT;
    metadata.protocol_rights = static_cast<u64>(NA_PROTOCOL_RIGHT_INVOKE) | static_cast<u64>(NA_PROCESS_RIGHT_WAIT) |
                               static_cast<u64>(NA_PROCESS_RIGHT_INSPECT) |
                               static_cast<u64>(NA_PROCESS_RIGHT_JOB_CONTROL);
    return metadata;
}

} // namespace

/// sleep current thread
int sleep(const timeclock::time *time)
{
    if (!is_user_space_range(time, sizeof(*time)))
        return EPARAM;
    task::do_sleep(*time);
    return OK;
}

struct sig_info_t
{
    u64 error;
    u64 code;
    u64 status;
    u64 tid;
    u64 pid;
};

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
    if (!is_user_space_range(target, sizeof(*target)))
        return EPARAM;
    if (info != nullptr && !is_user_space_range(info, sizeof(*info)))
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

int set_tcb(void *p)
{
    if (!is_user_space_pointer(p))
        return EPARAM;
    auto t = task::current();
    task::set_tcb(t, p);

    return 0;
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

int64_t process_exec(const na_process_exec_frame_t *frame)
{
    if (frame == nullptr || !is_user_space_range(frame, sizeof(u32)))
        return EFAULT;

    na_process_exec_frame_t values{};
    const auto copy_status = naos::usercopy::copy_versioned(values, frame);
    if (copy_status != NA_STATUS_OK)
        return copy_status == NA_STATUS_FAULT ? EFAULT : EINVAL;
    if (values.flags != 0 || values.reserved0 != 0 || values.reserved1 != 0 || values.executable == NA_HANDLE_INVALID)
        return EINVAL;

    const auto path = reinterpret_cast<const char *>(values.path);
    const auto argv = reinterpret_cast<char *const *>(values.argv);
    const auto envp = reinterpret_cast<char *const *>(values.envp);
    if (!is_user_space_pointer(path) || !is_user_space_pointer_or_null(argv) || !is_user_space_pointer(envp))
        return EFAULT;

    auto *process = task::current_process();
    capability::entry entry;
    if (process == nullptr || !process->resource.lookup_native(values.executable, entry) || !entry.object)
        return EBADF;
    if (entry.meta.binding != NA_BINDING_KERNEL_VIEW)
        return EINVAL;
    if (entry.meta.scope != NA_SCOPE_FILE)
        return EINVAL;
    if (entry.object->get<fs::vfs::file>() == nullptr)
        return EINVAL;

    handle_t<fs::vfs::file> file(entry.object.get_control());
    process->resource.close_native(values.executable);
    return task::execve(std::move(file), path, before_user_thread, argv, envp);
}

int64_t process_spawn(const na_process_spawn_frame_t *frame)
{
    if (frame == nullptr || !is_user_space_range(frame, sizeof(u32)))
        return EFAULT;

    na_process_spawn_frame_t values{};
    const auto copy_status = naos::usercopy::copy_versioned(values, frame);
    if (copy_status != NA_STATUS_OK)
        return copy_status == NA_STATUS_FAULT ? EFAULT : EINVAL;
    if (values.struct_size < sizeof(values) || values.flags != 0 || values.reserved0 != 0 || values.reserved1 != 0 ||
        values.executable == NA_HANDLE_INVALID || values.bootstrap_endpoint == NA_HANDLE_INVALID ||
        values.process == 0)
        return EINVAL;

    const auto path = reinterpret_cast<const char *>(values.path);
    const auto argv = reinterpret_cast<char *const *>(values.argv);
    const auto envp = reinterpret_cast<char *const *>(values.envp);
    if (!is_user_space_pointer(path) || !is_user_space_pointer_or_null(argv) ||
        !is_user_space_pointer_or_null(envp) || !is_user_space_range(reinterpret_cast<void *>(values.process),
                                                                       sizeof(na_handle_t)) ||
        (values.pid != 0 && !is_user_space_range(reinterpret_cast<void *>(values.pid), sizeof(process_id))))
        return EFAULT;
    if (values.pid != 0 &&
        naos::usercopy::ranges_overlap(values.process, sizeof(na_handle_t), values.pid, sizeof(process_id)))
        return EINVAL;

    auto *parent = task::current_process();
    capability::entry executable_entry;
    capability::entry endpoint_entry;
    if (parent == nullptr || !parent->resource.lookup_native(values.executable, executable_entry) ||
        !parent->resource.lookup_native(values.bootstrap_endpoint, endpoint_entry) || !executable_entry.object ||
        !endpoint_entry.object)
        return EBADF;
    if (executable_entry.meta.binding != NA_BINDING_KERNEL_VIEW || executable_entry.meta.scope != NA_SCOPE_FILE ||
        executable_entry.object->get<fs::vfs::file>() == nullptr)
        return EINVAL;
    if (endpoint_entry.meta.binding != NA_BINDING_RAW_CHANNEL_END)
        return EINVAL;

    na_resource_disposition_t dispositions[2]{};
    dispositions[0].handle = values.executable;
    dispositions[0].operation = NA_RESOURCE_MOVE;
    dispositions[1].handle = values.bootstrap_endpoint;
    dispositions[1].operation = NA_RESOURCE_MOVE;
    capability::transfer_record_list records(memory::KernelCommonAllocatorV);
    auto status = parent->resource.take_native_batch(dispositions, 2, NA_HANDLE_INVALID, records);
    if (status != NA_STATUS_OK)
        return EINVAL;

    auto *file_object = records[0].resource.object()->get<fs::vfs::file>();
    if (file_object == nullptr)
    {
        parent->resource.restore_native_batch(records);
        return EINVAL;
    }
    handle_t<fs::vfs::file> file(records[0].resource.object().get_control());
    const auto child_flags = task::create_process_flags::deferred_start |
                             task::create_process_flags::no_shared_root |
                             task::create_process_flags::no_shared_work_dir |
                             task::create_process_flags::no_shared_files |
                             task::create_process_flags::no_shared_stdin |
                             task::create_process_flags::no_shared_stdout |
                             task::create_process_flags::no_shared_stderror;
    auto *child = task::create_process(std::move(file), path, before_user_thread,
                                       reinterpret_cast<const char *const *>(argv),
                                       reinterpret_cast<const char *const *>(envp), child_flags);
    if (child == nullptr)
    {
        parent->resource.restore_native_batch(records);
        return EFAILED;
    }

    child->bootstrap_root_directory.reset();
    child->bootstrap_current_directory.reset();
    child->console_in_handle = NA_HANDLE_INVALID;
    child->console_out_handle = NA_HANDLE_INVALID;
    child->console_err_handle = NA_HANDLE_INVALID;

    auto process_object = handle_t<task::process_object>::make(child);
    const auto process_handle = parent->resource.install_native(std::move(process_object), process_capability_metadata());
    if (process_handle == NA_HANDLE_INVALID)
    {
        task::abort_unstarted_process(child);
        parent->resource.restore_native_batch(records);
        return EFAILED;
    }

    status = naos::usercopy::copy_to(values.process, &process_handle, sizeof(process_handle));
    if (status == NA_STATUS_OK && values.pid != 0)
    {
        const auto pid = child->pid;
        status = naos::usercopy::copy_to(values.pid, &pid, sizeof(pid));
    }
    if (status != NA_STATUS_OK)
    {
        parent->resource.close_native(process_handle);
        task::abort_unstarted_process(child);
        parent->resource.restore_native_batch(records);
        return status == NA_STATUS_FAULT ? EFAULT : EFAILED;
    }

    freelibcxx::vector<na_handle_t> child_handles(memory::KernelCommonAllocatorV);
    status = child->resource.reserve_native(child_handles, 1);
    if (status == NA_STATUS_OK)
        status = child->resource.activate_native(child_handles[0], std::move(records[1].resource));
    if (status != NA_STATUS_OK)
    {
        child->resource.rollback_native(child_handles);
        parent->resource.close_native(process_handle);
        task::abort_unstarted_process(child);
        parent->resource.restore_native_batch(records);
        return EFAILED;
    }

    child->bootstrap_channel_handle = child_handles[0];
    auto dropped_executable = records[0].resource.take_object();
    dropped_executable.reset();
    task::start_process(child);
    return 0;
}
int yield() { return 0; }

BEGIN_SYSCALL

SYSCALL(NA_SYSCALL_FUTEX, futex)
SYSCALL(NA_SYSCALL_EXIT, exit)
SYSCALL(NA_SYSCALL_EXIT_THREAD, exit_thread)
SYSCALL(NA_SYSCALL_SLEEP, sleep)
SYSCALL(NA_SYSCALL_CURRENT_PID, current_pid)
SYSCALL(NA_SYSCALL_CURRENT_TID, current_tid)
SYSCALL(NA_SYSCALL_SIGSEND, sigsend)
SYSCALL(NA_SYSCALL_SIGMASK, sigmask)
SYSCALL(NA_SYSCALL_SET_TCB, set_tcb)
SYSCALL(NA_SYSCALL_FORK, fork)
SYSCALL(NA_SYSCALL_CLONE, clone)
SYSCALL(NA_SYSCALL_YIELD, yield)
SYSCALL(NA_SYSCALL_PROCESS_EXEC, process_exec)
SYSCALL(NA_SYSCALL_PROCESS_HANDLE_OPEN, process_handle_open)
SYSCALL(NA_SYSCALL_PROCESS_SPAWN, process_spawn)

END_SYSCALL

} // namespace naos::syscall
