#pragma once
#include "arch/task.hpp"
#include "common.hpp"
#include "mm/vm.hpp"

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

    // high
    void *app_stack_start;
    void *app_current_stack;
    void *app_code_start;
    void *app_code_entry;

    void *app_head_start;
    void *app_current_head;
};

enum class thread_state : u8
{
    ready = 0,       // schedule
    running,         // running at a CPU
    interruptable,   // sleep
    uninterruptable, // uninterrupt sleep
    stop,
    destroy,
};

struct handle_table_t
{
};

struct process_t
{
    process_id pid;
    u64 parent_pid;
    mm_info_t *mm_info;
    handle_table_t res_table;
    void *thread_list;
    void *schedule_data;
};

struct thread_t
{
    volatile thread_state state;
    volatile u32 attributes;
    thread_id tid;
    process_t *process;
    i64 priority;
    void *schedule_data;
    ::arch::task::register_info_t *register_info;
    void *kernel_stack_high;
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

u64 do_exec(exec_segment code_segment, const char *args, const char *env, flag_t flag);
u64 do_exec(const char *filename, const char *args, const char *env, flag_t flag);

namespace fork_flags
{
enum fork_flags : flag_t
{
    vm_sharing = 1,
};
} // namespace fork_flags

u64 do_fork(kernel_thread_start_func *func, u64 args, flag_t flag);
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

typedef util::linked_list<thread_t *> thread_list_t;
typedef list_node_cache<thread_list_t> thread_list_cache_t;
typedef memory::ListNodeCacheAllocator<thread_list_cache_t> thread_list_node_cache_allocator_t;

typedef util::linked_list<process_t *> process_list_t;
typedef list_node_cache<process_list_t> process_list_cache_t;
typedef memory::ListNodeCacheAllocator<process_list_cache_t> process_list_node_cache_allocator_t;

} // namespace task
