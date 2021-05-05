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
#include "kernel/util/str.hpp"

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

#include "kernel/dev/tty/tty.hpp"

using mm_info_t = memory::vm::info_t;
namespace task
{
const thread_id max_thread_id = 0x10000;

const process_id max_process_id = 0x100000;

const group_id max_group_id = 0x10000;

using thread_list_t = util::linked_list<thread_t *>;

using process_id_generator_t = util::seq_generator;
process_id_generator_t *process_id_generator;

using thread_id_generator_t = util::seq_generator;

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
    uctx::RawSpinLockUninterruptibleContext icu(process_list_lock);
    auto id = process_id_generator->next();
    if (id == util::null_id)
        return nullptr;
    process_t *process = memory::New<process_t>(process_t_allocator);
    process->pid = id;
    process->thread_list = memory::New<thread_list_t>(memory::KernelCommonAllocatorV, memory::KernelCommonAllocatorV);
    process->mm_info = memory::kernel_vm_info;
    process->thread_id_gen = memory::New<thread_id_generator_t>(memory::KernelCommonAllocatorV, 0, 1);
    global_process_map->insert(id, process);
    return process;
}

inline process_t *new_process()
{
    uctx::RawSpinLockUninterruptibleContext icu(process_list_lock);
    auto id = process_id_generator->next();
    if (id == util::null_id)
        return nullptr;
    process_t *process = memory::New<process_t>(process_t_allocator);
    process->attributes = 0;
    process->pid = id;
    process->thread_list = memory::New<thread_list_t>(memory::KernelCommonAllocatorV, memory::KernelCommonAllocatorV);
    process->mm_info = memory::New<mm_info_t>(mm_info_t_allocator);
    process->thread_id_gen = memory::New<thread_id_generator_t>(memory::KernelCommonAllocatorV, 0, 1);
    global_process_map->insert(id, process);

    return process;
}

inline void delete_process(process_t *p)
{
    uctx::RawSpinLockUninterruptibleContext icu(process_list_lock);
    if (p->mm_info != nullptr)
        memory::Delete(mm_info_t_allocator, (mm_info_t *)p->mm_info);

    memory::Delete<thread_list_t>(memory::KernelCommonAllocatorV, (thread_list_t *)p->thread_list);
    global_process_map->remove(p->pid);
    // process_id_generator->collect(p->pid);
    memory::Delete<>(process_t_allocator, p);
}

inline thread_t *new_thread(process_t *p)
{
    using arch::task::register_info_t;
    uctx::RawSpinLockUninterruptibleContext icu(p->thread_list_lock);

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
    kassert(thd->state == thread_state::destroy, "thread state check failed.");
    if (thd->do_wait_queue_now)
        thd->do_wait_queue_now->remove(thd);

    uctx::RawSpinLockUninterruptibleContext icu(thd->process->thread_list_lock);

    using arch::task::register_info_t;

    auto thd_list = ((thread_list_t *)thd->process->thread_list);
    thd_list->remove(thd_list->find(thd));

    if (likely((u64)thd->kernel_stack_top != 0))
        delete_kernel_stack((void *)((u64)thd->kernel_stack_top - memory::kernel_stack_size));

    // ((thread_id_generator_t *)thd->process->thread_id_gen)->collect(thd->tid);
    memory::Delete<>(register_info_t_allocator, thd->register_info);
    memory::Delete<>(thread_t_allocator, thd);
}

process_t::process_t()
    : wait_counter(0)
{
}

thread_t::thread_t()
    : wait_counter(0)
    , do_wait_queue_now(nullptr)
{
}

