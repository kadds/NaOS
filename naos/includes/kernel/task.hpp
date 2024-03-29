#pragma once
#include "arch/task.hpp"
#include "common.hpp"
#include "cpu.hpp"
#include "freelibcxx/allocator.hpp"
#include "freelibcxx/vector.hpp"
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
struct thread_t;

namespace process_attributes
{
enum attributes : flag_t
{
    destroy = 1,
    no_thread = 2,
    userspace = 4,
};
} // namespace process_attributes

/// The process struct
struct process_t
{
    process_id pid;
    std::atomic_uint64_t attributes;
    u64 parent_pid;             ///< The parent process id
    void *mm_info;              ///< Memory map infomation
    resource_table_t resource;  ///< Resource table
    void *thread_id_gen;
    wait_queue_t wait_queue;
    std::atomic_int wait_counter;

    thread_t *main_thread;
    u64 ret_val;

    lock::spinlock_t thread_list_lock;
    void *thread_list; ///< The threads belong to process
    void *schedule_data;
    handle_t<fs::vfs::file> file;
    signal_pack_t signal_pack;
    process_t();
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
    freelibcxx::vector<args_array_item_t> argv;
    freelibcxx::vector<args_array_item_t> env;
    process_args_t(freelibcxx::Allocator *allocator)
        : data_ptr(nullptr)
        , size(0)
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

process_t *create_kernel_process(thread_start_func start_func, void *arg, flag_t flags);

int fork();

int execve(handle_t<fs::vfs::file> file, const char *path, thread_start_func start_func, char *const argv[],
           char *const envp[]);

void do_sleep(const timeclock::time &time);

NoReturn void do_exit(i64 value);

u64 wait_process(process_t *process, i64 &ret);

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

process_t *find_pid(process_id pid);
thread_t *find_tid(process_t *process, thread_id tid);

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
