#include "kernel/task.hpp"
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

#include "kernel/fs/vfs/dentry.hpp"
#include "kernel/fs/vfs/file.hpp"
#include "kernel/fs/vfs/pseudo.hpp"
#include "kernel/fs/vfs/vfs.hpp"

#include "kernel/scheduler.hpp"

#include "kernel/timer.hpp"
#include "kernel/ucontext.hpp"

#include "kernel/cpu.hpp"
#include "kernel/smp.hpp"
#include "kernel/task/binary_handle/bin_handle.hpp"
#include "kernel/task/binary_handle/elf.hpp"
#include "kernel/task/builtin/idle_task.hpp"
#include "kernel/task/builtin/soft_irq_task.hpp"
#include "kernel/wait.hpp"

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
    global_process_map->insert(id, process);
    process->signal_actions = nullptr;
    return process;
}

inline process_t *new_process()
{
    uctx::SpinLockUnInterruptableContext icu(process_list_lock);
    auto id = process_id_generator->next();
    if (id == util::null_id)
        return nullptr;
    process_t *process = memory::New<process_t>(process_t_allocator);
    process->attributes = 0;
    process->pid = id;
    process->thread_list = memory::New<thread_list_t>(memory::KernelCommonAllocatorV, thread_list_cache_allocator);
    process->mm_info = memory::New<mm_info_t>(mm_info_t_allocator);
    process->thread_id_gen = memory::New<thread_id_generator_t>(memory::KernelCommonAllocatorV, thread_id_param);
    global_process_map->insert(id, process);
    process->signal_actions = memory::New<signal_actions_t>(memory::KernelCommonAllocatorV);

    return process;
}

