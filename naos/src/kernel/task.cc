#include "kernel/task.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/arch/mm.hpp"
#include "kernel/arch/paging.hpp"
#include "kernel/arch/task.hpp"

#include "kernel/fs/vfs/defines.hpp"
#include "kernel/handle.hpp"
#include "kernel/kobject.hpp"
#include "kernel/mm/list_node_cache.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/mm/slab.hpp"
#include "kernel/mm/vm.hpp"

#include "freelibcxx/hash_map.hpp"
#include "freelibcxx/string.hpp"
#include "freelibcxx/vector.hpp"
#include "kernel/terminal.hpp"
#include "kernel/time.hpp"
#include "kernel/trace.hpp"
#include "kernel/types.hpp"
#include "kernel/util/id_generator.hpp"

#include "kernel/fs/vfs/dentry.hpp"
#include "kernel/fs/vfs/file.hpp"
#include "kernel/fs/vfs/inode.hpp"
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

using thread_list_t = freelibcxx::linked_list<thread_t *>;

using process_id_generator_t = util::seq_generator;
process_id_generator_t *process_id_generator;

using thread_id_generator_t = util::seq_generator;

memory::SlabObjectAllocator *thread_t_allocator;
memory::SlabObjectAllocator *process_t_allocator;
memory::SlabObjectAllocator *mm_info_t_allocator;

struct process_hash
{
    u64 operator()(process_id pid) { return pid; }
};

struct thread_hash
{
    u64 operator()(thread_id tid) { return tid; }
};

using process_map_t = freelibcxx::hash_map<process_id, process_t *, process_hash>;
using thread_map_t = freelibcxx::hash_map<process_id, process_t *, process_hash>;

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
    process->attributes = process_attributes::userspace;
    process->pid = id;
    process->thread_list = memory::New<thread_list_t>(memory::KernelCommonAllocatorV, memory::KernelCommonAllocatorV);
    process->mm_info = memory::New<mm_info_t>(mm_info_t_allocator);
    process->thread_id_gen = memory::New<thread_id_generator_t>(memory::KernelCommonAllocatorV, 0, 1);
    global_process_map->insert(id, process);

    return process;
}

inline process_t *copy_process(process_t *p)
{
    uctx::RawSpinLockUninterruptibleContext icu(process_list_lock);
    auto id = process_id_generator->next();
    if (id == util::null_id)
        return nullptr;
    process_t *process = memory::New<process_t>(process_t_allocator);
    process->attributes.store(p->attributes.load());
    process->pid = id;
    process->file = p->file;
    process->parent_pid = p->pid;
    process->thread_list = memory::New<thread_list_t>(memory::KernelCommonAllocatorV, memory::KernelCommonAllocatorV);
    auto info = memory::New<mm_info_t>(mm_info_t_allocator);
    reinterpret_cast<mm_info_t *>(p->mm_info)->share_to(p->pid, id, info);
    process->mm_info = info;
    process->thread_id_gen = memory::New<thread_id_generator_t>(memory::KernelCommonAllocatorV, 0, 1);
    global_process_map->insert(id, process);

    return process;
}

inline void delete_process(process_t *p)
{
    uctx::RawSpinLockUninterruptibleContext icu(process_list_lock);
    if (p->mm_info != nullptr)
        memory::Delete(mm_info_t_allocator, (mm_info_t *)p->mm_info);

    memory::KernelCommonAllocatorV->Delete(reinterpret_cast<thread_id_generator_t *>(p->thread_id_gen));

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
    thd->register_info = arch::task::new_register(p->attributes & process_attributes::userspace);
    thd->tid = id;
    thd->attributes = 0;
    thd->cpumask.mask = cpumask_none;

    void *stack = new_kernel_stack();
    void *stack_top = (char *)stack + memory::kernel_stack_size;
    thd->kernel_stack_top = stack_top;

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
    arch::task::delete_register(thd->register_info);
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
    auto root = fs::vfs::global_root;
    fs::vfs::create("/dev", root, root, fs::create_flags::directory);

    fs::vfs::create("/dev/tty", root, root, fs::create_flags::directory);
    dev::tty::tty_pseudo_t *ps;

    char fname[] = "/dev/tty/x";
    int max = term::get_terms()->total();
    for (int i = 0; i < max; i++)
    {
        fname[sizeof(fname) / sizeof(fname[0]) - 2] = '0' + i;
        fs::vfs::create(fname, root, root, fs::create_flags::chr);
        auto f = fs::vfs::open(fname, root, root, fs::mode::read | fs::mode::write, 0);

        ps = memory::KernelCommonAllocatorV->New<dev::tty::tty_pseudo_t>(i, memory::page_size * 2);
        fs::vfs::fcntl(f, fs::fcntl_type::set, 0, fs::fcntl_attr::pseudo_func, reinterpret_cast<u64 *>(&ps), 8);
    }

    fs::vfs::link("/dev/console", "/dev/tty/0", root, root);
}