void create_devs()
{
    fs::vfs::create("/dev", fs::vfs::global_root, fs::vfs::global_root, fs::create_flags::directory);

    fs::vfs::create("/dev/tty", fs::vfs::global_root, fs::vfs::global_root, fs::create_flags::directory);

    char fname[] = "/dev/tty/x";
    for (int i = 1; i < 8; i++)
    {
        fname[sizeof(fname) / sizeof(fname[0]) - 2] = '0' + i;
        fs::vfs::create(fname, fs::vfs::global_root, fs::vfs::global_root, fs::create_flags::chr);
        auto *f = fs::vfs::open(fname, fs::vfs::global_root, fs::vfs::global_root, fs::mode::read, 0);
        auto ps = memory::New<dev::tty::tty_pseudo_t>(memory::KernelCommonAllocatorV, memory::page_size);
        fs::vfs::fcntl(f, fs::fcntl_type::set, 0, fs::fcntl_attr::pseudo_func, (u64 *)&ps, 8);
        f->close();
    }

    fs::vfs::link("/dev/tty/0", "/dev/tty/1", fs::vfs::global_root, fs::vfs::global_root);
    fs::vfs::link("/dev/console", "/dev/tty/1", fs::vfs::global_root, fs::vfs::global_root);

    auto *f = fs::vfs::open("/dev/console", fs::vfs::global_root, fs::vfs::global_root, fs::mode::read, 0);
    auto ps = memory::New<dev::tty::tty_pseudo_t>(memory::KernelCommonAllocatorV, memory::page_size);
    fs::vfs::fcntl(f, fs::fcntl_type::set, 0, fs::fcntl_attr::pseudo_func, (u64 *)&ps, 8);

    f->close();
}

std::atomic_bool is_init = false, init_ok = false;
void init()
{
    process_t *process;
    if (cpu::current().is_bsp())
    {
        uctx::UninterruptibleContext icu;
        global_process_map = memory::New<process_map_t>(memory::KernelCommonAllocatorV, memory::KernelCommonAllocatorV);

        thread_t_allocator = memory::New<memory::SlabObjectAllocator>(
            memory::KernelCommonAllocatorV, NewSlabGroup(memory::global_object_slab_domain, thread_t, 8, 0));

        process_t_allocator = memory::New<memory::SlabObjectAllocator>(
            memory::KernelCommonAllocatorV, NewSlabGroup(memory::global_object_slab_domain, process_t, 8, 0));

        mm_info_t_allocator = memory::New<memory::SlabObjectAllocator>(
            memory::KernelCommonAllocatorV, NewSlabGroup(memory::global_object_slab_domain, mm_info_t, 8, 0));

        using arch::task::register_info_t;
        register_info_t_allocator = memory::New<memory::SlabObjectAllocator>(
            memory::KernelCommonAllocatorV, NewSlabGroup(memory::global_object_slab_domain, register_info_t, 8, 0));

        process_id_generator = memory::New<process_id_generator_t>(memory::KernelCommonAllocatorV, 0, 1);
        // init for kernel process
        process = new_kernel_process();
        process->parent_pid = 0;
        process->res_table.get_file_table()->root = fs::vfs::global_root;
        process->res_table.get_file_table()->current = fs::vfs::global_root;
    }
    else
    {
        while (!is_init)
        {
            cpu_pause();
        }
        process = find_pid(0);
    }

    thread_t *thd = new_thread(process);
    thd->state = thread_state::running;
    thd->static_priority = 125;
    thd->dynamic_priority = 0;
    thd->cpumask = current_cpu_mask();
    thd->cpuid = cpu::current().id();
    process->main_thread = thd;
    thd->attributes |= thread_attributes::main;

    arch::task::init(thd, thd->register_info);
    cpu::current().set_task(thd);
    cpu::current().set_idle_task(thd);
    trace::debug("Idle process (pid=", process->pid, ") thread (tid=", thd->tid, ") init");

    if (cpu::current().is_bsp())
    {
        auto ft = current_process()->res_table.get_file_table();
        create_devs();
        ft->id_gen.tag(0);
        ft->id_gen.tag(1);
        ft->id_gen.tag(2);
        auto tty0 = fs::vfs::open("/dev/tty/0", fs::vfs::global_root, fs::vfs::global_root, fs::mode::read, 0);
        ft->file_map.insert(0, tty0->clone());
        ft->file_map.insert(1, tty0->clone());
        ft->file_map.insert(2, tty0->clone());
        tty0->close();
        is_init = true;

        bin_handle::init();
        init_ok = true;
    }
    while (!init_ok)
        cpu_pause();
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

    thd->cpumask.mask = cpumask_none;

    arch::task::create_thread(thd, (void *)start_func, arg0, arg1, arg2, 0);

    if (flags & create_thread_flags::real_time_rr)
        scheduler::add(thd, scheduler::scheduler_class::round_robin);
    else
        scheduler::add(thd, scheduler::scheduler_class::cfs);

    return thd;
}

