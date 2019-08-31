#include "kernel/task.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/arch/tss.hpp"
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
typedef util::linked_list<task_t *> task_list_t;
typedef list_node_cache<task_list_t> task_list_cache_t;

task_list_cache_t *list_cache;
task_list_t *task_list;
memory::ListNodeCacheAllocator<task_list_cache_t> *task_list_node_cache_allocator;
memory::BuddyAllocator *buddy_allocator;

void *new_stack() { return buddy_allocator->allocate(arch::task::stack_size, 0); }
void free_stack(void *p) { buddy_allocator->deallocate(p); }
void init()
{
    buddy_allocator =
        memory::New<memory::BuddyAllocator>(memory::KernelCommonAllocatorV, memory::zone_t::prop::present);
    list_cache = memory::New<task_list_cache_t>(memory::KernelCommonAllocatorV, buddy_allocator);
    task_list_node_cache_allocator =
        memory::New<memory::ListNodeCacheAllocator<task_list_cache_t>>(memory::KernelCommonAllocatorV, list_cache);
    task_list = memory::New<task_list_t>(memory::KernelCommonAllocatorV, task_list_node_cache_allocator);

    NewSlabGroup(global_object_slab_cache_pool, task_t, 8, 0);
    NewSlabGroup(global_object_slab_cache_pool, mm_info_t, 8, 0);
    using arch::task::register_info_t;
    NewSlabGroup(global_object_slab_cache_pool, register_info_t, 8, 0);

    memory::SlabObjectAllocator task_allocator(global_object_slab_cache_pool, "task_t");
    memory::SlabObjectAllocator mm_allocator(global_object_slab_cache_pool, "mm_info_t");
    memory::SlabObjectAllocator register_allocator(global_object_slab_cache_pool, "register_info_t");

    // init for idle task
    register_info_t *register_info = memory::New<register_info_t>(&register_allocator);
    task_t *task = memory::New<task_t>(&task_allocator);
    task->register_info = register_info;
    task->pid = 0;
    task->state = task_state::running;

    arch::task::init(task, register_info);

    task_list->push_back(task);
}

u64 do_fork(kernel_thread_start_func *func, u64 args, u64 flags)
{
    memory::SlabObjectAllocator task_allocator(global_object_slab_cache_pool, "task_t");
    memory::SlabObjectAllocator mm_allocator(global_object_slab_cache_pool, "mm_info_t");
    memory::SlabObjectAllocator register_allocator(global_object_slab_cache_pool, "register_info_t");
    task_t *task = memory::New<task_t>(&task_allocator);
    arch::task::regs_t regs;
    arch::task::register_info_t *register_info = memory::New<arch::task::register_info_t>(&register_allocator);

    *task = *current();

    task->pid++;
    task->register_info = register_info;

    void *stack = new_stack();
    void *stack_high = (char *)stack + arch::task::stack_size;
    arch::task::do_fork(task, stack_high, regs, *register_info, (void *)func, args);

    util::memcopy((char *)stack_high - sizeof(arch::task::regs_t), &regs, sizeof(arch::task::regs_t));

    mm_info_t *mm = memory::New<mm_info_t>(&mm_allocator);
    mm->kernel_stack_start_addr = stack_high;
    task->mm_info = mm;

    task_list->push_back(task);
    return 0;
}

void start_task_idle() { task::builtin::idle::main(0); }
task_t *find_pid(u64 pid)
{
    for (auto it = task_list->begin(); it != task_list->end(); it = task_list->next(it))
    {
        if (it->element->pid == pid)
            return it->element;
    }
    return nullptr;
}
void switch_task(task_t *old, task_t *new_task) { _switch_task(old->register_info, new_task->register_info); }

} // namespace task