std::atomic_bool is_init = false, init_ok = false;
bool has_init() { return is_init; }

void init()
{
    process_t *process;
    auto root = fs::vfs::global_root;
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

        process_id_generator = memory::New<process_id_generator_t>(memory::KernelCommonAllocatorV, 0, 1);
        // init for kernel process
        process = new_kernel_process();
        process->parent_pid = 0;
        process->resource.set_root(root);
        process->resource.set_current(root);
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
        create_devs();
        auto tty1read = fs::vfs::open("/dev/tty/1", root, root, fs::mode::read, 0);
        auto tty1write = fs::vfs::open("/dev/tty/1", root, root, fs::mode::write, 0);
        auto tty1err = fs::vfs::open("/dev/tty/1", root, root, fs::mode::write, 0);
        auto &res = current_process()->resource;
        kassert(tty1read, "invalid tty");

        res.set_handle(console_in, tty1read);
        res.set_handle(console_out, tty1write);
        res.set_handle(console_err, tty1err);
        is_init = true;
        term::get_terms()->switch_term(1);

        bin_handle::init();
        init_ok = true;
    }
    while (!init_ok)
        cpu_pause();
}

thread_t *create_thread(process_t *process, thread_start_func start_func, void *entry, void *arg, flag_t flags)
{
    thread_t *thd = new_thread(process);
    thd->state = thread_state::ready;

    auto &vma = ((mm_info_t *)process->mm_info)->vma();

    if (process->mm_info != memory::kernel_vm_info)
    {
        auto stack_vm = vma.allocate_map(memory::user_stack_maximum_size,
                                         memory::vm::flags::readable | memory::vm::flags::writeable |
                                             memory::vm::flags::expand | memory::vm::flags::user_mode,
                                         memory::vm::page_fault_method::common, 0);

        thd->user_stack_top = (void *)stack_vm->end;
        thd->user_stack_bottom = (void *)stack_vm->start;
    }

    thd->cpumask.mask = cpumask_none;

    thread_start_info_t *info = memory::New<thread_start_info_t>(memory::KernelCommonAllocatorV);
    info->args = arg;
    info->userland_entry = entry;
    info->userland_stack_offset = 0;

    arch::task::create_thread(thd, (void *)start_func, reinterpret_cast<u64>(info), 0, 0, 0);

    if (flags & create_thread_flags::real_time_rr)
        scheduler::add(thd, scheduler::scheduler_class::round_robin);
    else
        scheduler::add(thd, scheduler::scheduler_class::cfs);

    return thd;
}

void befor_run_process(thread_start_func start_func, process_args_t *args, u64 none, void *entry)
{
    thread_t *thd = current();
    byte *base = reinterpret_cast<byte *>(thd->user_stack_top);
    memcpy(base - args->size, args->data_ptr, args->size);
    byte *base_array = base - args->size;

    // bytes
    // env[0], env[1], nullptr
    // argv[0], argv[1], nullptr
    // argv_pointer
    // argc
    u64 base_bytes = sizeof(void *) * (args->argv.size() + args->env.size() + 1 + 1 + 1);

    byte **tail = reinterpret_cast<byte **>(base_array - base_bytes);
    // argc
    *(reinterpret_cast<u64 *>(tail)) = args->argv.size();
    tail++;
    // argv_pointer
    for (auto item : args->argv)
    {
        char *ptr = reinterpret_cast<char *>(base_array + item.offset);
        *(reinterpret_cast<char **>(tail)) = ptr;
        tail++;
    }
    // nullptr
    *(reinterpret_cast<byte **>(tail)) = nullptr;
    tail++;
    // envp
    for (auto item : args->env)
    {
        char *ptr = reinterpret_cast<char *>(base_array + item.offset);
        *(reinterpret_cast<char **>(tail)) = ptr;
        tail++;
    }
    // nullptr
    *(reinterpret_cast<byte **>(tail)) = nullptr;
    tail++;

    u64 size = args->size;
    memory::DeleteArray(memory::KernelCommonAllocatorV, args->data_ptr, args->size);
    memory::Delete(memory::KernelCommonAllocatorV, args);

    thread_start_info_t *info = memory::New<thread_start_info_t>(memory::KernelCommonAllocatorV);
    info->userland_entry = entry;
    info->userland_stack_offset = size + base_bytes;
    info->args = nullptr;

    start_func(info);
}

