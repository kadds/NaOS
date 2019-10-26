#include "kernel/task.hpp"
#include "kernel/arch/cpu.hpp"
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
#include "kernel/schedulers/time_span_scheduler.hpp"

#include "kernel/timer.hpp"
#include "kernel/ucontext.hpp"
namespace task
{

using thread_list_t = util::linked_list<thread_t *>;
using process_list_t = util::linked_list<process_t *>;
using thread_list_node_allocator_t = memory::list_node_cache_allocator<thread_list_t>;
using process_list_node_allocator_t = memory::list_node_cache_allocator<process_list_t>;

thread_list_node_allocator_t *thread_list_cache_allocator;
process_list_node_allocator_t *process_list_cache_allocator;

process_list_t *global_process_list;
lock::spinlock_t process_list_lock;

memory::SlabObjectAllocator *thread_t_allocator;
memory::SlabObjectAllocator *process_t_allocator;
memory::SlabObjectAllocator *mm_info_t_allocator;
memory::SlabObjectAllocator *register_info_t_allocator;

thread_t *idle_task;

inline void *new_kernel_stack() { return memory::KernelBuddyAllocatorV->allocate(memory::kernel_stack_size, 0); }

inline void delete_kernel_stack(void *p) { memory::KernelBuddyAllocatorV->deallocate(p); }

inline process_t *new_kernel_process()
{
    uctx::SpinLockUnInterruptableContext icu(process_list_lock);
    process_t *process = memory::New<process_t>(process_t_allocator);
    process->thread_list = memory::New<thread_list_t>(memory::KernelCommonAllocatorV, thread_list_cache_allocator);
    process->mm_info = nullptr;
    global_process_list->push_back(process);
    return process;
}

inline process_t *new_process()
{
    uctx::SpinLockUnInterruptableContext icu(process_list_lock);
    process_t *process = memory::New<process_t>(process_t_allocator);
    process->thread_list = memory::New<thread_list_t>(memory::KernelCommonAllocatorV, thread_list_cache_allocator);
    process->mm_info = memory::New<mm_info_t>(mm_info_t_allocator);
    global_process_list->push_back(process);
    return process;
}

inline void delete_process(process_t *p)
{
    uctx::SpinLockUnInterruptableContext icu(process_list_lock);
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
    uctx::SpinLockUnInterruptableContext icu(p->thread_list_lock);

    thread_t *thd = memory::New<thread_t>(thread_t_allocator);
    thd->process = p;
    ((thread_list_t *)p->thread_list)->push_back(thd);
    register_info_t *register_info = memory::New<register_info_t>(register_info_t_allocator);
    thd->register_info = register_info;
    return thd;
}

inline void delete_thread(thread_t *thd)
{
    uctx::SpinLockUnInterruptableContext icu(thd->process->thread_list_lock);

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
    uctx::UnInterruptableContext icu;
    process_list_cache_allocator = memory::New<process_list_node_allocator_t>(memory::KernelCommonAllocatorV);
    thread_list_cache_allocator = memory::New<thread_list_node_allocator_t>(memory::KernelCommonAllocatorV);

    global_process_list = memory::New<process_list_t>(memory::KernelCommonAllocatorV, process_list_cache_allocator);

    thread_t_allocator = memory::New<memory::SlabObjectAllocator>(
        memory::KernelCommonAllocatorV, NewSlabGroup(memory::global_object_slab_domain, thread_t, 8, 0));

    process_t_allocator = memory::New<memory::SlabObjectAllocator>(
        memory::KernelCommonAllocatorV, NewSlabGroup(memory::global_object_slab_domain, process_t, 8, 0));

    mm_info_t_allocator = memory::New<memory::SlabObjectAllocator>(
        memory::KernelCommonAllocatorV, NewSlabGroup(memory::global_object_slab_domain, mm_info_t, 8, 0));

    using arch::task::register_info_t;
    register_info_t_allocator = memory::New<memory::SlabObjectAllocator>(
        memory::KernelCommonAllocatorV, NewSlabGroup(memory::global_object_slab_domain, register_info_t, 8, 0));

    // init for kernel process
    process_t *process = new_kernel_process();
    process->pid = 0;
    process->parent_pid = 0;

    thread_t *thd = new_thread(process);
    thd->tid = 0;
    thd->state = thread_state::running;
    thd->static_priority = 125;
    thd->dynamic_priority = 0;

    arch::task::init(thd, thd->register_info);
    idle_task = thd;
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
        if (flags & fork_flags::kernel_thread)
        {
            process = new_kernel_process();
        }
        else
        {
            process = new_process();
        }
        // fork thread new procress
        process->parent_pid = current_process()->pid;
        process->pid = current_process()->pid + 1;
    }
    thread_t *thd = new_thread(process);
    thd->state = thread_state::ready;
    thd->tid = current()->tid + 1;
    void *stack = new_kernel_stack();
    void *stack_top = (char *)stack + memory::kernel_stack_size;
    thd->kernel_stack_top = stack_top;

    arch::task::do_fork(thd, (void *)func, args);
    scheduler::add(thd);
    return 0;
}