void copy_args_in_process(int argc, char *argv, char **&userland_argv)
{
    if (argc == 0)
    {
        if (argv != nullptr)
            memory::KernelCommonAllocatorV->deallocate(argv);
        return;
    }
    if (argv == nullptr)
        return;

    auto mm_info = (memory::vm::info_t *)current_process()->mm_info;
    auto ptr = mm_info->get_brk();
    mm_info->set_brk_now(ptr + sizeof(char *) * argc + *(u16 *)argv);
    argv += sizeof(u16) / sizeof(char);
    userland_argv = (char **)ptr;
    ptr += sizeof(char **) * argc;
    char *dst = (char *)ptr;

    for (int i = 0; i < argc; i++)
    {
        int n = util::strcopy(dst, argv) + 1;
        userland_argv[i] = dst;

        argv += n;
        dst += n;
    }
    memory::KernelCommonAllocatorV->deallocate(argv);
}

void befor_run_process(thread_start_func start_func, int argc, char *argv, void *entry)
{
    char **userland_argv;
    copy_args_in_process(argc, argv, userland_argv);
    start_func(0, argc, (u64)userland_argv, (u64)entry);
}

void cast_args(process_t *process, const char *args[], int *argc, char **argv)
{
    /// here: maximum args byte size is 4096bytes (a page) - sizeof(u16)
    constexpr int max_args_size = memory::page_size - 2;
    int arg_size = 0;
    int arg_count = 0;
    if (args == nullptr)
    {
        *argc = 0;
        *argv = nullptr;
        return;
    }
    for (int i = 0; i < 255; i++)
    {
        if (args[i] == nullptr)
        {
            break;
        }
        int len = util::strlen(args[i]);
        if (arg_size + len + 1 > max_args_size)
            break;
        arg_size += len + 1;
        arg_count++;
    }
    *argc = arg_count;

    char *ptr = (char *)memory::KernelCommonAllocatorV->allocate(arg_size + sizeof(u16), sizeof(u16));

    char *cur = ptr;
    *(u16 *)cur = arg_size;
    cur += sizeof(u16) / sizeof(char);

    for (int i = 0; i < arg_count; i++)
    {
        int len = util::strcopy(cur, args[i]);
        cur += len + 1;
    }

    *argv = ptr;
}

void copy_fd(fs::vfs::file *file, process_t *new_proc, process_t *old_proc, flag_t flags)
{
    kassert(new_proc != old_proc, "2 parameter processes assert failed");

    auto old_ft = old_proc->res_table.get_file_table();
    auto new_ft = new_proc->res_table.get_file_table();

    new_ft->id_gen.tag(0);
    new_ft->id_gen.tag(1);
    new_ft->id_gen.tag(2);

    if (unlikely(flags & create_process_flags::no_shared_root))
        new_ft->root = fs::vfs::global_root;
    else
        new_ft->root = old_ft->root;

    if (unlikely(flags & create_process_flags::shared_work_dir))
        new_ft->current = old_ft->current;
    else // set to parent dir (file directory)
        new_ft->current = file ? file->get_entry()->get_parent() : fs::vfs::global_root;

    if (flags & create_process_flags::shared_file_table)
    {
        new_ft->file_map = old_ft->file_map;
    }

    if (!(flags & create_process_flags::no_shared_stdin) && old_ft->file_map.get(0, &file))
        new_ft->file_map.insert(0, file->clone());
    else
        new_ft->file_map.insert(0, nullptr);

    if (!(flags & create_process_flags::no_shared_stdout) && old_ft->file_map.get(1, &file))
        new_ft->file_map.insert(1, file->clone());
    else
        new_ft->file_map.insert(1, nullptr);

    if (!(flags & create_process_flags::no_shared_stderror) && old_ft->file_map.get(2, &file))
        new_ft->file_map.insert(2, file->clone());
    else
        new_ft->file_map.insert(2, nullptr);
}

