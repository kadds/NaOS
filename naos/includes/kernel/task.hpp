#pragma once
#include "arch/task.hpp"
#include "common.hpp"
#include "mm/vm.hpp"

namespace task
{

typedef void kernel_thread_start_func(u64 args);
typedef u64 user_main_func(char *args, u64 argc);

struct mm_info_t
{
    memory::vm::mmu_paging mmu_paging;

    // high
    void *kernel_stack_start;

    // high
    void *app_stack_start;
    void *app_current_stack;
    void *app_code_start;
    void *app_code_entry;

    void *app_head_start;
    void *app_current_head;
};
enum class task_state : u64
{
    running,
    wait,
    // more...
};

struct task_t
{
    volatile task_state state;
    ::arch::task::register_info_t *register_info;
    mm_info_t *mm_info;
    u64 pid;
    u64 tid;
    u64 gid;
    u64 parent_pid;
    i64 priority;
};

struct exec_segment
{
    u64 start_offset;
    u64 length;
};

void init();
void start_task_idle(const kernel_start_args *args);
int kernel_thread(kernel_thread_start_func *function, u64 arg);
void switch_task(task_t *old, task_t *new_task);
u64 do_exec(exec_segment code_segment, char *args, char *env);

u64 do_fork(kernel_thread_start_func *func, u64 args, u64 flags);
task_t *find_pid(u64 pid);

inline task_t *current() { return (task_t *)arch::task::current_task(); }
inline task_t *current(void *stack) { return (task_t *)arch::task::get_task(stack); }

} // namespace task