inline void delete_process(process_t *p)
{
    uctx::SpinLockUnInterruptableContext icu(process_list_lock);
    if (p->mm_info != nullptr)
        memory::Delete(mm_info_t_allocator, (mm_info_t *)p->mm_info);

    if (p->signal_actions != nullptr)
    {
        memory::Delete<>(memory::KernelCommonAllocatorV, p->signal_actions);
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
    thd->attributes = 0;
    return thd;
}

void delete_thread(thread_t *thd)
{
    uctx::SpinLockUnInterruptableContext icu(thd->process->thread_list_lock);

    using arch::task::register_info_t;

    auto thd_list = ((thread_list_t *)thd->process->thread_list);
    thd_list->remove(thd_list->find(thd));
    if (likely((u64)thd->kernel_stack_top > memory::kernel_stack_size))
        delete_kernel_stack((void *)((u64)thd->kernel_stack_top - memory::kernel_stack_size));

    ((thread_id_generator_t *)thd->process->thread_id_gen)->collect(thd->tid);
    memory::Delete<>(register_info_t_allocator, thd->register_info);
    memory::Delete<>(thread_t_allocator, thd);
}

process_t::process_t()
    : wait_que(memory::KernelCommonAllocatorV)
    , wait_counter(0)
{
}

thread_t::thread_t()
    : wait_que(memory::KernelCommonAllocatorV)
    , wait_counter(0)
{
}

void create_devs()
{
    fs::vfs::create("/dev", fs::vfs::global_root, fs::vfs::global_root, fs::create_flags::directory);

    fs::vfs::create("/dev/input", fs::vfs::global_root, fs::vfs::global_root, fs::create_flags::chr);
    auto *f = fs::vfs::open("/dev/input", fs::vfs::global_root, fs::vfs::global_root, fs::mode::read, 0);
    auto ps = memory::New<fs::vfs::pseudo_pipe_t>(memory::KernelCommonAllocatorV, memory::page_size);
    fs::vfs::fctl(f, fs::fctl_type::set, 0, fs::fctl_attr::pseudo_func, (u64 *)&ps, 8);

    fs::vfs::close(f);

    fs::vfs::create("/dev/tty", fs::vfs::global_root, fs::vfs::global_root, fs::create_flags::directory);

    char fname[] = "/dev/tty/x";
    for (int i = 0; i < 8; i++)
    {
        fname[sizeof(fname) / sizeof(fname[0]) - 2] = '0' + i;
        fs::vfs::create(fname, fs::vfs::global_root, fs::vfs::global_root, fs::create_flags::chr);
        auto *f = fs::vfs::open(fname, fs::vfs::global_root, fs::vfs::global_root, fs::mode::read, 0);
        fs::vfs::fctl(f, fs::fctl_type::set, 0, fs::fctl_attr::pseudo_func, (u64 *)&ps, 8);
        fs::vfs::close(f);
    }
}

void init()
{
    process_t *process;
    if (cpu::current().is_bsp())
    {
        uctx::UnInterruptableContext icu;
        thread_list_cache_allocator = memory::New<thread_list_node_allocator_t>(memory::KernelCommonAllocatorV);
        global_process_map = memory::New<process_map_t>(memory::KernelCommonAllocatorV, memory::KernelMemoryAllocatorV);

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
        process = new_kernel_process();
        process->parent_pid = 0;
        process->res_table.get_file_table()->root = fs::vfs::global_root;
        process->res_table.get_file_table()->current = fs::vfs::global_root;
    }
    else
    {
        process = find_pid(0);
    }

    thread_t *thd = new_thread(process);
    thd->state = thread_state::running;
    thd->static_priority = 125;
    thd->dynamic_priority = 0;
    process->main_thread = thd;
    thd->attributes |= thread_attributes::main;

    arch::task::init(thd, thd->register_info);
    cpu::current().set_task(thd);
    cpu::current().set_idle_task(thd);
    trace::debug("idle process (pid=", process->pid, ") thread (tid=", thd->tid, ") init...");

    if (cpu::current().is_bsp())
    {

        auto ft = current_process()->res_table.get_file_table();
        create_devs();
        ft->file_map[0] = fs::vfs::open("/dev/input", fs::vfs::global_root, fs::vfs::global_root, fs::mode::read, 0);
        bin_handle::init();
    }
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

    if (flags & create_thread_flags::real_time_rr)
        scheduler::add(thd, scheduler::scheduler_class::round_robin);
    else
        scheduler::add(thd, scheduler::scheduler_class::cfs);

    return thd;
}

process_t *create_process(fs::vfs::file *file, thread_start_func start_func, u64 arg0, const char *args,
                          const char *env, flag_t flags)
{
    auto process = new_process();
    if (!process)
        return nullptr;

    process->parent_pid = current_process()->pid;

    auto old_ft = current_process()->res_table.get_file_table();
    auto new_ft = process->res_table.get_file_table();

    if (unlikely(flags & create_process_flags::no_shared_root))
        new_ft->root = fs::vfs::global_root;
    else
        new_ft->root = old_ft->root;

    if (unlikely(flags & create_process_flags::shared_work_dir))
        new_ft->current = old_ft->current;
    else
        new_ft->current = file->get_entry()->get_parent();

    if (!(flags & create_process_flags::no_shared_stdin))
        new_ft->file_map[0] = old_ft->file_map[0];

    if (!(flags & create_process_flags::no_shared_stdout))
        new_ft->file_map[1] = old_ft->file_map[1];

    if (!(flags & create_process_flags::no_shared_stderror))
        new_ft->file_map[2] = old_ft->file_map[2];

    auto mm_info = (mm_info_t *)process->mm_info;
    auto &vm_paging = mm_info->mmu_paging;
    // read executeable file header 128 bytes
    byte *header = (byte *)memory::KernelCommonAllocatorV->allocate(128, 8);
    file->move(0);
    file->read(header, 128, 0);
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
    if (!thd)
        return nullptr;
    process->main_thread = thd;
    thd->attributes |= thread_attributes::main;
    thd->state = thread_state::ready;
    void *stack = new_kernel_stack();
    void *stack_top = (char *)stack + memory::kernel_stack_size;
    thd->kernel_stack_top = stack_top;

    /// TODO: cast args, env

    arch::task::create_thread(thd, (void *)start_func, arg0, (u64)args, (u64)env, (u64)exec_info.entry_start_address);

    thd->user_stack_top = exec_info.stack_top;
    thd->user_stack_bottom = exec_info.stack_bottom;
    vm_paging.sync_kernel();

    if (flags & create_process_flags::real_time_rr)
        scheduler::add(thd, scheduler::scheduler_class::round_robin);
    else
        scheduler::add(thd, scheduler::scheduler_class::cfs);

    return process;
}

process_t *create_kernel_process(thread_start_func start_func, u64 arg0, flag_t flags)
{
    auto process = new_kernel_process();
    if (!process)
        return nullptr;

    process->parent_pid = current_process()->pid;

    auto old_ft = current_process()->res_table.get_file_table();
    auto new_ft = process->res_table.get_file_table();

    if (unlikely(flags & create_process_flags::no_shared_root))
        new_ft->root = fs::vfs::global_root;
    else
        new_ft->root = old_ft->root;

    new_ft->current = old_ft->current;

    if (!(flags & create_process_flags::no_shared_stdin))
        new_ft->file_map[0] = old_ft->file_map[0];

    if (!(flags & create_process_flags::no_shared_stdout))
        new_ft->file_map[1] = old_ft->file_map[1];

    if (!(flags & create_process_flags::no_shared_stderror))
        new_ft->file_map[2] = old_ft->file_map[2];

    auto mm_info = (mm_info_t *)process->mm_info;
    auto &vm_paging = mm_info->mmu_paging;
    /// create thread
    thread_t *thd = new_thread(process);
    if (!thd)
        return nullptr;
    process->main_thread = thd;
    thd->attributes |= thread_attributes::main;
    thd->state = thread_state::ready;
    void *stack = new_kernel_stack();
    void *stack_top = (char *)stack + memory::kernel_stack_size;
    thd->kernel_stack_top = stack_top;

    arch::task::create_thread(thd, (void *)start_func, arg0, 0, 0, 0);

    thd->user_stack_top = 0;
    thd->user_stack_bottom = 0;

    if (flags & create_process_flags::real_time_rr)
        scheduler::add(thd, scheduler::scheduler_class::round_robin);
    else
        scheduler::add(thd, scheduler::scheduler_class::cfs);

    return process;
}

void sleep_callback_func(u64 pass, u64 data)
{
    thread_t *thd = (thread_t *)data;
    if (thd != nullptr)
    {
        uctx::UnInterruptableContext icu;
        scheduler::update_state(thd, thread_state::ready);
        return;
    }
    return;
}

void do_sleep(u64 milliseconds)
{
    uctx::UnInterruptableContext icu;
    current()->attributes |= task::thread_attributes::need_schedule;

    if (milliseconds != 0)
    {
        timer::add_watcher(milliseconds * 1000, sleep_callback_func, (u64)current());
        scheduler::update_state(current(), thread_state::interruptable);
    }
}

void do_exit(u64 ret)
{
    process_t *process = current_process();
    {
        uctx::SpinLockUnInterruptableContext icu(process->thread_list_lock);
        auto &list = *(thread_list_t *)process->thread_list;
        for (auto thd : list)
        {
            // Just destroy all thread
            if (thd->state == thread_state::stop || thd->state == thread_state::destroy)
            {
                thd->state = thread_state::destroy;
            }
            else
            {
                scheduler::remove(thd);
            }

            // do_wake_up(&thd->wait_que); don't wake up
        }
        process->ret_val = ret;
        auto parent = find_pid(process->parent_pid);
        process->attributes |= process_attributes::no_thread;
        do_wake_up(&process->wait_que);

        if (!parent)
        {
        }
    }
    thread_yield();
    trace::panic("Unreachable control flow.");
}

void destroy_process(process_t *process)
{
    /// TODO: delete file desc
    /// process->res_table.get_file_table()->file_map;

    delete_process(process);
}

void start_task_idle()
{
    {
        task::current()->preempt_data.disable_preempt();
        uctx::UnInterruptableContext icu;
        scheduler::init();
        SMP::wait_sync();
        scheduler::init_cpu();
    }

    task::current()->preempt_data.enable_preempt();
    task::builtin::idle::main();
}

bool wait_process_exit(u64 user_data)
{
    auto proc = (process_t *)user_data;
    return proc->attributes & process_attributes::no_thread;
}

u64 wait_process(process_t *process, u64 &ret)
{
    if (process == nullptr)
        return 1;
    uctx::UnInterruptableContext icu;

    process->wait_counter++;
    do_wait(&process->wait_que, wait_process_exit, (u64)process, wait_context_type::interruptable);
    ret = (u64)process->ret_val;
    process->wait_counter--;
    if (process->wait_counter == 0)
    {
        process->attributes |= process_attributes::destroy;
        check_process(process);
    }
    return 0;
}

bool wait_exit(u64 user_data) { return ((thread_t *)user_data)->state == thread_state::stop; }

void check_process(process_t *process)
{
    kassert(process != current_process(), "Invalid param thd");

    if (process->attributes & process_attributes::destroy)
    {
        bool all_destroy = true;
        uctx::SpinLockUnInterruptableContext icu(process->thread_list_lock);
        auto list = ((thread_list_t *)process->thread_list);
        for (auto it = list->begin(); it != list->end();)
        {
            auto sub_thd = *it;
            if (sub_thd->state == thread_state::destroy)
            {
                memory::Delete<>(register_info_t_allocator, sub_thd->register_info);
                if (likely((u64)sub_thd->kernel_stack_top > memory::kernel_stack_size))
                    delete_kernel_stack((void *)((u64)sub_thd->kernel_stack_top - memory::kernel_stack_size));
                memory::Delete<>(thread_t_allocator, sub_thd);
                it = list->remove(it);
            }
            else
            {
                all_destroy = false;
                ++it;
            }
            // do_wake_up(&thd->wait_que); don't wake up
        }
        if (all_destroy)
        {
            destroy_process(process);
        }
    }
}
void check_thread(thread_t *thd)
{
    kassert(thd != current(), "Invalid param thd");
    if (thd->state == thread_state::destroy)
    {
        delete_thread(thd);
    }
}

void exit_thread(u64 ret)
{
    {
        uctx::UnInterruptableContext icu;
        auto thd = current();
        thd->user_stack_top = (void *)ret;
        thd->state = thread_state::stop;
        scheduler::remove(thd);
        if (thd->attributes & thread_attributes::detached)
        {
            thd->state = thread_state::destroy;
            check_thread(thd);
        }
        else
        {
            do_wake_up(&thd->wait_que);
        }
    }

    thread_yield();
    trace::panic("Unreachable control flow.");
}

u64 detach_thread(thread_t *thd)
{
    if (thd == nullptr)
        return 1;
    if (thd == current())
        return 3;
    if (thd->attributes & thread_attributes::detached)
        return 2;
    if (thd->attributes & thread_attributes::main)
        return 4;
    thd->attributes |= thread_attributes::detached;
    return 0;
}

u64 join_thread(thread_t *thd, u64 &ret)
{
    if (thd == nullptr)
        return 1;
    if (thd == current())
        return 3;
    if (thd->attributes & thread_attributes::detached)
        return 2;
    if (thd->attributes & thread_attributes::main)
        return 4;
    uctx::UnInterruptableContext icu;
    thd->wait_counter++;
    do_wait(&thd->wait_que, wait_exit, (u64)thd, wait_context_type::interruptable);
    ret = (u64)thd->user_stack_top;
    thd->wait_counter--;
    if (thd->wait_counter == 0)
    {
        thd->state = thread_state::destroy;
        check_thread(thd);
    }
    return 0;
}

void stop_thread(thread_t *thread, flag_t flags) { scheduler::update_state(thread, thread_state::uninterruptible); }

void continue_thread(thread_t *thread, flag_t flags) { scheduler::update_state(thread, thread_state::ready); }

void kill_thread(thread_t *thread, flag_t flags)
{
    {
        uctx::UnInterruptableContext icu;
        thread->user_stack_top = (void *)0;
        thread->state = thread_state::stop;
        scheduler::remove(thread);
        if (thread->attributes & thread_attributes::detached)
        {
            thread->state = thread_state::destroy;
            check_thread(thread);
        }
        else
        {
            do_wake_up(&thread->wait_que);
        }
    }
    thread_yield();

    trace::panic("Unreachable control flow.");
}

process_t *find_pid(process_id pid)
{
    uctx::SpinLockUnInterruptableContext icu(process_list_lock);
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

void switch_thread(thread_t *old, thread_t *new_task)
{
    kassert(!arch::idt::is_enable(), "expect failed");

    cpu::current().set_task(new_task);

    if (old->process != new_task->process && old->process->mm_info != new_task->process->mm_info)
        ((mm_info_t *)new_task->process->mm_info)->mmu_paging.load_paging();

    _switch_task(old->register_info, new_task->register_info);
}

void thread_yield()
{
    current()->attributes |= thread_attributes::need_schedule;
    scheduler::schedule();
}

ExportC void kernel_return() { yield_preempt(); }

ExportC void userland_return()
{
    scheduler::schedule();
    do_signal();
}

} // namespace task