process_t *create_process(fs::vfs::file *file, thread_start_func start_func, const char *args[], flag_t flags)
{
    auto process = new_process();
    if (!process)
        return nullptr;

    process->parent_pid = current_process()->pid;

    copy_fd(file, process, current_process(), flags);

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
    thd->cpumask.mask = cpumask_none;
    void *stack = new_kernel_stack();
    void *stack_top = (char *)stack + memory::kernel_stack_size;
    thd->kernel_stack_top = stack_top;

    int argc;
    char *argv;
    cast_args(process, args, &argc, &argv);

    arch::task::create_thread(thd, (void *)befor_run_process, (u64)start_func, (u64)argc, (u64)argv,
                              (u64)exec_info.entry_start_address);

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
    copy_fd(nullptr, process, current_process(), flags);

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
        scheduler::update_state(thd, thread_state::ready);
        return;
    }
    return;
}

void do_sleep(u64 milliseconds)
{
    uctx::UninterruptibleContext icu;

    if (milliseconds != 0)
    {
        timer::add_watcher(milliseconds * 1000, sleep_callback_func, (u64)current());
        scheduler::update_state(current(), thread_state::stop);
    }
    else
    {
        current()->attributes |= task::thread_attributes::need_schedule;
    }
}

struct process_data_t
{
    thread_t *thd;
    i64 ret;
};

void exit_process_inner(thread_t *thd, i64 ret)
{
    if (thd->state != thread_state::destroy)
    {
        process_data_t *data = memory::New<process_data_t>(memory::KernelCommonAllocatorV);
        data->thd = thd;
        data->ret = ret;

        scheduler::remove(
            thd,
            [](u64 data) {
                auto *dt = reinterpret_cast<process_data_t *>(data);
                auto ret = dt->ret;
                auto process = dt->thd->process;
                auto list = (thread_list_t *)process->thread_list;

                dt->thd->user_stack_top = (void *)dt->ret;
                dt->thd->state = thread_state::destroy;
                dt->thd->wait_queue.do_wake_up();
                delete_thread(dt->thd);

                memory::Delete<>(memory::KernelCommonAllocatorV, dt);
                uctx::RawSpinLockUninterruptibleController icu(process->thread_list_lock);
                icu.begin();
                if (!list->empty())
                {
                    auto thd = list->front();
                    icu.end();
                    exit_process_inner(thd, ret);
                }
                else
                {
                    icu.end();
                    process->res_table.clear();
                    process->attributes |= process_attributes::no_thread;
                    process->wait_queue.do_wake_up();
                }
            },
            (u64)data);
    }
}

void exit_process(process_t *process, i64 ret, flag_t flags)
{
    // TODO: core_dump from flags
    trace::debug("process ", process->pid, " exit with code ", ret);
    uctx::RawSpinLockUninterruptibleController icu(process->thread_list_lock);
    auto &list = *(thread_list_t *)process->thread_list;

    process->ret_val = ret;
    icu.begin();
    if (!list.empty())
    {
        auto thd = list.front();
        icu.end();
        exit_process_inner(thd, ret);
    }
    else
    {
        icu.end();
        process->res_table.clear();
        process->attributes |= process_attributes::no_thread;
        process->wait_queue.do_wake_up();
    }
}

