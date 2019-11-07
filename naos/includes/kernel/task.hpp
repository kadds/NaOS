#pragma once
#include "arch/task.hpp"
#include "common.hpp"
#include "id_defined.hpp"
#include "lock.hpp"
#include "util/array.hpp"
#include <atomic>

namespace fs::vfs
{
class file;
}
namespace task
{

typedef void (*kernel_thread_entry)(u64 arg);
typedef void (*userland_thread_entry)(u64 arg);

typedef void (*thread_start_func)(void *entry, u64 arg2);

/// 65536
extern const thread_id max_thread_id;

/// 1048576
extern const process_id max_process_id;

/// 65536
extern const group_id max_group_id;
using file_array_t = util::array<fs::vfs::file *>;

struct resource_table_t
{
    void *console_attribute;
    file_array_t *files;
    resource_table_t();
};

/// The process struct
struct process_t
{
    process_id pid;
    u64 parent_pid;             ///< The parent process id
    void *mm_info;              ///< Memory map infomation
    resource_table_t res_table; ///< Resource table
    void *thread_id_gen;
    lock::spinlock_t thread_list_lock;
    void *thread_list; ///< The threads belong to process
    void *schedule_data;
};

enum class thread_state : u8
{
    ready = 0,       ///< Can schedule
    running,         ///< Running at a CPU
    interruptable,   ///< Sleep
    uninterruptible, ///< Uninterruptible sleep
    stop,            ///< Can't reschedule
    destroy,
};

namespace thread_attributes
{
enum attributes : u32
{
    need_schedule = 1,
    block = 2,
};
} // namespace thread_attributes
struct preempt_t
{
  private:
    std::atomic_int preempt_counter;

  public:
    bool preemptible() { return preempt_counter == 0; }
    void enable_preempt() { --preempt_counter; }
    void disable_preempt() { ++preempt_counter; }
    preempt_t()
        : preempt_counter(0)
    {
    }
};
struct statistics_t
{
    u64 sleep_time;
    u64 kernel_time;
    u64 userland_time;
};

/// The thread struct
struct thread_t
{
    volatile thread_state state;
    volatile u32 attributes;
    thread_id tid;
    process_t *process;
    void *schedule_data;
    ::arch::task::register_info_t *register_info;
    /// The kernel context RSP value
    void *kernel_stack_top;
    void *user_stack_top;
    void *user_stack_bottom;

    /// from 0 - 255, 0 is the highest priority, 100 - 139 to user thread, default is 125
    u8 static_priority;
    /// dynamic priority
    ///
    /// -128 - 128, miniumu value means a CPU bound thread, maximum value means a IO
    /// bound thread, the default value is 0.
    i8 dynamic_priority;
    /// run in cpu core
    u32 cpuid;
    statistics_t statistics;
    preempt_t preempt_data;
};

void init();
void start_task_idle(const kernel_start_args *args);
void kernel_thread(kernel_thread_entry entry, u64 arg);
void userland_thread(userland_thread_entry entry, u64 arg);
void switch_thread(thread_t *old, thread_t *new_task);

namespace create_thread_flags
{
enum create_thread_flags : flag_t
{
    immediately = 1,

    noreturn = 4,
};
} // namespace create_thread_flags

namespace create_process_flags
{
enum create_process_flags : flag_t
{
    noreturn = 1,
    binary_file = 2,
};
} // namespace create_process_flags

thread_t *create_thread(process_t *process, thread_start_func start_func, void *entry, u64 arg, flag_t flags);
process_t *create_process(fs::vfs::file *file, const char *args, const char *env, flag_t flags);

void do_sleep(u64 milliseconds);

NoReturn void do_exit(u64 value);

void destroy_thread(thread_t *thread);
void destroy_process(process_t *process);

u64 wait_procress(process_id pid, u64 max_time);
u64 wait_thread(thread_id tid, u64 max_time);

process_t *find_pid(process_id pid);
thread_t *find_tid(process_t *process, thread_id tid);

thread_t *get_idle_task();

inline thread_t *current() { return (thread_t *)arch::task::current_task(); }
inline thread_t *current(void *stack) { return (thread_t *)arch::task::get_task(stack); }
inline process_t *current_process() { return current()->process; }
inline process_t *current_process(void *stack) { return current(stack)->process; }

void yield_preempt();

void disable_preempt();
void enable_preempt();
} // namespace task
