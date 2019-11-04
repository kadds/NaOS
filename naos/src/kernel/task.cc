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

#include "kernel/fs/vfs/file.hpp"
#include "kernel/fs/vfs/vfs.hpp"
#include "kernel/kernel.hpp"
#include "kernel/scheduler.hpp"
#include "kernel/schedulers/time_span_scheduler.hpp"

#include "kernel/timer.hpp"
#include "kernel/ucontext.hpp"
#include "kernel/util/id_generator.hpp"

#include "kernel/mm/vm.hpp"
#include "kernel/task/binary_handle/bin_handle.hpp"
#include "kernel/task/binary_handle/elf.hpp"
using mm_info_t = memory::vm::info_t;
namespace task
{
const thread_id max_thread_id = 0x10000;

const process_id max_process_id = 0x100000;

const group_id max_group_id = 0x10000;

using thread_list_t = util::linked_list<thread_t *>;
using process_list_t = util::linked_list<process_t *>;
using thread_list_node_allocator_t = memory::list_node_cache_allocator<thread_list_t>;
using process_list_node_allocator_t = memory::list_node_cache_allocator<process_list_t>;

const u64 process_id_param[] = {0x1000, 0x8000, 0x40000, 0x80000};
using process_id_generator_t = util::id_level_generator<sizeof(process_id_param) / sizeof(u64)>;
process_id_generator_t *process_id_generator;

const u64 thread_id_param[] = {0x1000, 0x8000, 0x40000};
using thread_id_generator_t = util::id_level_generator<sizeof(thread_id_param) / sizeof(u64)>;

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
    auto id = process_id_generator->next();
    if (id == util::null_id)
        return nullptr;
    process_t *process = memory::New<process_t>(process_t_allocator);
    process->pid = id;
    process->thread_list = memory::New<thread_list_t>(memory::KernelCommonAllocatorV, thread_list_cache_allocator);
    process->mm_info = memory::kernel_vm_info;
    process->thread_id_gen = memory::New<thread_id_generator_t>(memory::KernelCommonAllocatorV, thread_id_param);
    global_process_list->push_back(process);
    return process;
}

inline process_t *new_process()
{
    uctx::SpinLockUnInterruptableContext icu(process_list_lock);
    auto id = process_id_generator->next();
    if (id == util::null_id)
        return nullptr;
    process_t *process = memory::New<process_t>(process_t_allocator);
    process->pid = id;
    process->thread_list = memory::New<thread_list_t>(memory::KernelCommonAllocatorV, thread_list_cache_allocator);
    process->mm_info = memory::New<mm_info_t>(mm_info_t_allocator);
    process->thread_id_gen = memory::New<thread_id_generator_t>(memory::KernelCommonAllocatorV, thread_id_param);
    global_process_list->push_back(process);
    return process;
}

inline void delete_process(process_t *p)
{
    uctx::SpinLockUnInterruptableContext icu(process_list_lock);
    if (p->mm_info != nullptr)
        memory::Delete(mm_info_t_allocator, (mm_info_t *)p->mm_info);

    memory::Delete<thread_list_t>(memory::KernelCommonAllocatorV, (thread_list_t *)p->thread_list);
    auto node = global_process_list->find(p);
    if (likely(node != global_process_list->end()))
    {
        global_process_list->remove(node);
    }
    process_id_generator->collect(p->pid);
    memory::Delete<>(process_t_allocator, p);
}

inline thread_t *new_thread(process_t *p)
{
    using arch::task::register_info_t;
    uctx::SpinLockUnInterruptableContext icu(p->thread_list_lock);

    auto id = ((thread_id_generator_t *)p->thread_id_gen)->next();
    if (unlikely(id == util::null_id))
    {
        return nullptr;
    }

    thread_t *thd = memory::New<thread_t>(thread_t_allocator);
    thd->process = p;
    ((thread_list_t *)p->thread_list)->push_back(thd);
    register_info_t *register_info = memory::New<register_info_t>(register_info_t_allocator);
    thd->register_info = register_info;
    thd->tid = id;
    return thd;
}

