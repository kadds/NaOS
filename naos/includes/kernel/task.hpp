#pragma once
#include "arch/task.hpp"
#include "cpu.hpp"
#include "freelibcxx/allocator.hpp"
#include "freelibcxx/vector.hpp"
#include "kernel/common.hpp"
#include "kernel/fs/vfs/native_directory.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/time.hpp"
#include "lock.hpp"
#include "resource.hpp"
#include "signal.hpp"
#include "types.hpp"
#include "wait.hpp"
#include <atomic>
namespace fs::vfs
{
class file;
}
namespace task::scheduler
{
class scheduler;
}

namespace dev::tty
{
class tty_core;
}

namespace task
{

struct thread_start_info_t
{
    void *userland_entry;
    u64 userland_stack_offset;
    void *args;
};

typedef void (*thread_start_func)(thread_start_info_t *);

/// 65536
extern const thread_id max_thread_id;

/// 1048576
extern const process_id max_process_id;

/// 65536
extern const group_id max_group_id;

extern const session_id max_session_id;

struct thread_t;
struct session_t;
struct process_group_t;

namespace process_attributes
{
enum attributes : flag_t
{
    destroy = 1,
    no_thread = 2,
    userspace = 4,
    job_control_cleanup_done = 8,
    job_control_stopped = 16,
};
} // namespace process_attributes

/// The process struct
struct process_t
{
    process_id pid;
    std::atomic_uint64_t attributes;
    process_id parent_pid;     ///< The parent process id
    void *mm_info;             ///< Memory map infomation
    resource_table_t resource; ///< Resource table
    /// Kernel-owned bootstrap seeds for the native root/cwd capabilities.
    /// They are object references, not user-visible handle numbers and are
    /// never used by a legacy path syscall.  The process runtime owns the
    /// actual capabilities returned by bootstrap.
    handle_t<fs::vfs::native_directory> bootstrap_root_directory;
    handle_t<fs::vfs::native_directory> bootstrap_current_directory;
    /// Explicit bootstrap console capabilities; there is no fd-number keyed
    /// kernel object table anymore.
    na_handle_t console_in_handle = NA_HANDLE_INVALID;
    na_handle_t console_out_handle = NA_HANDLE_INVALID;
    na_handle_t console_err_handle = NA_HANDLE_INVALID;
    /// A native child consumes this endpoint exactly once during startup.
    na_handle_t bootstrap_channel_handle = NA_HANDLE_INVALID;
    std::atomic_bool bootstrap_consumed{false};
    std::atomic_bool main_thread_started{false};
    void *thread_id_gen;
    wait_queue_t wait_queue;
    wait_queue_t child_wait_queue;
    std::atomic_int wait_counter;
    std::atomic_bool wait_claimed;
    std::atomic_uint64_t child_wait_generation;
    std::atomic_uint64_t capability_refs;
    std::atomic_bool reap_pending;
    std::atomic_bool storage_released;

    thread_t *main_thread;
    u64 ret_val;

    lock::spinlock_t thread_list_lock;
    void *thread_list; ///< The threads belong to process
    void *schedule_data;
    handle_t<fs::vfs::file> file;
    signal_pack_t signal_pack;

    /// Session and process-group membership used by job control.
    ::session_id session_id = 0;
    group_id process_group_id = 0;
    session_t *session = nullptr;
    process_group_t *process_group = nullptr;
    dev::tty::tty_core *controlling_tty = nullptr;

    /// Only meaningful on the session leader. The value is shared through the
    /// session leader rather than duplicated in every process in the session.
    group_id foreground_process_group = 0;
    process_t();
};

struct job_control_info
{
    session_id session = 0;
    group_id process_group = 0;
    group_id foreground_process_group = 0;
    bool has_controlling_tty = false;
};

/// A process capability keeps the process state alive after the parent reaps
/// it.  The user-visible identity is the resource-table handle, never pid.
class process_object final : public kobject
{
  public:
    explicit process_object(process_t *process);
    ~process_object() override;

    static type_e type_of() { return type_e::process; }

    process_t *process() { return process_; }
    const process_t *process() const { return process_; }
    na_signal_t capability_signals() const override;
    u64 capability_state() const override;

