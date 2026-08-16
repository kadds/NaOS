#include "kernel/task.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/errno.hpp"
#include "kernel/fs/vfs/file.hpp"
#include "kernel/fs/vfs/vfs.hpp"
#include "kernel/ipc/channel.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/syscall.hpp"
#include "kernel/time.hpp"
#include "kernel/timer.hpp"
#include "kernel/usercopy.hpp"
#include "naos/abi.h"
#include "naos/bootstrap.hpp"
#include "naos/generated/system/Process.hpp"
#include "naos/generated/system/Stream.hpp"
#include "naos/generated/system_uapi.h"
#include <atomic>
#include <limits>
#include <utility>

namespace naos::syscall
{

namespace
{
na_status_t native_status_from_errno(i64 error)
{
    switch (error)
    {
    case 0:
        return NA_STATUS_OK;
    case EFAULT:
        return NA_STATUS_FAULT;
    case EINVAL:
        return NA_STATUS_INVALID_ARGUMENT;
    case EBADF:
    case ECHILD:
        return NA_STATUS_INVALID_HANDLE;
    case ENOMEM:
        return NA_STATUS_RESOURCE_EXHAUSTED;
    case ENOEXEC:
        return NA_STATUS_NOT_SUPPORTED;
    case EIO:
        return NA_STATUS_IO_ERROR;
    default:
        return NA_STATUS_IO_ERROR;
    }
}
} // namespace

enum futex_op
{
    futex_wake = 1,
    futex_wait = 2,
};

struct futex_bucket
{
    task::wait_queue_t waiters;
    std::atomic_uint64_t generation{0};

