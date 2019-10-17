#pragma once
#include "arch/task.hpp"
#include "common.hpp"
#include "mm/vm.hpp"
#include <atomic>
namespace task
{

typedef void kernel_thread_start_func(u64 args);
typedef u64 user_main_func(char *args, u64 argc);
typedef u64 handle_t;
typedef u64 thread_id;
typedef u64 process_id;

struct mm_info_t
{
    memory::vm::mmu_paging mmu_paging;

    void *app_code_start;
    void *app_head_start;
    void *app_current_head;
    void *map_start;
    void *map_end;
};

enum class thread_state : u8
{
    ready = 0,       ///< Can schedule
    running,         ///< Running at a CPU
    interruptable,   ///< Sleep
    uninterruptable, ///< Uninterruptable sleep
    stop,            ///< Can't reschedule
    destroy,
};

struct handle_table_t
{
};

/// The process struct
struct process_t
{
    process_id pid;
    u64 parent_pid;           ///< The parent process id
    mm_info_t *mm_info;       ///< Memory map infomation
    handle_table_t res_table; ///< Resource table
    void *thread_list;        ///< The threads belong to process
    void *schedule_data;
};
namespace thread_attributes
{
enum attributes
{
    need_schedule = 1,

};
} // namespace thread_attributes
struct preempt_t
{
    u16 preempt_counter;
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
    void *running_code_entry_address;

    /// from 0 - 255, 0 is the highest priority, 100 - 139 to user thread, default is 125
    i8 static_priority;

    /// dynamic priority
    ///
    /// -128 - 128, miniumu value means this is a CPU bound thread, maximum value means is a IO
    /// bound thread, the default value is 0.
    i8 dynamic_priority;
    u16 cpuid;
    u64 sleep_time;
    preempt_t preempt_data;
};

struct exec_segment
{
    u64 start_offset;
    u64 length;
};

void init();
void start_task_idle(const kernel_start_args *args);
int kernel_thread(kernel_thread_start_func *function, u64 arg);
void switch_thread(thread_t *old, thread_t *new_task);

namespace exec_flags
{
enum exec_flags : flag_t
{
    immediately = 1,
};
} // namespace exec_flags
u64 do_fork(kernel_thread_start_func *func, u64 args, flag_t flag);

u64 do_exec(exec_segment code_segment, const char *args, const char *env, flag_t flag);
u64 do_exec(const char *filename, const char *args, const char *env, flag_t flag);

namespace fork_flags
{
enum fork_flags : flag_t
{
    vm_sharing = 1,
};
} // namespace fork_flags

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

} // namespace task