  private:
    process_t *process_;
};

enum class thread_state : u8
{
    ready = 0, ///< Can schedule
    running,   ///< Running at a CPU
    stop,      ///< Can't reschedule
    destroy,
    sched_switch_to_ready, ///< the task is interrupted by realtime task or switch to next scheduler
};

namespace thread_attributes
{
enum attributes : flag_t
{
    need_schedule = 1,
    block_to_stop = 4,
    detached = 8,
    main = 16,
    real_time = 32,
    on_migrate = 128,
    job_control_stopped = 256,
};
} // namespace thread_attributes
struct preempt_t
{
  private:
    std::atomic_int preempt_counter;

  public:
    bool preemptible() { return preempt_counter == 0; }
    void enable_preempt() { preempt_counter--; }
    void disable_preempt() { preempt_counter++; }
    int get() const { return preempt_counter.load(); }
    void set(int val) { preempt_counter.store(val); }
    preempt_t()
        : preempt_counter(0)
    {
    }
};

struct statistics_t
{
    u64 user_time;
    u64 iowait_time;
    u64 sys_time;
    u64 intr_time;
    u64 soft_intr_time;
};

struct cpu_mask_t
{
    u64 mask;
    cpu_mask_t(u64 mask)
        : mask(mask)
    {
    }
    cpu_mask_t()
        : mask(0xFFFFFFFFFFFFFFFFUL)
    {
    }
};

/// The thread struct
struct thread_t
{
    volatile thread_state state;
    std::atomic_ulong attributes;
    thread_id tid;
    process_t *process;
    scheduler::scheduler *scheduler;
    void *schedule_data;
    ::arch::task::register_info_t *register_info;
    /// The kernel context RSP value
    void *kernel_stack_top;
    void *user_stack_top;
    void *user_stack_bottom;

    /// from 0 - 255, 0 is the highest priority, 100 - 139 to user thread, default is 125
    u8 static_priority = 0;
    /// dynamic priority
    ///
    /// -128 - 128, minimum value means a CPU bound thread, maximum value means a IO
    /// bound thread, the default value is 0.
    i8 dynamic_priority = 0;
    /// run in cpu core
    u32 cpuid = 0;
    cpu_mask_t cpumask;
    statistics_t statistics;
    preempt_t preempt_data;
    wait_queue_t wait_queue;
    std::atomic_int wait_counter;
    u64 error_code = 0;
    wait_queue_t *do_wait_queue_now = nullptr;
    void *tcb = 0;

    // A fault-safe usercopy temporarily arms the page-fault dispatcher with
    // a recovery continuation.  These fields are per-thread because a
    // process may have concurrent syscalls touching different user buffers.
    volatile bool usercopy_active = false;
    volatile u64 usercopy_resume = 0;

    void wake_from_sleep(timeclock::microsecond_t) noexcept;

    thread_t();
};

inline constexpr u64 cpumask_none = 0xFFFFFFFFFFFFFFFF;

void init();
bool has_init();
void start_task_idle();
void switch_thread(thread_t *old, thread_t *new_task);

namespace create_thread_flags
{
enum create_thread_flags : flag_t
{
    immediately = 1,
    noreturn = 4,

    real_time_rr = 4096,
};
} // namespace create_thread_flags

namespace create_process_flags
{
enum create_process_flags : flag_t
{
    noreturn = 1UL,
    binary_file = 1UL << 1,
    real_time_rr = 1UL << 2,
    deferred_start = 1UL << 3,

    no_shared_root = 1UL << 20,
    no_shared_work_dir = 1UL << 21,