struct str_len_t
{
    const char *ptr;
    int len;
};

freelibcxx::vector<str_len_t> do_count_string_array(const char *const arr[], int *cur_bytes, int max_bytes)
{
    const char *const *tmp_arr = arr;
    freelibcxx::vector<str_len_t> args(memory::KernelCommonAllocatorV);
    if (tmp_arr == nullptr)
    {
        *cur_bytes = (*cur_bytes + sizeof(void *) - 1) & ~(sizeof(void *) - 1);
        return args;
    }
    while (*tmp_arr != nullptr)
    {
        int len = strlen(*tmp_arr) + 1;
        *cur_bytes += len;
        args.push_back(str_len_t{*tmp_arr, len});
        if (*cur_bytes >= max_bytes)
        {
            return args;
        }
        tmp_arr++;
    }
    *cur_bytes = (*cur_bytes + sizeof(void *) - 1) & ~(sizeof(void *) - 1);
    return args;
}

process_args_t *copy_args(const char *path, const char *const argv[], const char *const env[])
{
    process_args_t *ret = memory::New<process_args_t>(memory::KernelCommonAllocatorV, memory::KernelCommonAllocatorV);
    constexpr int max_args_bytes = memory::page_size * 8 - 2;

    // argv[0]
    int path_bytes = strlen(path) + 1 + sizeof(u64);
    int count_bytes = path_bytes;

    freelibcxx::vector<str_len_t> argvs = do_count_string_array(argv, &count_bytes, max_args_bytes);
    // int argv_bytes = count_bytes - path_bytes;

    freelibcxx::vector<str_len_t> envs = do_count_string_array(env, &count_bytes, max_args_bytes);
    // int env_bytes = count_bytes - argv_bytes;

    // argv[0] argv[1]
    // env[0]

    byte *ptr = memory::NewArray<byte>(memory::KernelCommonAllocatorV, count_bytes);
    byte *cur = ptr;

    // argv
    memcpy(cur, path, path_bytes);
    ret->argv.push_back(args_array_item_t(path_bytes, cur - ptr));
    cur += path_bytes;

    for (auto item : argvs)
    {
        ret->argv.push_back(args_array_item_t(item.len, cur - ptr));
        memcpy(cur, item.ptr, item.len);
        cur += item.len;
    }

    // env
    for (auto item : envs)
    {
        ret->env.push_back(args_array_item_t(item.len, cur - ptr));
        memcpy(cur, item.ptr, item.len);
        cur += item.len;
    }

    ret->data_ptr = ptr;
    ret->size = count_bytes;

    return ret;
}

void copy_fd(handle_t<fs::vfs::file> file, process_t *new_proc, process_t *old_proc, flag_t flags)
{
    kassert(new_proc != old_proc, "2 parameter processes assert failed");

    auto &old_res = old_proc->resource;
    auto &new_res = new_proc->resource;

    struct shared_t
    {
        flag_t flags;
    };
    shared_t shared{flags};

    old_res.clone(
        &new_res,
        [](file_desc fd, khandle handle, u64 u) -> bool {
            shared_t *s = reinterpret_cast<shared_t *>(u);
            bool is_console = fd <= console_err;
            if (s->flags & create_process_flags::no_shared_files)
            {
                if (!is_console)
                {
                    return false;
                }
            }
            if (is_console)
            {
                if (fd == console_in)
                    return !(s->flags & create_process_flags::no_shared_stdin);
                if (fd == console_out)
                    return !(s->flags & create_process_flags::no_shared_stdout);
                if (fd == console_err)
                    return !(s->flags & create_process_flags::no_shared_stderror);
            }
            return true;
        },
        reinterpret_cast<u64>(&shared));

    auto root = fs::vfs::global_root;

    if (unlikely(flags & create_process_flags::no_shared_root))
        new_res.set_root(root);
    else
        new_res.set_root(old_res.root());

    if (unlikely(flags & create_process_flags::no_shared_work_dir))
        new_res.set_current(file ? file->get_entry()->get_parent() : root);
    else
        new_res.set_current(old_res.current());
}