void do_exit(i64 ret)
{
    process_t *process = current_process();
    exit_process(process, ret, 0);
    thread_yield();
    trace::panic("Unreachable control flow.");
}

process_t *init_process = nullptr;
process_t *get_init_process() { return init_process; }

void set_init_process(process_t *proc) { init_process = proc; }

void start_task_idle()
{
    disable_preempt();
    {
        uctx::UninterruptibleContext icu;
        scheduler::init();
        scheduler::init_cpu();
    }
    enable_preempt();
    task::builtin::idle::main();
}

bool wait_process_exit(u64 user_data)
{
    auto proc = (process_t *)user_data;
    return proc->attributes & process_attributes::no_thread;
}

u64 wait_process(process_t *process, i64 &ret)
{
    if (process == nullptr)
        return 1;
    uctx::UninterruptibleContext icu;

    process->wait_counter++;
    process->wait_queue.do_wait(wait_process_exit, (u64)process);
    ret = (i64)process->ret_val;
    process->wait_counter--;
    if (process->wait_counter == 0)
    {
        process->attributes |= process_attributes::destroy;
        delete_process(process);
    }
    return 0;
}

bool wait_exit(u64 user_data) { return ((thread_t *)user_data)->state == thread_state::destroy; }

void exit_thread(thread_t *thd, i64 ret)
{
    trace::debug("exit thread ", thd->tid, " pid ", thd->process->pid, " code ", ret);
    struct data_t
    {
        thread_t *thd;
        i64 ret;
    };
    data_t *data = memory::New<data_t>(memory::KernelCommonAllocatorV);
    data->thd = thd;
    data->ret = ret;
    scheduler::remove(
        thd,
        [](u64 data) {
            auto *dt = reinterpret_cast<data_t *>(data);
            dt->thd->user_stack_top = (void *)dt->ret;
            dt->thd->state = thread_state::destroy;
            if (dt->thd->attributes & thread_attributes::detached)
            {
                delete_thread(dt->thd);
            }
            else
            {
                dt->thd->wait_queue.do_wake_up();
            }
        },
        reinterpret_cast<u64>(data));
}

void do_exit_thread(i64 ret)
{
    auto thd = current();
    exit_thread(thd, ret);
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

u64 join_thread(thread_t *thd, i64 &ret)
{
    if (thd == nullptr)
        return 1;
    if (thd == current())
        return 3;
    if (thd->attributes & thread_attributes::detached)
        return 2;
    if (thd->attributes & thread_attributes::main)
        return 4;
    uctx::UninterruptibleContext icu;
    thd->wait_counter++;
    thd->wait_queue.do_wait(wait_exit, (u64)thd);

    ret = (i64)thd->user_stack_top;
    thd->wait_counter--;
    if (thd->wait_counter == 0)
    {
        delete_thread(thd);
    }
    return 0;
}

void stop_thread(thread_t *thread, flag_t flags) { scheduler::update_state(thread, thread_state::stop); }

void continue_thread(thread_t *thread, flag_t flags) { scheduler::update_state(thread, thread_state::ready); }

process_t *find_pid(process_id pid)
{
    uctx::RawSpinLockUninterruptibleContext icu(process_list_lock);
    process_t *process = nullptr;
    global_process_map->get(pid, &process);
    return process;
}

thread_t *find_tid(process_t *process, thread_id tid)
{
    uctx::RawSpinLockUninterruptibleContext icu(process->thread_list_lock);
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

void set_cpu_mask(thread_t *thd, cpu_mask_t mask)
{
    thd->cpumask = mask;
    thd->attributes |= thread_attributes::need_schedule;
}

void thread_yield()
{
    current()->attributes |= thread_attributes::need_schedule;
    yield_preempt();
}

ExportC void kernel_return() { yield_preempt(); }

ExportC void userland_return() { scheduler::schedule(); }

} // namespace task