    void wake(timeclock::microsecond_t) noexcept
    {
        generation.fetch_add(1, std::memory_order_release);
        waiters.do_wake_up();
    }
};

constexpr u64 futex_bucket_count = 64;
std::atomic<futex_bucket *> futex_buckets{nullptr};
std::atomic_flag futex_buckets_lock = ATOMIC_FLAG_INIT;

futex_bucket *ensure_futex_buckets()
{
    auto *buckets = futex_buckets.load(std::memory_order_acquire);
    if (buckets != nullptr)
        return buckets;
    while (futex_buckets_lock.test_and_set(std::memory_order_acquire))
        cpu_pause();
    buckets = futex_buckets.load(std::memory_order_relaxed);
    if (buckets == nullptr)
    {
        buckets = memory::NewArray<futex_bucket>(memory::KernelCommonAllocatorV, futex_bucket_count);
        futex_buckets.store(buckets, std::memory_order_release);
    }
    futex_buckets_lock.clear(std::memory_order_release);
    return buckets;
}

futex_bucket &bucket_for(futex_bucket *buckets, const int *ptr)
{
    const auto key = reinterpret_cast<u64>(ptr) >> 2;
    return buckets[key % futex_bucket_count];
}

int futex(int *ptr, int op, int val, const timeclock::time *timeout, int val2)
{
    (void)val2;
    if (ptr == nullptr || !is_user_space_range(ptr, sizeof(*ptr)))
        return EFAULT;

    auto *buckets = ensure_futex_buckets();
    if (buckets == nullptr)
        return EFAILED;
    auto &bucket = bucket_for(buckets, ptr);
    if (op == futex_op::futex_wait)
    {
        int observed = 0;
        if (naos::usercopy::copy_from(&observed, reinterpret_cast<u64>(ptr), sizeof(observed)) != NA_STATUS_OK)
            return EFAULT;
        if (observed != val)
            return EAGAIN;

        timeclock::time relative(0, 0);
        bool has_timeout = timeout != nullptr;
        if (has_timeout &&
            naos::usercopy::copy_from(&relative, reinterpret_cast<u64>(timeout), sizeof(relative)) != NA_STATUS_OK)
            return EFAULT;
        if (has_timeout && (relative.tv_sec < 0 || relative.tv_nsec < 0 || relative.tv_nsec >= 1000000000))
            return EPARAM;

        const auto generation = bucket.generation.load(std::memory_order_acquire);
        timer::watcher_id deadline_watcher = timer::invalid_watcher_id;
        if (has_timeout)
        {
            const auto seconds = static_cast<u64>(relative.tv_sec);
            const auto nanoseconds = static_cast<u64>(relative.tv_nsec);
            if (seconds > std::numeric_limits<u64>::max() / 1000000 ||
                seconds * 1000000 > std::numeric_limits<u64>::max() - nanoseconds / 1000)
                return EOVERFLOW;
            const auto duration = seconds * 1000000 + nanoseconds / 1000;
            const auto now = timer::get_high_resolution_time();
            if (duration > std::numeric_limits<u64>::max() - now)
                return EOVERFLOW;
            if (duration == 0)
                return ETIMEDOUT;
            deadline_watcher =
                timer::schedule_at(now + duration, timer::timer_handler::bind<&futex_bucket::wake>(bucket));
            if (deadline_watcher == timer::invalid_watcher_id)
                return EFAILED;
        }

        bucket.waiters.do_wait([&bucket, generation, ptr, val] {
            int current = 0;
            if (naos::usercopy::copy_from(&current, reinterpret_cast<u64>(ptr), sizeof(current)) != NA_STATUS_OK)
                return true;
            return bucket.generation.load(std::memory_order_acquire) != generation || current != val;
        });
        if (deadline_watcher != timer::invalid_watcher_id)
            (void)timer::cancel(deadline_watcher);

        if (naos::usercopy::copy_from(&observed, reinterpret_cast<u64>(ptr), sizeof(observed)) != NA_STATUS_OK)
            return EFAULT;
        if (observed == val && bucket.generation.load(std::memory_order_acquire) == generation)
            return has_timeout ? ETIMEDOUT : EAGAIN;
        return 0;
    }
    else if (op == futex_op::futex_wake)
    {
        bucket.generation.fetch_add(1, std::memory_order_release);
        if (val <= 0)
            return 0;
        return static_cast<int>(bucket.waiters.do_wake_up(static_cast<u64>(val)));
    }
    return EPARAM;
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

na_status_t process_handle_open(i64 pid, na_handle_t *output)
{
    if (output == nullptr || !is_user_space_range(output, sizeof(*output)))
        return NA_STATUS_FAULT;

    khandle object;
    const auto open_status = task::open_process_handle(task::current_process(), pid, object);
    if (open_status != 0)
        return native_status_from_errno(open_status);

    capability::metadata metadata;
    metadata.binding = NA_BINDING_KERNEL_VIEW;
    metadata.protocol_uuid = naos::system::Process::protocol_uuid;
    metadata.scope = NA_SCOPE_PROCESS;
    metadata.revision = naos::system::Process::revision;
    metadata.meta_rights = NA_RIGHT_DUPLICATE | NA_RIGHT_TRANSFER | NA_RIGHT_WAIT | NA_RIGHT_INSPECT;
    metadata.protocol_rights = static_cast<u64>(NA_PROTOCOL_RIGHT_INVOKE) | static_cast<u64>(NA_PROCESS_RIGHT_WAIT) |
                               static_cast<u64>(NA_PROCESS_RIGHT_INSPECT) |
                               static_cast<u64>(NA_PROCESS_RIGHT_JOB_CONTROL) |
                               static_cast<u64>(NA_PROCESS_RIGHT_START);
    const auto handle = task::current_process()->resource.install_native(std::move(object), metadata);
    if (handle == NA_HANDLE_INVALID)
        return NA_STATUS_RESOURCE_EXHAUSTED;
    const auto copy_status = naos::usercopy::copy_to(reinterpret_cast<u64>(output), &handle, sizeof(handle));
    if (copy_status != NA_STATUS_OK)
        task::current_process()->resource.close_native(handle);
    return copy_status;
}

namespace
{
capability::metadata process_capability_metadata()
{
    capability::metadata metadata;
    metadata.binding = NA_BINDING_KERNEL_VIEW;
    metadata.protocol_uuid = naos::system::Process::protocol_uuid;
    metadata.scope = NA_SCOPE_PROCESS;
    metadata.revision = naos::system::Process::revision;
    metadata.meta_rights = NA_RIGHT_DUPLICATE | NA_RIGHT_TRANSFER | NA_RIGHT_WAIT | NA_RIGHT_INSPECT;
    metadata.protocol_rights = static_cast<u64>(NA_PROTOCOL_RIGHT_INVOKE) | static_cast<u64>(NA_PROCESS_RIGHT_WAIT) |
                               static_cast<u64>(NA_PROCESS_RIGHT_INSPECT) |
                               static_cast<u64>(NA_PROCESS_RIGHT_JOB_CONTROL) |
                               static_cast<u64>(NA_PROCESS_RIGHT_START);
    return metadata;
}

} // namespace

/// sleep current thread
int sleep(const timeclock::time *time)
{
    if (!is_user_space_range(time, sizeof(*time)))
        return EPARAM;
    timeclock::time value(0, 0);
    if (naos::usercopy::copy_from(&value, reinterpret_cast<u64>(time), sizeof(value)) != NA_STATUS_OK)
        return EFAULT;
    task::do_sleep(value);
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

    target_t target_values{};
    if (naos::usercopy::copy_from(&target_values, reinterpret_cast<u64>(target), sizeof(target_values)) != NA_STATUS_OK)
        return EFAULT;

    sig_info_t info_values{};
    if (info != nullptr &&
        naos::usercopy::copy_from(&info_values, reinterpret_cast<u64>(info), sizeof(info_values)) != NA_STATUS_OK)
        return EFAULT;

    if (target_values.flags != send_to_process && target_values.flags != send_to_group)
        return EPARAM;
    if (num >= task::max_signal_count)
        return EPARAM;

    if (target_values.flags == send_to_process)
    {
        auto proc = task::find_pid(target_values.id);
        if (proc == nullptr)
            return ENOEXIST;
        if (num != 0)
        {
            if (info != nullptr)
                proc->signal_pack.send(proc, num, info_values.error, info_values.code, info_values.status);
            else
                proc->signal_pack.send(proc, num, 0, 0, 0);
        }
    }
    else
    {
        const auto group =
            target_values.id == 0 ? task::current_process()->process_group_id : static_cast<group_id>(target_values.id);
        const auto result = task::send_signal_to_process_group(group, num, info != nullptr ? info_values.error : 0,
                                                               info != nullptr ? info_values.code : 0,
                                                               info != nullptr ? info_values.status : 0);
        if (result < 0)
            return result;
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
    if ((valid_mask && !is_user_space_range(valid_mask, sizeof(*valid_mask))) ||
        (block_mask && !is_user_space_range(block_mask, sizeof(*block_mask))) ||
        (ignore_mask && !is_user_space_range(ignore_mask, sizeof(*ignore_mask))))
        return EFAULT;

    u64 valid_value = 0;
    u64 block_value = 0;
    u64 ignore_value = 0;
    const auto read_mask = [](u64 *source, u64 &destination) {
        if (source == nullptr)
            return true;
        return naos::usercopy::copy_from(&destination, reinterpret_cast<u64>(source), sizeof(destination)) ==
               NA_STATUS_OK;
    };
    const auto write_mask = [](u64 *destination, u64 value) {
        return destination == nullptr ||
               naos::usercopy::copy_to(reinterpret_cast<u64>(destination), &value, sizeof(value)) == NA_STATUS_OK;
    };
    if (opt != SIGOPT_GET && opt != SIGOPT_INVALID_ALL)
    {
        if (!read_mask(valid_mask, valid_value) || !read_mask(block_mask, block_value) ||
            !read_mask(ignore_mask, ignore_value))
            return EFAULT;
    }
    auto &sigpack = task::current_process()->signal_pack;

    auto &valid = sigpack.get_mask().get_valid_set();
    auto &block = sigpack.get_mask().get_block_set();
    auto &ignore = sigpack.get_mask().get_ignore_set();

    if (opt == SIGOPT_GET)
    {
        if (valid_mask)
            valid_value = valid.get();
        if (block_mask)
            block_value = block.get();
        if (ignore_mask)
            ignore_value = ignore.get();
        if (!write_mask(valid_mask, valid_value) || !write_mask(block_mask, block_value) ||
            !write_mask(ignore_mask, ignore_value))
            return EFAULT;
    }
    else if (opt == SIGOPT_SET)
    {
        if (valid_mask)
            valid.set(valid_value);
        if (block_mask)
            block.set(block_value);
        if (ignore_mask)
            ignore.set(ignore_value);
    }
    else if (opt == SIGOPT_OR)
    {
        if (valid_mask)
            valid.set(valid_value | valid.get());
        if (block_mask)
            block.set(block_value | block.get());
        if (ignore_mask)
            ignore.set(ignore_value | ignore.get());
    }
    else if (opt == SIGOPT_AND)
    {
        if (valid_mask)
            valid.set(valid_value & valid.get());
        if (block_mask)
            block.set(block_value & block.get());
        if (ignore_mask)
            ignore.set(ignore_value & ignore.get());
    }
    else if (opt == SIGOPT_XOR)
    {
        if (valid_mask)
            valid.set(valid_value ^ valid.get());
        if (block_mask)
            block.set(block_value ^ block.get());
        if (ignore_mask)
            ignore.set(ignore_value ^ ignore.get());
    }
    else if (opt == SIGOPT_INVALID_ALL)
    {
        valid.set(0);
    }
    else
    {
        return EPARAM;
    }

    // SIGKILL and SIGSTOP cannot be caught, ignored, or blocked.
    valid -= task::signal::sigkill;
    valid -= task::signal::sigstop;
    block -= task::signal::sigkill;
    block -= task::signal::sigstop;
    ignore -= task::signal::sigkill;
    ignore -= task::signal::sigstop;
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

na_status_t process_exec(const na_process_exec_frame_t *frame)
{
    if (frame == nullptr || !is_user_space_range(frame, sizeof(u32)))
        return NA_STATUS_FAULT;

    na_process_exec_frame_t values{};
    const auto copy_status = naos::usercopy::copy_versioned(values, frame);
    if (copy_status != NA_STATUS_OK)
        return copy_status;
    if (values.flags != 0 || values.reserved0 != 0 || values.reserved1 != 0 || values.executable == NA_HANDLE_INVALID)
        return NA_STATUS_INVALID_ARGUMENT;

    const auto path = reinterpret_cast<const char *>(values.path);
    const auto argv = reinterpret_cast<char *const *>(values.argv);
    const auto envp = reinterpret_cast<char *const *>(values.envp);
    if (!is_user_space_pointer(path) || !is_user_space_pointer_or_null(argv) || !is_user_space_pointer(envp))
        return NA_STATUS_FAULT;

    auto *process = task::current_process();
    capability::entry entry;
    if (process == nullptr || !process->resource.lookup_native(values.executable, entry) || !entry.object)
        return NA_STATUS_INVALID_HANDLE;
    if (entry.meta.binding != NA_BINDING_KERNEL_VIEW)
        return NA_STATUS_WRONG_BINDING;
    if (entry.meta.scope != NA_SCOPE_FILE)
        return NA_STATUS_WRONG_SCOPE;
    if (entry.object->get<fs::vfs::file>() == nullptr)
        return NA_STATUS_WRONG_BINDING;

    handle_t<fs::vfs::file> file(entry.object.get_control());
    process->resource.close_native(values.executable);
    return native_status_from_errno(task::execve(std::move(file), path, before_user_thread, argv, envp));
}

na_status_t process_spawn(const na_process_spawn_frame_t *frame)
{
    if (frame == nullptr || !is_user_space_range(frame, sizeof(u32)))
        return NA_STATUS_FAULT;

    na_process_spawn_frame_t values{};
    const auto copy_status = naos::usercopy::copy_versioned(values, frame);
    if (copy_status != NA_STATUS_OK)
        return copy_status;
    if (values.struct_size < sizeof(values) ||
        (values.flags & ~NA_PROCESS_SPAWN_DEFERRED_START) != 0 || values.reserved0 != 0 || values.reserved1 != 0 ||
        values.executable == NA_HANDLE_INVALID || values.bootstrap_endpoint == NA_HANDLE_INVALID || values.process == 0)
        return NA_STATUS_INVALID_ARGUMENT;

    const auto path = reinterpret_cast<const char *>(values.path);
    const auto argv = reinterpret_cast<char *const *>(values.argv);
    const auto envp = reinterpret_cast<char *const *>(values.envp);
    if (!is_user_space_pointer(path) || !is_user_space_pointer_or_null(argv) || !is_user_space_pointer_or_null(envp) ||
        !is_user_space_range(reinterpret_cast<void *>(values.process), sizeof(na_handle_t)) ||
        (values.pid != 0 && !is_user_space_range(reinterpret_cast<void *>(values.pid), sizeof(process_id))))
        return NA_STATUS_FAULT;
    if (values.pid != 0 &&
        naos::usercopy::ranges_overlap(values.process, sizeof(na_handle_t), values.pid, sizeof(process_id)))
        return NA_STATUS_INVALID_ARGUMENT;

    auto *parent = task::current_process();
    capability::entry executable_entry;
    capability::entry endpoint_entry;
    if (parent == nullptr || !parent->resource.lookup_native(values.executable, executable_entry) ||
        !parent->resource.lookup_native(values.bootstrap_endpoint, endpoint_entry) || !executable_entry.object ||
        !endpoint_entry.object)
        return NA_STATUS_INVALID_HANDLE;
    if (executable_entry.meta.binding != NA_BINDING_KERNEL_VIEW ||
        executable_entry.object->get<fs::vfs::file>() == nullptr)
        return NA_STATUS_WRONG_BINDING;
    if (executable_entry.meta.scope != NA_SCOPE_FILE)
        return NA_STATUS_WRONG_SCOPE;
    if (endpoint_entry.meta.binding != NA_BINDING_RAW_CHANNEL_END)
        return NA_STATUS_WRONG_BINDING;

    na_resource_disposition_t dispositions[2]{};
    dispositions[0].handle = values.executable;
    dispositions[0].operation = NA_RESOURCE_MOVE;
    dispositions[1].handle = values.bootstrap_endpoint;
    dispositions[1].operation = NA_RESOURCE_MOVE;
    capability::transfer_record_list records(memory::KernelCommonAllocatorV);
    auto status = parent->resource.take_native_batch(dispositions, 2, NA_HANDLE_INVALID, records);
    if (status != NA_STATUS_OK)
        return status;
    auto restore = [&](na_status_t failure) {
        const auto restore_status = parent->resource.restore_native_batch(records);
        return restore_status == NA_STATUS_OK ? failure : restore_status;
    };

    auto *file_object = records[0].resource.object()->get<fs::vfs::file>();
    if (file_object == nullptr)
        return restore(NA_STATUS_WRONG_BINDING);
    handle_t<fs::vfs::file> file(records[0].resource.object().get_control());
    const auto child_flags = task::create_process_flags::deferred_start | task::create_process_flags::no_shared_root |
                             task::create_process_flags::no_shared_work_dir |
                             task::create_process_flags::no_shared_files | task::create_process_flags::no_shared_stdin |
                             task::create_process_flags::no_shared_stdout |
                             task::create_process_flags::no_shared_stderror;
    auto *child =
        task::create_process(std::move(file), path, before_user_thread, reinterpret_cast<const char *const *>(argv),
                             reinterpret_cast<const char *const *>(envp), child_flags);
    if (child == nullptr)
        return restore(NA_STATUS_RESOURCE_EXHAUSTED);
    child->bootstrap_root_directory.reset();
    child->bootstrap_current_directory.reset();
    child->console_in_handle = NA_HANDLE_INVALID;
    child->console_out_handle = NA_HANDLE_INVALID;
    child->console_err_handle = NA_HANDLE_INVALID;

    auto process_object = handle_t<task::process_object>::make(child);
    const auto process_handle =
        parent->resource.install_native(std::move(process_object), process_capability_metadata());
    if (process_handle == NA_HANDLE_INVALID)
    {
        task::abort_unstarted_process(child);
        return restore(NA_STATUS_RESOURCE_EXHAUSTED);
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
        return restore(status);
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
        return restore(status);
    }

    child->bootstrap_channel_handle = child_handles[0];
    auto dropped_executable = records[0].resource.take_object();
    dropped_executable.reset();
    parent->resource.commit_native_batch(records);
    if ((values.flags & NA_PROCESS_SPAWN_DEFERRED_START) == 0)
        task::start_process(child);
    return NA_STATUS_OK;
}
int yield() { return 0; }

na_status_t pipe_create(na_pipe_create_frame_t *frame)
{
    if (frame == nullptr || !is_user_space_range(frame, sizeof(*frame)))
        return NA_STATUS_FAULT;

    auto file = fs::vfs::open_pipe();
    if (!file)
        return NA_STATUS_IO_ERROR;

    capability::metadata metadata;
    metadata.binding = NA_BINDING_KERNEL_VIEW;
    metadata.protocol_uuid = naos::system::Stream::protocol_uuid;
    metadata.scope = NA_SCOPE_STREAM;
    metadata.revision = 1;
    metadata.meta_rights = NA_RIGHT_DUPLICATE | NA_RIGHT_TRANSFER | NA_RIGHT_WAIT | NA_RIGHT_INSPECT;
    metadata.protocol_rights = NA_PROTOCOL_RIGHT_INVOKE;

    auto &resources = task::current_process()->resource;
    khandle read_object = file;
    khandle write_object = file;
    const auto read_handle = resources.install_native(std::move(read_object), metadata);
    const auto write_handle = resources.install_native(std::move(write_object), metadata);
    if (read_handle == NA_HANDLE_INVALID || write_handle == NA_HANDLE_INVALID)
    {
        resources.close_native(read_handle);
        resources.close_native(write_handle);
        return NA_STATUS_RESOURCE_EXHAUSTED;
    }

    na_pipe_create_frame_t values{read_handle, write_handle};
    const auto status = naos::usercopy::copy_to(reinterpret_cast<u64>(frame), &values, sizeof(values));
    if (status != NA_STATUS_OK)
    {
        resources.close_native(read_handle);
        resources.close_native(write_handle);
        return status;
    }
    return NA_STATUS_OK;
}

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
SYSCALL(NA_SYSCALL_PIPE_CREATE, pipe_create)

END_SYSCALL

} // namespace naos::syscall