process_t *create_process(handle_t<fs::vfs::file> file, const char *path, thread_start_func start_func,
                          const char *const args[], const char *const envp[], flag_t flags)
{
    auto process = new_process();
    if (!process)
        return nullptr;

    process->parent_pid = current_process()->pid;
    process->file = file;

    copy_fd(file, process, current_process(), flags);

    auto mm_info = (mm_info_t *)process->mm_info;
    auto &paging = mm_info->paging();
    // read file header 128 bytes
    byte *header = (byte *)memory::KernelCommonAllocatorV->allocate(128, 8);
    file->pread(0, header, 128, 0);
    bin_handle::execute_info exec_info;
    if (flags & create_process_flags::binary_file)
    {
        bin_handle::load_bin(header, &file, mm_info, &exec_info);
    }
    else if (!bin_handle::load(header, &file, mm_info, &exec_info))
    {
        memory::KernelCommonAllocatorV->deallocate(header);
        trace::info("Can't load execute file.");
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

    auto process_args = copy_args(path, args, envp);

    arch::task::create_thread(thd, (void *)befor_run_process, reinterpret_cast<u64>(start_func),
                              reinterpret_cast<u64>(process_args), 0,
                              reinterpret_cast<u64>(exec_info.entry_start_address));

    thd->user_stack_top = exec_info.stack_top;
    thd->user_stack_bottom = exec_info.stack_bottom;
    paging.map_kernel_space();

    if (flags & create_process_flags::real_time_rr)
    {
        thd->attributes |= thread_attributes::real_time;
        scheduler::add(thd, scheduler::scheduler_class::round_robin);
    }
    else
        scheduler::add(thd, scheduler::scheduler_class::cfs);

    return process;
}

process_t *create_kernel_process(thread_start_func start_func, void *arg, flag_t flags)
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

    arch::task::create_thread(thd, (void *)start_func, reinterpret_cast<u64>(arg), 0, 0, 0);

    thd->user_stack_top = 0;
    thd->user_stack_bottom = 0;

    if (flags & create_process_flags::real_time_rr)
    {
        thd->attributes |= thread_attributes::real_time;
        scheduler::add(thd, scheduler::scheduler_class::round_robin);
    }
    else
        scheduler::add(thd, scheduler::scheduler_class::cfs);

    return process;
}

void fork_start_func(regs_t *regs)
{
    regs_t r = *regs;
    memory::Delete(memory::KernelCommonAllocatorV, regs);
    arch::task::enter_userland(current(), r);
}

int fork()
{
    auto current_thread = current();
    auto process = copy_process(current_process());
    if (!process)
        return -1;

    copy_fd(current_process()->file, process, current_process(), 0);

    auto mm_info = (mm_info_t *)process->mm_info;
    auto &paging = mm_info->paging();

    /// create thread
    thread_t *thd = new_thread(process);
    if (!thd)
        return -1;
    process->main_thread = thd;
    thd->attributes |= thread_attributes::main;
    thd->state = thread_state::ready;
    thd->cpumask.mask = cpumask_none;

    thd->user_stack_top = current_thread->user_stack_top;
    thd->user_stack_bottom = current_thread->user_stack_bottom;
    thd->tcb = current_thread->tcb;

    regs_t *regs = memory::New<regs_t>(memory::KernelCommonAllocatorV);
    arch::task::get_syscall_regs(*regs);
    regs->rax = 0;

    arch::task::create_thread(thd, (void *)fork_start_func, reinterpret_cast<u64>(regs), 0, 0, 0);

    paging.map_kernel_space();
    paging.load();

    if (current()->attributes & thread_attributes::real_time)
    {
        thd->attributes |= thread_attributes::real_time;
        scheduler::add(thd, scheduler::scheduler_class::round_robin);
    }
    else
    {
        scheduler::add(thd, scheduler::scheduler_class::cfs);
    }

    return thd->process->pid;
}

