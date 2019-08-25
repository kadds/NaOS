#pragma once
#include "arch/task.hpp"
#include "common.hpp"
#include "mm/vm.hpp"

namespace task
{
typedef void kernel_thread_start_func(u64 args);
// 32 kb
const int kernel_stack_page_count = 8;

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
    register_info_t *register_info;
    mm_info_t *mm_info;
    u64 pid;
    u64 tid;
    u64 gid;
    u64 parent_pid;
    i64 priority;
};
struct thread_info_t
{
    u64 magic_wall;
    task_t *task;
    u64 magic_end_wall;
    bool is_valid() { return magic_wall == 0xFFCCFFCCCCFFCCFF && magic_end_wall == 0x0AEEAAEEEEAAEE0A; }
    thread_info_t()
        : magic_wall(0xFFCCFFCCCCFFCCFF)
        , magic_end_wall(0x0AEEAAEEEEAAEE0A)
    {
    }
};

void init(void *rsp);
void start_task_zero();
int kernel_thread(kernel_thread_start_func *function, u64 arg);

void create(void *rip, void *ss);
void switch_task(task_t *old, task_t *new_task);

} // namespace task