    no_shared_stdin = 1UL << 22,
    no_shared_stderror = 1UL << 23,
    no_shared_stdout = 1UL << 24,
    no_shared_files = 1UL << 25,
};
} // namespace create_process_flags

struct args_array_item_t
{
    u32 size;
    u32 offset;
    args_array_item_t(u32 size, u32 offset)
        : size(size)
        , offset(offset)
    {
    }
};

struct process_args_t
{
    byte *data_ptr;
    u64 size;
    u32 execfn_offset;
    void *program_header;
    u64 program_header_entry_size;
    u64 program_header_count;
    u64 base_address;
    u64 hwcap;
    freelibcxx::vector<args_array_item_t> argv;
    freelibcxx::vector<args_array_item_t> env;
    process_args_t(freelibcxx::Allocator *allocator)
        : data_ptr(nullptr)
        , size(0)
        , execfn_offset(0)
        , program_header(nullptr)
        , program_header_entry_size(0)
        , program_header_count(0)
        , base_address(0)
        , hwcap(0)
        , argv(allocator)
        , env(allocator)
    {
    }
};

thread_t *create_thread(process_t *process, thread_start_func start_func, void *userland_entry, void *arg,
                        flag_t flags);

process_args_t *copy_args(const char *path, const char *argv[], const char *env[]);

process_t *create_process(handle_t<fs::vfs::file> file, const char *path, thread_start_func start_func,
                          const char *const args[], const char *const envp[], flag_t flags);

/// Publish a process created with create_process_flags::deferred_start.
void start_process(process_t *process);

/// Tear down a process that has a prepared but never scheduled main thread.
void abort_unstarted_process(process_t *process);

process_t *create_kernel_process(thread_start_func start_func, void *arg, flag_t flags);

int fork();

int execve(handle_t<fs::vfs::file> file, const char *path, thread_start_func start_func, char *const argv[],
           char *const envp[]);

void do_sleep(const timeclock::time &time);

NoReturn void do_exit(i64 value);

i64 wait_process_children(process_t *parent, i64 requested_pid, flag_t flags, i64 &ret, process_id &waited_pid);
i64 wait_process_handle(process_t *parent, process_t *target, flag_t flags, i64 &ret, process_id &waited_pid);
i64 open_process_handle(process_t *caller, i64 requested_pid, khandle &object);

NoReturn void do_exit_thread(i64 ret);
u64 detach_thread(thread_t *thd);
u64 join_thread(thread_t *thd, i64 &ret);

thread_t *find_kernel_stack_thread(void *stack_ptr);

namespace exit_control_flags
{
enum : flag_t
{
    core_dump = 2,
};
}

void stop_thread(thread_t *thread, flag_t flags);
void continue_thread(thread_t *thread, flag_t flags);
void stop_process(process_t *process, flag_t flags = 0);
void continue_process(process_t *process, flag_t flags = 0);

process_t *find_pid(process_id pid);
thread_t *find_tid(process_t *process, thread_id tid);

/// Create a new session for a process and make it the session and process-group
/// leader. Returns a negative kernel errno on failure, or the new session id.
i64 setsid(process_t *process);

/// Move a process into an existing process group, or create the group's leader
/// group when pgid == target pid. pid == 0 means the caller.
int setpgid(process_t *caller, process_id pid, group_id pgid);

/// Snapshot the job-control state associated with a process capability.
bool get_job_control_info(const process_t *process, job_control_info &info);

/// Attach/detach the controlling tty associated with a session.
int attach_controlling_tty(process_t *process, dev::tty::tty_core *tty, bool force = false);
void detach_controlling_tty(process_t *process);

/// Query or update the foreground process group for a controlling tty.
i64 get_foreground_process_group(dev::tty::tty_core *tty);
int set_foreground_process_group(process_t *process, dev::tty::tty_core *tty, group_id pgid);

/// Enforce the controlling-terminal rules for a process using a tty.
/// Returns zero when the operation may proceed, or a negative errno after
/// delivering the appropriate job-control signal.
i64 check_tty_job_control(process_t *process, dev::tty::tty_core *tty, bool input, bool tostop = false);

/// Return the controlling tty without exposing its definition to task users.
dev::tty::tty_core *get_controlling_tty(process_t *process);

void exit_process(process_t *process, i64 ret, flag_t flags);

process_t *get_init_process();

void set_init_process(process_t *proc);

inline thread_t *current()
{
    auto &t = cpu::current();
    if (likely(&t))
    {
        return (thread_t *)t.get_task();
    }
    return nullptr;
}
inline process_t *current_process()
{
    auto t = current();
    if (likely(t))
    {
        return t->process;
    }
    return nullptr;
}

void set_cpu_mask(thread_t *thd, cpu_mask_t mask);

static inline cpu_mask_t current_cpu_mask() { return (1ul << cpu::current().id()); }

void thread_yield();
void user_schedule();

void set_tcb(thread_t *t, void *p);

struct main_stack_data_t
{
    int argc;
    char **argv;
    char **envp;
};
void write_main_stack(thread_t *thread, main_stack_data_t stack);

} // namespace task