int execve(handle_t<fs::vfs::file> file, const char *path, thread_start_func start_func, char *const argv[],
           char *const envp[])
{
    // trace::info("exec ", path);
    auto thd = current();
    auto process = thd->process;
    auto process_args = copy_args(path, argv, envp);
    // trace::info("process ", process->pid, " execve with ", path);

    auto mm_info = (mm_info_t *)process->mm_info;
    auto new_mm_info = memory::New<mm_info_t>(mm_info_t_allocator);
    {
        uctx::UninterruptibleContext ctx;
        process->mm_info = new_mm_info;
        new_mm_info->paging().map_kernel_space();
        new_mm_info->paging().load();
    }
    process->file = file;

    memory::Delete(mm_info_t_allocator, mm_info);
    mm_info = new_mm_info;

    // read file header 128 bytes
    byte *header = (byte *)memory::KernelCommonAllocatorV->allocate(128, 8);
    file->pread(0, header, 128, 0);
    bin_handle::execute_info exec_info;
    if (!bin_handle::load(header, &file, mm_info, &exec_info))
    {
        memory::KernelCommonAllocatorV->deallocate(header);
        trace::info("Can't load execute file.");
        memory::DeleteArray(memory::KernelCommonAllocatorV, process_args->data_ptr, process_args->size);
        memory::Delete(memory::KernelCommonAllocatorV, process_args);
        return ENOEXEC;
    }
    memory::KernelCommonAllocatorV->deallocate(header);
    thd->user_stack_top = exec_info.stack_top;
    thd->user_stack_bottom = exec_info.stack_bottom;

    befor_run_process(start_func, process_args, 0, exec_info.entry_start_address);
    return 0;
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

void do_sleep(const timeclock::time &time)
{
    timeclock::time t = time;
    auto us = t.tv_nsec / 1000 + t.tv_sec * 1000 * 1000;
    uctx::UninterruptibleContext icu;

    if (us != 0)
    {
        scheduler::update_state(current(), thread_state::stop);
        timer::add_watcher(us, sleep_callback_func, (u64)current());
    }
    else
    {
        current()->attributes |= task::thread_attributes::need_schedule;
    }
}

struct process_data_t
{
    thread_t *thd;
};

void exit_process_inner(thread_t *thd);
void exit_process_thread(process_t *process)
{
    uctx::RawSpinLockUninterruptibleController icu(process->thread_list_lock);
    auto &list = *(thread_list_t *)process->thread_list;

    icu.begin();
    if (!list.empty())
    {
        auto thd = list.front();
        icu.end();
        exit_process_inner(thd);
    }
    else
    {
        icu.end();
        process->resource.clear();
        process->attributes |= process_attributes::no_thread;
        if (process == get_init_process())
        {
            trace::panic("init process startup fail");
        }
        process->wait_queue.do_wake_up();
    }
}

void exit_process_inner(thread_t *thd)
{
    if (thd->state != thread_state::destroy)
    {
        process_data_t *data = memory::New<process_data_t>(memory::KernelCommonAllocatorV);
        data->thd = thd;

        scheduler::remove(
            thd,
            [](u64 data) {
                
                auto *dt = reinterpret_cast<process_data_t *>(data);
                auto process = dt->thd->process;

                dt->thd->state = thread_state::destroy;
                dt->thd->wait_queue.do_wake_up();
                delete_thread(dt->thd);

                memory::Delete<>(memory::KernelCommonAllocatorV, dt);
                exit_process_thread(process);
            },
            (u64)data);
    }
    else
    {
        trace::panic((int)thd->state);
    }
}

void exit_process(process_t *process, i64 ret, flag_t flags)
{
    // TODO: write core_dump from flags
    // trace::debug("process ", process->pid, " exit with code ", ret);
    process->ret_val = ret;
    exit_process_thread(process);
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
    task::builtin::idle::main(0);
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
    if (--process->wait_counter == 0)
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
            memory::Delete<>(memory::KernelCommonAllocatorV, dt);
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

thread_t *find_kernel_stack_thread(void *stack_ptr)
{
    u64 s = reinterpret_cast<u64>(stack_ptr);
    uctx::RawSpinLockUninterruptibleContext icu(process_list_lock);
    for (auto p : *global_process_map)
    {
        auto process = p.value;
        uctx::RawSpinLockUninterruptibleContext icu(process->thread_list_lock);
        auto &list = *(thread_list_t *)process->thread_list;
        for (auto thd : list)
        {
            if (reinterpret_cast<u64>(thd->kernel_stack_top) > s &&
                reinterpret_cast<u64>(thd->kernel_stack_top) - memory::kernel_stack_size < s)
            {
                return thd;
            }
        }
    }
    return nullptr;
}

void stop_thread(thread_t *thread, flag_t flags) { scheduler::update_state(thread, thread_state::stop); }

void continue_thread(thread_t *thread, flag_t flags) { scheduler::update_state(thread, thread_state::ready); }

process_t *find_pid(process_id pid)
{
    uctx::RawSpinLockUninterruptibleContext icu(process_list_lock);
    return global_process_map->get(pid).value_or(nullptr);
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
    {
        ((mm_info_t *)new_task->process->mm_info)->paging().load();
    }

    arch::task::update_fs(new_task);
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

void set_tcb(thread_t *t, void *p)
{
    t->tcb = p;
    // trace::info("process ", t->process->pid, " thread ", t->tid, " set tcb ", trace::hex(p));
    arch::task::update_fs(t);
}

void write_main_stack(thread_t *thread, main_stack_data_t stack)
{
    memcpy(thread->user_stack_top, &stack, sizeof(stack));
}

} // namespace task