inline void delete_thread(thread_t *thd)
{
    uctx::SpinLockUnInterruptableContext icu(thd->process->thread_list_lock);

    using arch::task::register_info_t;

    auto thd_list = ((thread_list_t *)thd->process->thread_list);
    thd_list->remove(thd_list->find(thd));

    ((thread_id_generator_t *)thd->process->thread_id_gen)->collect(thd->tid);
    memory::Delete<>(register_info_t_allocator, thd->register_info);
    memory::Delete<>(thread_t_allocator, thd);
}

using file_array_t = util::array<fs::vfs::file *>;

resource_table_t::resource_table_t()
    : files(memory::New<file_array_t>(memory::KernelCommonAllocatorV, memory::KernelMemoryAllocatorV))
{
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

    process_id_generator = memory::New<process_id_generator_t>(memory::KernelCommonAllocatorV, process_id_param);

    // init for kernel process
    process_t *process = new_kernel_process();
    process->parent_pid = 0;

    thread_t *thd = new_thread(process);
    thd->state = thread_state::running;
    thd->static_priority = 125;
    thd->dynamic_priority = 0;

    arch::task::init(thd, thd->register_info);
    idle_task = thd;
    trace::debug("idle process (pid=", process->pid, ") thread (tid=", thd->tid, ") init...");
    bin_handle::init();
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
            /// copy the kernel page table
            ((memory::vm::info_t *)process->mm_info)->mmu_paging.sync_kernel();
        }
        // fork thread new procress
        process->parent_pid = current_process()->pid;
    }
    thread_t *thd = new_thread(process);
    thd->state = thread_state::ready;
    void *stack = new_kernel_stack();
    void *stack_top = (char *)stack + memory::kernel_stack_size;
    thd->kernel_stack_top = stack_top;

    arch::task::do_fork(thd, (void *)func, args);
    scheduler::add(thd);
    if (process == current_process())
    {
        trace::debug("New thread forked tid=", thd->tid, ", pid=", process->pid);
    }
    else
    {
        trace::debug("New process(thread) forked tid=", thd->tid, ", pid=", process->pid,
                     ", parent pid=", process->parent_pid);
    }
    return 0;
}

u64 do_exec(fs::vfs::file *file, const char *args, const char *env, flag_t flags)
{
    auto thd = current();
    {
        uctx::SpinLockUnInterruptableContext icu(thd->process->thread_list_lock);
        if (((thread_list_t *)thd->process->thread_list)->size() > 1)
        {
            return 2;
        }
    }

    auto old_mm = (mm_info_t *)thd->process->mm_info;
    thd->process->mm_info = memory::New<mm_info_t>(mm_info_t_allocator);
    auto mm_info = (mm_info_t *)thd->process->mm_info;
    auto &vm_paging = mm_info->mmu_paging;
    byte *header = (byte *)memory::KernelCommonAllocatorV->allocate(128, 8);
    file->move(0);
    file->read(header, 128);
    bin_handle::execute_info exec_info;
    if (flags & exec_flags::binary_file)
    {
        bin_handle::load_bin(header, file, old_mm, &exec_info);
    }
    else if (!bin_handle::load(header, file, old_mm, &exec_info))
    {
        trace::info("Can't load execute file.");
        memory::Delete<>(mm_info_t_allocator, mm_info);
        thd->process->mm_info = old_mm;
        memory::KernelCommonAllocatorV->deallocate(header);

        return 1;
    }
    memory::KernelCommonAllocatorV->deallocate(header);
    mm_info->mmu_paging.sync_kernel();

    thd->user_stack_top = exec_info.stack_top;
    thd->user_stack_bottom = exec_info.stack_bottom;
    if (old_mm != nullptr)
    {
        memory::Delete<>(mm_info_t_allocator, (mm_info_t *)old_mm);
    }
    vm_paging.load_paging();
    file->remove_ref();
    return arch::task::do_exec(thd, exec_info.entry_start_address);
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

void do_exit(u64 value)
{
    thread_t *thd = current();
    thd->register_info->trap_vector = value;
    thd->attributes = thread_attributes::need_schedule;
    thd->state = thread_state::stop;
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
    uctx::SpinLockUnInterruptableContext icu(process->thread_list_lock);
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
    if (old->process != new_task->process && old->process->mm_info != new_task->process->mm_info)
        ((mm_info_t *)new_task->process->mm_info)->mmu_paging.load_paging();
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
