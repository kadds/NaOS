#pragma once
#include "arch/task.hpp"
#include "common.hpp"
#include "mm/vm.hpp"

namespace task
{

typedef void kernel_thread_start_func(u64 args);

struct mm_info_t
{
    memory::mmu_paging mmu_paging;

    // high
    void *kernel_stack_start_addr;

    void *app_head_addr;
    void *app_head_base_addr;
    void *app_stack_base_addr;
};
enum class task_state
{
    running,
    wait,
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

void init();
void start_task_idle();
int kernel_thread(kernel_thread_start_func *function, u64 arg);
void switch_task(task_t *old, task_t *new_task);
u64 do_fork(kernel_thread_start_func *func, u64 args, u64 flags);
task_t *find_pid(u64 pid);

inline task_t *current() { return (task_t *)arch::task::current_task(); }
} // namespace task
