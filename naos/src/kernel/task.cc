#include "kernel/task.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/arch/task.hpp"
#include "kernel/arch/tss.hpp"

#include "kernel/idle_task.hpp"
#include "kernel/init_task.hpp"
#include "kernel/mm/buddy.hpp"
#include "kernel/mm/list_node_cache.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/mm/slab.hpp"
#include "kernel/util/linked_list.hpp"
#include "kernel/util/memory.hpp"

#include "kernel/fs/vfs/vfs.hpp"
#include "kernel/kernel.hpp"
#include "kernel/scheduler.hpp"
#include "kernel/schedulers/fix_time_scheduler.hpp"

namespace task
{
thread_list_cache_t *thread_list_cache;
process_list_cache_t *global_process_list_cache;
process_list_t *global_process_list;

thread_list_node_cache_allocator_t *thread_list_node_cache_allocator;
process_list_node_cache_allocator_t *global_process_list_node_cache_allocator;

memory::SlabObjectAllocator *thread_t_allocator;
memory::SlabObjectAllocator *process_t_allocator;
memory::SlabObjectAllocator *mm_info_t_allocator;
memory::SlabObjectAllocator *register_info_t_allocator;

inline void *new_kernel_stack() { return memory::KernelBuddyAllocatorV->allocate(arch::task::kernel_stack_size, 0); }

inline void delete_kernel_stack(void *p) { memory::KernelBuddyAllocatorV->deallocate(p); }

inline process_t *new_kernel_process()
{
    process_t *process = memory::New<process_t>(process_t_allocator);
    process->thread_list = memory::New<thread_list_t>(memory::KernelCommonAllocatorV, thread_list_node_cache_allocator);
    process->mm_info = nullptr;
    global_process_list->push_back(process);
    return process;
}

inline process_t *new_process()
{
    process_t *process = memory::New<process_t>(process_t_allocator);
    process->thread_list = memory::New<thread_list_t>(memory::KernelCommonAllocatorV, thread_list_node_cache_allocator);
    process->mm_info = memory::New<mm_info_t>(mm_info_t_allocator);
    global_process_list->push_back(process);
    return process;
}

inline void delete_process(process_t *p)
{
    if (p->mm_info != nullptr)
    {
        memory::Delete(mm_info_t_allocator, p->mm_info);
    }
    memory::Delete<thread_list_t>(memory::KernelCommonAllocatorV, (thread_list_t *)p->thread_list);
    auto node = global_process_list->find(p);
    if (likely(node != global_process_list->end()))
    {
        global_process_list->remove(node);
    }
    memory::Delete<>(process_t_allocator, p);
}

inline thread_t *new_thread(process_t *p)
{
    using arch::task::register_info_t;

    thread_t *thd = memory::New<thread_t>(thread_t_allocator);
    thd->process = p;
    ((thread_list_t *)p->thread_list)->push_back(thd);
    register_info_t *register_info = memory::New<register_info_t>(register_info_t_allocator);
    thd->register_info = register_info;
    return thd;
}

inline void delete_thread(thread_t *thd)
{
    using arch::task::register_info_t;

    auto thd_list = ((thread_list_t *)thd->process->thread_list);
    auto node = thd_list->find(thd);
    if (likely(node != thd_list->end()))
    {
        thd_list->remove(node);
    }

    memory::Delete<>(register_info_t_allocator, thd->register_info);
    memory::Delete<>(thread_t_allocator, thd);
}

void init()
{
    // init all allocators and caches
    thread_list_cache = memory::New<thread_list_cache_t>(memory::KernelCommonAllocatorV, memory::KernelBuddyAllocatorV);
    thread_list_node_cache_allocator =
        memory::New<thread_list_node_cache_allocator_t>(memory::KernelCommonAllocatorV, thread_list_cache);

    global_process_list_cache =
        memory::New<process_list_cache_t>(memory::KernelCommonAllocatorV, memory::KernelBuddyAllocatorV);
    global_process_list_node_cache_allocator =
        memory::New<process_list_node_cache_allocator_t>(memory::KernelCommonAllocatorV, global_process_list_cache);
    global_process_list =
        memory::New<process_list_t>(memory::KernelCommonAllocatorV, global_process_list_node_cache_allocator);

    thread_t_allocator = memory::New<memory::SlabObjectAllocator>(
        memory::KernelCommonAllocatorV, NewSlabGroup(global_object_slab_domain, thread_t, 8, 0));

    process_t_allocator = memory::New<memory::SlabObjectAllocator>(
        memory::KernelCommonAllocatorV, NewSlabGroup(global_object_slab_domain, process_t, 8, 0));

    mm_info_t_allocator = memory::New<memory::SlabObjectAllocator>(
        memory::KernelCommonAllocatorV, NewSlabGroup(global_object_slab_domain, mm_info_t, 8, 0));

    using arch::task::register_info_t;
    register_info_t_allocator = memory::New<memory::SlabObjectAllocator>(
        memory::KernelCommonAllocatorV, NewSlabGroup(global_object_slab_domain, register_info_t, 8, 0));

    // init for kernel process
    process_t *process = new_kernel_process();
    process->pid = 0;
    process->parent_pid = 0;

    thread_t *thd = new_thread(process);
    thd->tid = 0;
    thd->state = thread_state::interruptable;

    arch::task::init(thd, thd->register_info);
    scheduler::fix_time_scheduler *scheduler =
        memory::New<scheduler::fix_time_scheduler>(memory::KernelCommonAllocatorV);
    scheduler::set_scheduler(scheduler);
}

u64 do_fork(kernel_thread_start_func *func, u64 args, flag_t flags)
{
    process_t *process;
    if (flags & fork_flags::vm_sharing)
    {
        // fork thread
        process = current_process();
    }
    else
    {
        // fork thread new procress
        process = new_process();
        process->parent_pid = current_process()->pid;
        process->pid = current_process()->pid + 1;
    }
    thread_t *thd = new_thread(process);
    thd->state = thread_state::ready;
    thd->tid = current()->tid + 1;
    void *stack = new_kernel_stack();
    void *stack_high = (char *)stack + arch::task::kernel_stack_size;
    thd->kernel_stack_high = stack_high;
    arch::task::do_fork(thd, stack, *thd->register_info, (void *)func, args);
    scheduler::add(thd);
    return 0;
}

u64 do_exec(exec_segment code_segment, const char *args, const char *env, flag_t flags)
{
    auto thd = current();
    auto mm_info = thd->process->mm_info;
    using namespace memory::vm;
    using namespace arch::task;

    // stack mapping, stack size 8MB
    mm_info->mmu_paging.add_vm_area((void *)user_stack_end_address, (void *)user_stack_start_address,
                                    flags::readable | flags::writeable | flags::expand_low | flags::user_mode);

    mm_info->app_stack_start = (void *)user_stack_start_address;
    mm_info->app_current_stack = (void *)user_stack_end_address;

    code_segment.length = (code_segment.length + memory::page_size - 1) & ~(memory::page_size - 1);

    void *code_end = (void *)(user_code_start_address + code_segment.length);
    // code mapping
    auto code_vm = mm_info->mmu_paging.add_vm_area((void *)user_code_start_address, code_end,
                                                   flags::readable | flags::exec | flags::user_mode);

    mm_info->mmu_paging.map_area_phy(code_vm, (void *)code_segment.start_offset);

    // head mapping
    mm_info->mmu_paging.add_vm_area(code_end, (char *)code_end + 0x1000,
                                    flags::readable | flags::writeable | flags::expand_low | flags::user_mode);

    mm_info->app_code_start = (void *)user_code_start_address;
    mm_info->app_code_entry = mm_info->app_code_start;

    mm_info->app_head_start = code_end;
    mm_info->app_current_head = (char *)code_end + 0x1000;

    thd->register_info->rip = (u64)&_sys_ret;
    thd->register_info->rsp0 = (u64)&_sys_ret;

    mm_info->mmu_paging.load_paging();

    return arch::task::do_exec(mm_info->app_code_entry, mm_info->app_stack_start, thd->kernel_stack_high);
}

u64 do_exec(const char *filename, const char *args, const char *env, flag_t flags)
{
    fs::vfs::file *f = fs::vfs::open(filename, fs::vfs::mode::bin | fs::vfs::mode::read, 0);
    u64 file_size = fs::vfs::size(f);
    void *buffer = memory::KernelBuddyAllocatorV->allocate(file_size, 0);
    fs::vfs::read(f, (byte *)buffer, file_size);

    exec_segment segment;
    segment.start_offset = (u64)memory::kernel_virtaddr_to_phyaddr(buffer);
    segment.length = file_size;
    return do_exec(segment, args, env, flags);
}

NoReturn void do_exit(u64 value)
{
    thread_t *thd = current();
    for (;;)
    {
        thd->state = thread_state::stop;
        scheduler::schedule();
    }
}

void destroy_thread(thread_t *thread)
{
    scheduler::remove(thread);
    process_t *process = thread->process;
    delete_thread(thread);
    if (((thread_list_t *)process->thread_list)->empty())
    {
        destroy_process(process);
    }
}

void destroy_process(process_t *process) { delete_process(process); }

void start_task_idle(const kernel_start_args *args)
{
    task::builtin::idle::main(args);
    for (;;)
        scheduler::schedule();
}

process_t *find_pid(process_id pid)
{
    for (auto it = global_process_list->begin(); it != global_process_list->end(); it = global_process_list->next(it))
    {
        if (it->element->pid == pid)
            return it->element;
    }
    return nullptr;
}

thread_t *find_tid(process_t *process, thread_id tid)
{
    auto list = (thread_list_t *)process->thread_list;
    for (auto it = list->begin(); it != list->end(); it = list->next(it))
    {
        if (it->element->tid == tid)
            return it->element;
    }
    return nullptr;
}

thread_t *get_idle_task() { return find_tid(find_pid(0), 0); }

void switch_thread(thread_t *old, thread_t *new_task)
{
    if (old->process != new_task->process)
        new_task->process->mm_info->mmu_paging.load_paging();
    _switch_task(old->register_info, new_task->register_info);
}

} // namespace task
