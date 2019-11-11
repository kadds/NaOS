#include "kernel/task.hpp"
#include "kernel/arch/cpu.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/arch/task.hpp"

#include "kernel/mm/list_node_cache.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/mm/slab.hpp"
#include "kernel/mm/vm.hpp"

#include "kernel/util/array.hpp"
#include "kernel/util/hash_map.hpp"
#include "kernel/util/id_generator.hpp"
#include "kernel/util/memory.hpp"

#include "kernel/fs/vfs/file.hpp"
#include "kernel/fs/vfs/vfs.hpp"

#include "kernel/scheduler.hpp"
#include "kernel/schedulers/time_span_scheduler.hpp"

#include "kernel/timer.hpp"
#include "kernel/ucontext.hpp"

#include "kernel/task/binary_handle/bin_handle.hpp"
#include "kernel/task/binary_handle/elf.hpp"
#include "kernel/task/builtin/idle_task.hpp"
#include "kernel/task/builtin/soft_irq_task.hpp"

using mm_info_t = memory::vm::info_t;
namespace task
{
const thread_id max_thread_id = 0x10000;

const process_id max_process_id = 0x100000;

const group_id max_group_id = 0x10000;

using thread_list_t = util::linked_list<thread_t *>;
// using process_list_t = util::linked_list<process_t *>;
using thread_list_node_allocator_t = memory::list_node_cache_allocator<thread_list_t>;
// using process_list_node_allocator_t = memory::list_node_cache_allocator<process_list_t>;

const u64 process_id_param[] = {0x1000, 0x8000, 0x40000, 0x80000};
using process_id_generator_t = util::id_level_generator<sizeof(process_id_param) / sizeof(u64)>;
process_id_generator_t *process_id_generator;

const u64 thread_id_param[] = {0x1000, 0x8000, 0x40000};
using thread_id_generator_t = util::id_level_generator<sizeof(thread_id_param) / sizeof(u64)>;

thread_list_node_allocator_t *thread_list_cache_allocator;
// process_list_node_allocator_t *process_list_cache_allocator;

memory::SlabObjectAllocator *thread_t_allocator;
memory::SlabObjectAllocator *process_t_allocator;
memory::SlabObjectAllocator *mm_info_t_allocator;
memory::SlabObjectAllocator *register_info_t_allocator;

struct process_hash
{
    u64 operator()(process_id pid) { return pid; }
};

struct thread_hash
{
    u64 operator()(thread_id tid) { return tid; }
};

using process_map_t = util::hash_map<process_id, process_t *, process_hash>;
using thread_map_t = util::hash_map<process_id, process_t *, process_hash>;

process_map_t *global_process_map;
// process_list_t *global_process_list;
lock::spinlock_t process_list_lock;

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
    process->res_table.set_console_attribute(&trace::kernel_console_attribute);
    process->pid = id;
    process->thread_list = memory::New<thread_list_t>(memory::KernelCommonAllocatorV, thread_list_cache_allocator);
    process->mm_info = memory::kernel_vm_info;
    process->thread_id_gen = memory::New<thread_id_generator_t>(memory::KernelCommonAllocatorV, thread_id_param);
    global_process_map->insert(id, process);
    return process;
}

inline process_t *new_process()
{
    uctx::SpinLockUnInterruptableContext icu(process_list_lock);
    auto id = process_id_generator->next();
    if (id == util::null_id)
        return nullptr;
    process_t *process = memory::New<process_t>(process_t_allocator);
    process->res_table.set_console_attribute(memory::New<trace::console_attribute>(memory::KernelCommonAllocatorV));

    process->pid = id;
    process->thread_list = memory::New<thread_list_t>(memory::KernelCommonAllocatorV, thread_list_cache_allocator);
    process->mm_info = memory::New<mm_info_t>(mm_info_t_allocator);
    process->thread_id_gen = memory::New<thread_id_generator_t>(memory::KernelCommonAllocatorV, thread_id_param);
    global_process_map->insert(id, process);

    return process;
}

