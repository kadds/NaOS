#include "kernel/task.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/idle_task.hpp"
#include "kernel/init_task.hpp"
#include "kernel/mm/buddy.hpp"
#include "kernel/mm/list_node_cache.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/mm/slab.hpp"
#include "kernel/util/linked_list.hpp"
#include "kernel/util/memory.hpp"

namespace task
{
u64 stack_size;
typedef util::linked_list<task_t *> task_list_t;
typedef list_node_cache<task_list_t> task_list_cache_t;

task_list_cache_t *list_cache;
task_list_t *task_list;
memory::ListNodeCacheAllocator<task_list_cache_t> *task_list_node_cache_allocator;
memory::BuddyAllocator *buddy_allocator;

inline task_t *current()
{
    task_t *current = nullptr;
    __asm__ __volatile__("andq %%rsp,%0	\n\t" : "=r"(current) : "0"(~(stack_size - 1)));
    return current;
}

void *new_stack()
{
    memory::BuddyAllocator buddy_allocator(memory::zone_t::prop::present);
    return buddy_allocator.allocate(stack_size, 0);
}
void free_stack(void *p)
{
    memory::BuddyAllocator buddy_allocator(memory::zone_t::prop::present);
    buddy_allocator.deallocate(p);
}
void init(void *rsp)
{
    stack_size = kernel_stack_page_count * memory::page_size;
    buddy_allocator =
        memory::New<memory::BuddyAllocator>(memory::KernelCommonAllocatorV, memory::zone_t::prop::present);
    list_cache = memory::New<task_list_cache_t>(memory::KernelCommonAllocatorV, buddy_allocator);
    task_list_node_cache_allocator =
        memory::New<memory::ListNodeCacheAllocator<task_list_cache_t>>(memory::KernelCommonAllocatorV, list_cache);
    task_list = memory::New<task_list_t>(memory::KernelCommonAllocatorV, task_list_node_cache_allocator);

    NewSlabGroup(global_object_slab_cache_pool, task_t, 8, 0);
    NewSlabGroup(global_object_slab_cache_pool, mm_info_t, 8, 0);
    NewSlabGroup(global_object_slab_cache_pool, register_info_t, 8, 0);
    memory::SlabObjectAllocator mm_allocator(global_object_slab_cache_pool, "mm_info_t");
    memory::SlabObjectAllocator register_allocator(global_object_slab_cache_pool, "register_info_t");
    task_t *task_0 = current();
    task_0->pid = 0;
    task_0->register_info = memory::New<task::register_info_t>(&register_allocator);
    task_0->register_info->cr2 = 0;
    task_0->register_info->error_code = 0;
    task_0->register_info->fs = 0x10;
    task_0->register_info->gs = 0x10;
    task_0->register_info->rsp = (u64)rsp;
    task_0->register_info->rsp0 = (u64)rsp;
    task_0->register_info->trap_vector = 0;
    // task_0->mm_info = memory::New<mm_info_t>(&mm_allocator);
    // init idle task
    kernel_thread(task::builtin::idle::main, 0);
}
ExportC void task_do_exit(u64 exit_code)
{
    trace::debug("thread exit!");
    while (1)
        ;
}
u64 do_fork(task::regs_t *regs, u64 flags)
{
    memory::SlabObjectAllocator task_allocator(global_object_slab_cache_pool, "task_t");
    memory::SlabObjectAllocator mm_allocator(global_object_slab_cache_pool, "mm_info_t");
    memory::SlabObjectAllocator register_allocator(global_object_slab_cache_pool, "register_info_t");
    task_t *task = memory::New<task_t>(&task_allocator);
    register_info_t *register_info = memory::New<register_info_t>(&register_allocator);
    *task = *current();

    task->pid++;
    task->register_info = register_info;

    void *stack = new_stack();
    util::memcopy((char *)stack + stack_size - sizeof(regs_t), regs, sizeof(regs_t));

    mm_info_t *mm = memory::New<mm_info_t>(&mm_allocator);
    mm->kernel_stack_start_addr = (char *)stack + stack_size;
    task->mm_info = mm;

    thread_info_t *thread_info = new (stack) thread_info_t();
    thread_info->task = task;
    register_info->rip = regs->rip;
    register_info->rsp = (u64)mm->kernel_stack_start_addr - sizeof(regs_t);
    register_info->rsp0 = register_info->rsp;
    register_info->fs = 0x10;
    register_info->gs = 0x10;

    task_list->push_back(task);
    return 0;
}
int kernel_thread(kernel_thread_start_func *function, u64 arg)
{
    task::regs_t regs;
    util::memset(&regs, 0, sizeof(regs));

    regs.rbx = (u64)function;
    regs.rdx = (u64)arg;

    regs.ds = 0x10;
    regs.es = 0x10;
    regs.cs = 0x08;
    regs.ss = 0x10;
    regs.rflags = (1 << 9);
    regs.rip = (unsigned long)_kernel_thread;

    return do_fork(&regs, 0);
}

void create(void *rip, void *ss) {}
void start_task_zero() { _switch_task(current()->register_info, task_list->front()->register_info); }

} // namespace task