u64 do_exec(exec_segment code_segment, const char *args, const char *env, flag_t flags)
{
    auto thd = current();
    auto mm_info = thd->process->mm_info;
    auto &vm_paging = mm_info->mmu_paging;
    using namespace memory::vm;
    using namespace arch::task;

    // stack mapping, stack size 8MB
    auto stack_vm = vm_paging.get_vma().allocate_map(
        memory::user_stack_maximum_size, flags::readable | flags::writeable | flags::expand_low | flags::user_mode);

    thd->user_stack_top = (void *)stack_vm->end;
    thd->user_stack_bottom = (void *)stack_vm->start;

    code_segment.length = (code_segment.length + memory::page_size - 1) & ~(memory::page_size - 1);

    u64 code_end = memory::user_code_bottom_address + code_segment.length;
    // code mapping
    auto code_vm = vm_paging.get_vma().add_map(memory::user_code_bottom_address, code_end,
                                               flags::readable | flags::exec | flags::user_mode);

    mm_info->mmu_paging.map_area_phy(code_vm, (void *)code_segment.start_offset);
    // head mapping
    vm_paging.get_vma().add_map(code_end, code_end + 0x1000,
                                flags::readable | flags::writeable | flags::expand_high | flags::user_mode);

    mm_info->app_code_start = (void *)memory::user_code_bottom_address;
    thd->running_code_entry_address = mm_info->app_code_start;

    mm_info->app_head_start = (void *)code_end;
    mm_info->app_current_head = (char *)code_end + 0x1000;

    mm_info->mmu_paging.load_paging();

    return arch::task::do_exec(thd);
}

u64 do_exec(const char *filename, const char *args, const char *env, flag_t flags)
{
    fs::vfs::file *f = fs::vfs::open(filename, fs::vfs::mode::bin | fs::vfs::mode::read, 0);
    u64 file_size = fs::vfs::size(f);
    void *buffer = memory::KernelBuddyAllocatorV->allocate(file_size, 0);
    fs::vfs::read(f, (byte *)buffer, file_size);
    fs::vfs::close(f);
    exec_segment segment;
    segment.start_offset = (u64)memory::kernel_virtaddr_to_phyaddr(buffer);
    segment.length = file_size;
    return do_exec(segment, args, env, flags);
}

struct sleep_timer_struct
{
    u64 start;
    u64 end;
    thread_t *thd;
};

void sleep_callback_func(u64 pass, u64 data)
{
    sleep_timer_struct *st = (sleep_timer_struct *)data;
    if (st != nullptr)
    {
        uctx::UnInterruptableContext icu;
        st->thd->state = thread_state::ready;
        scheduler::update(st->thd);
        memory::Delete<>(memory::KernelCommonAllocatorV, st);
        return;
    }
    return;
}

void do_sleep(u64 milliseconds)
{
    uctx::UnInterruptableContext icu;
    if (milliseconds == 0)
    {
        current()->attributes |= task::thread_attributes::need_schedule;
        scheduler::update(current());
        task::yield_preempt();
    }
    else
    {
        auto start = timer::get_high_resolution_time();
        auto end = start + milliseconds * 1000;
        current()->state = thread_state::interruptable;
        sleep_timer_struct *st = memory::New<sleep_timer_struct>(memory::KernelCommonAllocatorV);
        st->start = start;
        st->end = end;
        st->thd = current();
        timer::add_watcher(milliseconds * 1000, sleep_callback_func, (u64)st);

        current()->attributes |= task::thread_attributes::need_schedule;
        scheduler::update(current());
    }
}

NoReturn void do_exit(u64 value)
{
    thread_t *thd = current();
    for (;;)
    {
        thd->state = thread_state::stop;
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
    scheduler::time_span_scheduler *scheduler =
        memory::New<scheduler::time_span_scheduler>(memory::KernelCommonAllocatorV);
    scheduler::set_scheduler(scheduler);

    task::builtin::idle::main(args);
}

process_t *find_pid(process_id pid)
{
    for (auto pro : *global_process_list)
    {
        if (pro->pid == pid)
            return pro;
    }
    return nullptr;
}

thread_t *find_tid(process_t *process, thread_id tid)
{
    auto &list = *(thread_list_t *)process->thread_list;
    for (auto thd : list)
    {
        if (thd->tid == tid)
            return thd;
    }
    return nullptr;
}

thread_t *get_idle_task() { return idle_task; }

void switch_thread(thread_t *old, thread_t *new_task)
{
    if (old->process != new_task->process && new_task->process->mm_info != nullptr)
        new_task->process->mm_info->mmu_paging.load_paging();
    _switch_task(old->register_info, new_task->register_info, new_task);
}

void yield_preempt()
{
    thread_t *thd = current();
    if (likely(thd != nullptr))
    {
        if (thd->preempt_data.preemptible() && thd->attributes & thread_attributes::need_schedule)
        {
            scheduler::schedule();
        }
    }
}

ExportC void yield_preempt_schedule() { yield_preempt(); }

void disable_preempt()
{
    thread_t *thd = current();
    if (likely(thd != nullptr))
    {
        thd->preempt_data.disable_preempt();
    }
}
void enable_preempt()
{
    thread_t *thd = current();
    if (likely(thd != nullptr))
    {
        thd->preempt_data.enable_preempt();
    }
}
} // namespace task