inline void delete_process(process_t *p)
{
    uctx::SpinLockUnInterruptableContext icu(process_list_lock);
    if (p->mm_info != nullptr)
        memory::Delete(mm_info_t_allocator, (mm_info_t *)p->mm_info);
    if (p->res_table.get_console_attribute() != &trace::kernel_console_attribute)
    {
        memory::Delete<>(memory::KernelCommonAllocatorV, p->res_table.get_console_attribute());
    }

    memory::Delete<thread_list_t>(memory::KernelCommonAllocatorV, (thread_list_t *)p->thread_list);
    global_process_map->remove(p->pid);
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

void init()
{
    uctx::UnInterruptableContext icu;
    // process_list_cache_allocator = memory::New<process_list_node_allocator_t>(memory::KernelCommonAllocatorV);
    thread_list_cache_allocator = memory::New<thread_list_node_allocator_t>(memory::KernelCommonAllocatorV);
    global_process_map = memory::New<process_map_t>(memory::KernelCommonAllocatorV, memory::KernelMemoryAllocatorV);
    // global_process_list = memory::New<process_list_t>(memory::KernelCommonAllocatorV, process_list_cache_allocator);

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

thread_t *create_thread(process_t *process, thread_start_func start_func, u64 arg0, u64 arg1, u64 arg2, flag_t flags)
{
    thread_t *thd = new_thread(process);
    thd->state = thread_state::ready;
    void *stack = new_kernel_stack();
    void *stack_top = (char *)stack + memory::kernel_stack_size;
    thd->kernel_stack_top = stack_top;
    auto &vma = ((mm_info_t *)process->mm_info)->vma;

    if (process->mm_info != memory::kernel_vm_info)
    {

        auto stack_vm = vma.allocate_map(memory::user_stack_maximum_size,
                                         memory::vm::flags::readable | memory::vm::flags::writeable |
                                             memory::vm::flags::expand | memory::vm::flags::user_mode,
                                         memory::vm::fill_expand_vm, 0);

        thd->user_stack_top = (void *)stack_vm->end;
        thd->user_stack_bottom = (void *)stack_vm->start;
    }

    arch::task::create_thread(thd, (void *)start_func, arg0, arg1, arg2, 0);
    scheduler::add(thd);
    return thd;
}

process_t *create_process(fs::vfs::file *file, thread_start_func start_func, u64 arg0, const char *args,
                          const char *env, flag_t flags)
{
    auto process = new_process();
    if (!process)
        return nullptr;

    process->parent_pid = current_process()->pid;

    auto mm_info = (mm_info_t *)process->mm_info;
    auto &vm_paging = mm_info->mmu_paging;
    // read executeable file header 128 bytes
    byte *header = (byte *)memory::KernelCommonAllocatorV->allocate(128, 8);
    file->move(0);
    file->read(header, 128);
    bin_handle::execute_info exec_info;
    if (flags & create_process_flags::binary_file)
    {
        bin_handle::load_bin(header, file, mm_info, &exec_info);
    }
    else if (!bin_handle::load(header, file, mm_info, &exec_info))
    {
        trace::info("Can't load execute file.");
        delete_process(process);
        memory::KernelCommonAllocatorV->deallocate(header);
        return nullptr;
    }
    memory::KernelCommonAllocatorV->deallocate(header);

    /// create thread
    thread_t *thd = new_thread(process);
    thd->state = thread_state::ready;
    void *stack = new_kernel_stack();
    void *stack_top = (char *)stack + memory::kernel_stack_size;
    thd->kernel_stack_top = stack_top;
    arch::task::create_thread(thd, (void *)start_func, arg0, (u64)args, (u64)env, (u64)exec_info.entry_start_address);

    thd->user_stack_top = exec_info.stack_top;
    thd->user_stack_bottom = exec_info.stack_bottom;

    vm_paging.sync_kernel();

    scheduler::add(thd);

    return process;
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

    create_thread(current_process(), builtin::softirq::main, 0, 0, 0, 0);
    task::builtin::idle::main(args);
}

process_t *find_pid(process_id pid)
{
    process_t *process = nullptr;
    global_process_map->get(pid, &process);
    return process;
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

void schedule()
{
    current()->attributes |= thread_attributes::need_schedule;
    scheduler::schedule();
}
} // namespace task
