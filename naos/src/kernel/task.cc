#include "kernel/task.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/arch/mm.hpp"
#include "kernel/arch/paging.hpp"
#include "kernel/arch/task.hpp"

#include "kernel/fs/vfs/defines.hpp"
#include "kernel/handle.hpp"
#include "kernel/ipc/channel.hpp"
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
#include "naos/generated/system_uapi.h"

#include "kernel/fs/vfs/dentry.hpp"
#include "kernel/fs/vfs/file.hpp"
#include "kernel/fs/vfs/inode.hpp"
#include "kernel/fs/vfs/pseudo.hpp"
#include "kernel/fs/vfs/vfs.hpp"

#include "kernel/scheduler.hpp"

#include "kernel/timer.hpp"
#include "kernel/ucontext.hpp"
#include <limits>

#include "kernel/cpu.hpp"
#include "kernel/errno.hpp"
#include "kernel/smp.hpp"
#include "kernel/task/binary_handle/bin_handle.hpp"
#include "kernel/task/binary_handle/elf.hpp"
#include "kernel/task/builtin/idle_task.hpp"
#include "kernel/task/builtin/soft_irq_task.hpp"
#include "kernel/wait.hpp"
#include "naos/generated/system/Stream.hpp"

#include "kernel/dev/framebuffer.hpp"
#include "kernel/dev/tty/pty_manager.hpp"
#include "kernel/dev/tty/tty.hpp"

using mm_info_t = memory::vm::info_t;
namespace task
{
const thread_id max_thread_id = 0x10000;

const process_id max_process_id = 0x100000;

const group_id max_group_id = 0x10000;

const session_id max_session_id = 0x10000;

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

struct session_hash
{
    u64 operator()(::session_id id) { return id; }
};

struct process_group_hash
{
    u64 operator()(group_id id) { return id; }
};

/// A session owns its membership index and the controlling-terminal state.
/// Process pointers in this index are non-owning; global_process_map remains
/// the owner of process lifetime.
struct session_t
{
    ::session_id id;
    process_map_t members;
    dev::tty::tty_core *controlling_tty = nullptr;
    group_id foreground_process_group = 0;

    explicit session_t(::session_id id)
        : id(id)
        , members(memory::KernelCommonAllocatorV)
    {
    }
};

/// Process-group membership is indexed independently so group-directed signal
/// delivery does not need to scan every process in the system.
struct process_group_t
{
    ::session_id session;
    group_id id;
    process_map_t members;

    process_group_t(::session_id session, group_id id)
        : session(session)
        , id(id)
        , members(memory::KernelCommonAllocatorV)
    {
    }
};

using session_map_t = freelibcxx::hash_map<::session_id, session_t *, session_hash>;
using process_group_map_t = freelibcxx::hash_map<group_id, process_group_t *, process_group_hash>;

process_map_t *global_process_map;
session_map_t *global_session_map;
process_group_map_t *global_process_group_map;
// process_list_t *global_process_list;
lock::spinlock_t process_list_lock;

inline void *new_kernel_stack() { return memory::KernelBuddyAllocatorV->allocate(memory::kernel_stack_size, 0); }

inline void delete_kernel_stack(void *p) { memory::KernelBuddyAllocatorV->deallocate(p); }

namespace
{
capability::metadata stream_capability_metadata()
{
    capability::metadata metadata;
    metadata.binding = NA_BINDING_KERNEL_VIEW;
    metadata.protocol_uuid = naos::system::Stream::protocol_uuid;
    metadata.scope = NA_SCOPE_STREAM;
    metadata.revision = 1;
    metadata.meta_rights = NA_RIGHT_DUPLICATE | NA_RIGHT_TRANSFER | NA_RIGHT_WAIT | NA_RIGHT_INSPECT;
    metadata.protocol_rights = NA_PROTOCOL_RIGHT_INVOKE;
    return metadata;
}

constexpr u64 aux_at_null = 0;
constexpr u64 aux_at_phdr = 3;
constexpr u64 aux_at_phent = 4;
constexpr u64 aux_at_phnum = 5;
constexpr u64 aux_at_base = 7;
constexpr u64 aux_at_pagesz = 6;
constexpr u64 aux_at_entry = 9;
constexpr u64 aux_at_uid = 11;
constexpr u64 aux_at_euid = 12;
constexpr u64 aux_at_gid = 13;
constexpr u64 aux_at_egid = 14;
constexpr u64 aux_at_platform = 15;
constexpr u64 aux_at_hwcap = 16;
constexpr u64 aux_at_clktck = 17;
constexpr u64 aux_at_secure = 23;
constexpr u64 aux_at_random = 25;
constexpr u64 aux_at_execfn = 31;

constexpr char aux_platform[] = "x86_64";
constexpr u64 aux_random_size = 16;
constexpr u64 aux_random_offset = (sizeof(aux_platform) + sizeof(u64) - 1) & ~(sizeof(u64) - 1);
constexpr u64 aux_data_size = aux_random_offset + aux_random_size;
constexpr u64 aux_vector_entries = 17;

void fill_auxiliary_vector(byte **&tail, const process_args_t &args, void *entry, const char *platform,
                           const byte *random, const char *execfn)
{
    auto push = [&tail](u64 type, u64 value) {
        *(reinterpret_cast<u64 *>(tail)) = type;
        tail++;
        *(reinterpret_cast<u64 *>(tail)) = value;
        tail++;
    };

    push(aux_at_phdr, reinterpret_cast<u64>(args.program_header));
    push(aux_at_phent, args.program_header_entry_size);
    push(aux_at_phnum, args.program_header_count);
    push(aux_at_base, args.base_address);
    push(aux_at_pagesz, memory::page_size);
    push(aux_at_entry, reinterpret_cast<u64>(entry));
    push(aux_at_platform, reinterpret_cast<u64>(platform));
    push(aux_at_hwcap, args.hwcap);
    push(aux_at_random, reinterpret_cast<u64>(random));
    push(aux_at_execfn, reinterpret_cast<u64>(execfn));
    push(aux_at_uid, 0);
    push(aux_at_euid, 0);
    push(aux_at_gid, 0);
    push(aux_at_egid, 0);
    push(aux_at_clktck, 100);
    push(aux_at_secure, 0);
    push(aux_at_null, 0);
}
} // namespace

namespace
{
session_t *find_session_unlocked(::session_id id)
{
    if (global_session_map == nullptr)
        return nullptr;
    return global_session_map->get(id).value_or(nullptr);
}

process_group_t *find_process_group_unlocked(group_id id)
{
    if (global_process_group_map == nullptr)
        return nullptr;
    return global_process_group_map->get(id).value_or(nullptr);
}

session_t *get_or_create_session_unlocked(::session_id id)
{
    auto *session = find_session_unlocked(id);
    if (session != nullptr)
        return session;

    session = memory::New<session_t>(memory::KernelCommonAllocatorV, id);
    global_session_map->insert(id, session);
    return session;
}

process_group_t *get_or_create_process_group_unlocked(::session_id session_id, group_id id)
{
    auto *process_group = find_process_group_unlocked(id);
    if (process_group != nullptr)
    {
        kassert(process_group->session == session_id, "process group belongs to another session");
        return process_group;
    }

    process_group = memory::New<process_group_t>(memory::KernelCommonAllocatorV, session_id, id);
    global_process_group_map->insert(id, process_group);
    return process_group;
}

void remove_process_group_if_empty_unlocked(process_group_t *process_group)
{
    if (process_group == nullptr || process_group->members.size() != 0)
        return;

    global_process_group_map->remove(process_group->id);
    memory::Delete<>(memory::KernelCommonAllocatorV, process_group);
}

void remove_session_if_empty_unlocked(session_t *session)
{
    if (session == nullptr || session->members.size() != 0 || session->controlling_tty != nullptr)
        return;

    global_session_map->remove(session->id);
    memory::Delete<>(memory::KernelCommonAllocatorV, session);
}

void register_process_job_control_unlocked(process_t *process)
{
    auto *session = get_or_create_session_unlocked(process->session_id);
    auto *process_group = get_or_create_process_group_unlocked(process->session_id, process->process_group_id);
    session->members.insert(process->pid, process);
    process_group->members.insert(process->pid, process);
    process->session = session;
    process->process_group = process_group;
}

void unregister_process_job_control_unlocked(process_t *process)
{
    if (process == nullptr)
        return;

    auto *session = process->session;
    auto *process_group = process->process_group;
    if (process_group != nullptr)
    {
        process_group->members.remove(process->pid);
        remove_process_group_if_empty_unlocked(process_group);
    }
    if (session != nullptr)
    {
        session->members.remove(process->pid);
        remove_session_if_empty_unlocked(session);
    }
    process->session = nullptr;
    process->process_group = nullptr;
}

void move_process_session_unlocked(process_t *process, ::session_id session_id, group_id process_group_id)
{
    if (process->session_id == session_id && process->process_group_id == process_group_id &&
        process->session != nullptr && process->process_group != nullptr)
        return;

    unregister_process_job_control_unlocked(process);
    process->session_id = session_id;
    process->process_group_id = process_group_id;
    register_process_job_control_unlocked(process);
}

void move_process_group_unlocked(process_t *process, group_id process_group_id)
{
    if (process->process_group_id == process_group_id && process->process_group != nullptr)
        return;

    auto *session = process->session;
    auto *old_process_group = process->process_group;
    const auto old_process_group_id = process->process_group_id;
    bool old_process_group_empty = false;
    if (old_process_group != nullptr)
    {
        old_process_group->members.remove(process->pid);
        old_process_group_empty = old_process_group->members.size() == 0;
        remove_process_group_if_empty_unlocked(old_process_group);
    }

    process->process_group_id = process_group_id;
    auto *new_process_group = get_or_create_process_group_unlocked(process->session_id, process_group_id);
    new_process_group->members.insert(process->pid, process);
    process->process_group = new_process_group;

    if (session != nullptr && session->foreground_process_group == old_process_group_id && old_process_group_empty)
    {
        session->foreground_process_group = 0;
        if (session->controlling_tty != nullptr)
            session->controlling_tty->set_foreground_process_group(0);
    }
}
} // namespace

inline process_t *new_kernel_process()
{
    uctx::RawSpinLockUninterruptibleContext icu(process_list_lock);
    auto id = process_id_generator->next();
    if (id == util::null_id)
        return nullptr;
    process_t *process = memory::New<process_t>(process_t_allocator);
    process->attributes.store(0);
    process->pid = id;
    process->session_id = id;
    process->process_group_id = id;
    process->thread_list = memory::New<thread_list_t>(memory::KernelCommonAllocatorV, memory::KernelCommonAllocatorV);
    process->mm_info = memory::kernel_vm_info;
    process->thread_id_gen = memory::New<thread_id_generator_t>(memory::KernelCommonAllocatorV, 0, 1);
    global_process_map->insert(id, process);
    register_process_job_control_unlocked(process);
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
    process->session_id = id;
    process->process_group_id = id;
    process->thread_list = memory::New<thread_list_t>(memory::KernelCommonAllocatorV, memory::KernelCommonAllocatorV);
    process->mm_info = memory::New<mm_info_t>(mm_info_t_allocator);
    process->thread_id_gen = memory::New<thread_id_generator_t>(memory::KernelCommonAllocatorV, 0, 1);
    global_process_map->insert(id, process);
    register_process_job_control_unlocked(process);

    return process;
}

inline process_t *copy_process(process_t *p)
{
    uctx::RawSpinLockUninterruptibleContext icu(process_list_lock);
    auto id = process_id_generator->next();
    if (id == util::null_id)
        return nullptr;
    process_t *process = memory::New<process_t>(process_t_allocator);
    process->attributes.store(p->attributes.load() & ~process_attributes::job_control_cleanup_done);
    process->pid = id;
    process->file = p->file;
    process->parent_pid = p->pid;
    process->session_id = p->session_id;
    process->process_group_id = p->process_group_id;
    process->controlling_tty = p->controlling_tty;
    process->signal_pack.inherit_mask_from(p->signal_pack);
    process->foreground_process_group = 0;
    process->thread_list = memory::New<thread_list_t>(memory::KernelCommonAllocatorV, memory::KernelCommonAllocatorV);
    auto info = memory::New<mm_info_t>(mm_info_t_allocator);
    reinterpret_cast<mm_info_t *>(p->mm_info)->share_to(p->pid, id, info);
    process->mm_info = info;
    process->thread_id_gen = memory::New<thread_id_generator_t>(memory::KernelCommonAllocatorV, 0, 1);
    global_process_map->insert(id, process);
    register_process_job_control_unlocked(process);

    return process;
}

void finalize_process(process_t *p)
{
    if (p->mm_info != nullptr)
        memory::Delete(mm_info_t_allocator, (mm_info_t *)p->mm_info);

    memory::KernelCommonAllocatorV->Delete(reinterpret_cast<thread_id_generator_t *>(p->thread_id_gen));

    memory::Delete<thread_list_t>(memory::KernelCommonAllocatorV, (thread_list_t *)p->thread_list);
    memory::Delete<>(process_t_allocator, p);
}

void maybe_finalize_process(process_t *p)
{
    if (p == nullptr || !p->reap_pending.load() || p->capability_refs.load() != 0)
        return;

    bool expected = false;
    if (p->storage_released.compare_exchange_strong(expected, true))
        finalize_process(p);
}

inline void delete_process(process_t *p)
{
    if (p == nullptr)
        return;

    {
        uctx::RawSpinLockUninterruptibleContext icu(process_list_lock);
        if (p->reap_pending.exchange(true))
            return;
        p->attributes |= process_attributes::destroy;
        unregister_process_job_control_unlocked(p);
        global_process_map->remove(p->pid);
        // process_id_generator->collect(p->pid);
    }
    maybe_finalize_process(p);
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
    , wait_claimed(false)
    , child_wait_generation(0)
    , capability_refs(0)
    , reap_pending(false)
    , storage_released(false)
    , ret_val(0)
{
}

process_object::process_object(process_t *process)
    : kobject(type_e::process)
    , process_(process)
{
    if (process_ != nullptr)
        process_->capability_refs.fetch_add(1);
}

process_object::~process_object()
{
    if (process_ != nullptr && process_->capability_refs.fetch_sub(1) == 1)
        maybe_finalize_process(process_);
}

na_signal_t process_object::capability_signals() const
{
    if (process_ == nullptr)
        return NA_SIGNAL_OBJECT_REVOKED;
    return (process_->attributes.load() & process_attributes::no_thread) != 0 ? NA_SIGNAL_COMPLETED : 0;
}

u64 process_object::capability_state() const { return process_ == nullptr ? 0 : static_cast<u64>(process_->ret_val); }

thread_t::thread_t()
    : wait_counter(0)
    , do_wait_queue_now(nullptr)
{
}

void create_devs()
{
    auto root = fs::vfs::global_root;
    fs::vfs::create("/dev", root, root, fs::create_flags::directory);
    fs::vfs::mkdir("/dev/pts", root, root, fs::create_flags::directory);
    fs::vfs::create("/dev/ptmx", root, root, fs::create_flags::chr);
    dev::pty::init();

    auto create_tty = [&](const char *name, int terminal_index) {
        fs::vfs::create(name, root, root, fs::create_flags::chr);
        auto f = fs::vfs::open(name, root, root, fs::mode::read | fs::mode::write, 0);
        auto *ps = memory::KernelCommonAllocatorV->New<dev::tty::tty_pseudo_t>(terminal_index, memory::page_size * 2);
        fs::vfs::fcntl(f, fs::fcntl_type::set, 0, fs::fcntl_attr::pseudo_func, reinterpret_cast<u64 *>(&ps), 8);
    };

    create_tty("/dev/console", term::terminal_manager::kernel_console_index);
    create_tty("/dev/tty0", term::terminal_manager::user_terminal_index);
    fs::vfs::symbolink("/dev/tty", "/dev/tty0", root, root, 0);

    {
        constexpr const char *fb_name = "/dev/fb0";
        fs::vfs::create(fb_name, root, root, fs::create_flags::chr);
        auto f = fs::vfs::open(fb_name, root, root, fs::mode::read | fs::mode::write, 0);
        auto *fb = memory::KernelCommonAllocatorV->New<dev::framebuffer::framebuffer_pseudo_t>(term::get_terms());
        fs::vfs::fcntl(f, fs::fcntl_type::set, 0, fs::fcntl_attr::pseudo_func, reinterpret_cast<u64 *>(&fb), 8);
    }
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
        global_session_map = memory::New<session_map_t>(memory::KernelCommonAllocatorV, memory::KernelCommonAllocatorV);
        global_process_group_map =
            memory::New<process_group_map_t>(memory::KernelCommonAllocatorV, memory::KernelCommonAllocatorV);

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
        process->bootstrap_root_directory = handle_t<fs::vfs::native_directory>::make(root, root);
        process->bootstrap_current_directory = handle_t<fs::vfs::native_directory>::make(root, root);
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
        auto tty0read = fs::vfs::open("/dev/tty0", root, root, fs::mode::read, 0);
        auto tty0write = fs::vfs::open("/dev/tty0", root, root, fs::mode::write, 0);
        auto tty0err = fs::vfs::open("/dev/tty0", root, root, fs::mode::write, 0);
        kassert(tty0read, "invalid tty");
        auto *init_process = current_process();
        const auto metadata = stream_capability_metadata();
        init_process->console_in_handle = init_process->resource.install_native(tty0read, metadata);
        init_process->console_out_handle = init_process->resource.install_native(tty0write, metadata);
        init_process->console_err_handle = init_process->resource.install_native(tty0err, metadata);
        kassert(init_process->console_in_handle != NA_HANDLE_INVALID &&
                    init_process->console_out_handle != NA_HANDLE_INVALID &&
                    init_process->console_err_handle != NA_HANDLE_INVALID,
                "unable to install bootstrap console capabilities");
        is_init = true;
        term::get_terms()->switch_term(term::terminal_manager::user_terminal_index);

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
    u64 argument_size = args->size;
    u64 stack_data_size = argument_size + aux_data_size;
    byte *base_array = base - stack_data_size;
    memcpy(base_array, args->data_ptr, argument_size);

    byte *aux_data = base_array + argument_size;
    memcpy(aux_data, aux_platform, sizeof(aux_platform));
    byte *random = aux_data + aux_random_offset;
    u64 random_value0 = _rdtsc() ^ reinterpret_cast<u64>(base) ^ current()->process->pid;
    u64 random_value1 = _rdtsc() ^ random_value0 ^ (random_value0 << 17);
    memcpy(random, &random_value0, sizeof(random_value0));
    memcpy(random + sizeof(random_value0), &random_value1, sizeof(random_value1));
    const char *execfn = reinterpret_cast<const char *>(base_array + args->execfn_offset);

    // bytes
    // env[0], env[1], nullptr
    // argv[0], argv[1], nullptr
    // AT_NULL, 0
    // argv_pointer
    // argc
    // Keep the initial stack in the usual ELF form. mlibc parses the
    // auxiliary vector after envp, even for statically linked binaries.
    u64 base_bytes = sizeof(void *) * (args->argv.size() + args->env.size() + 1 + 1 + 1 + aux_vector_entries * 2);
    u64 size = stack_data_size;
    // crt1 calls into mlibc immediately, so the stack pointer at the ELF
    // entry point must be 16-byte aligned to satisfy the x86-64 call ABI.
    base_bytes += (-((size + base_bytes) & 0xF)) & 0xF;

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

    fill_auxiliary_vector(tail, *args, entry, reinterpret_cast<const char *>(aux_data), random, execfn);

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
    int path_bytes = strlen(path) + 1;

    int count_bytes = 0;

    freelibcxx::vector<str_len_t> argvs = do_count_string_array(argv, &count_bytes, max_args_bytes);
    // execve() supplies argv[0] itself. Keep a useful fallback for callers
    // that pass an empty argument vector, but do not prepend path in the
    // normal case: execl(path, arg0, ...) must not gain an extra argument.
    if (argvs.empty())
    {
        count_bytes += path_bytes;
    }

    freelibcxx::vector<str_len_t> envs = do_count_string_array(env, &count_bytes, max_args_bytes);
    count_bytes += path_bytes;

    byte *ptr = memory::NewArray<byte>(memory::KernelCommonAllocatorV, count_bytes);
    byte *cur = ptr;

    // argv
    if (argvs.empty())
    {
        memcpy(cur, path, path_bytes);
        ret->argv.push_back(args_array_item_t(path_bytes, cur - ptr));
        cur += path_bytes;
    }

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

    ret->execfn_offset = cur - ptr;
    memcpy(cur, path, path_bytes);
    cur += path_bytes;

    ret->data_ptr = ptr;
    ret->size = count_bytes;

    return ret;
}

void copy_fd(handle_t<fs::vfs::file> file, process_t *new_proc, process_t *old_proc, flag_t flags)
{
    kassert(new_proc != old_proc, "2 parameter processes assert failed");
    auto root = fs::vfs::global_root;

    if (unlikely(flags & create_process_flags::no_shared_root))
        new_proc->bootstrap_root_directory = handle_t<fs::vfs::native_directory>::make(root, root);
    else
        new_proc->bootstrap_root_directory = old_proc->bootstrap_root_directory;

    if (unlikely(flags & create_process_flags::no_shared_work_dir))
    {
        const auto current_root =
            new_proc->bootstrap_root_directory ? new_proc->bootstrap_root_directory->root() : root;
        const auto current = file ? file->get_entry()->get_parent() : current_root;
        new_proc->bootstrap_current_directory = handle_t<fs::vfs::native_directory>::make(current_root, current);
    }
    else
        new_proc->bootstrap_current_directory = old_proc->bootstrap_current_directory;

    auto copy_console = [](resource_table_t &source, resource_table_t &destination, na_handle_t source_handle,
                           na_handle_t &destination_handle) {
        destination_handle = NA_HANDLE_INVALID;
        if (source_handle == NA_HANDLE_INVALID)
            return;
        capability::entry entry;
        if (!source.lookup_native(source_handle, entry) || !entry.object)
            return;
        destination_handle = destination.install_native(entry.object, entry.meta);
    };

    const bool share_all = (flags & create_process_flags::no_shared_files) == 0;
    if (share_all || (flags & create_process_flags::no_shared_stdin) == 0)
        copy_console(old_proc->resource, new_proc->resource, old_proc->console_in_handle, new_proc->console_in_handle);
    if (share_all || (flags & create_process_flags::no_shared_stdout) == 0)
        copy_console(old_proc->resource, new_proc->resource, old_proc->console_out_handle,
                     new_proc->console_out_handle);
    if (share_all || (flags & create_process_flags::no_shared_stderror) == 0)
        copy_console(old_proc->resource, new_proc->resource, old_proc->console_err_handle,
                     new_proc->console_err_handle);
}

process_t *create_process(handle_t<fs::vfs::file> file, const char *path, thread_start_func start_func,
                          const char *const args[], const char *const envp[], flag_t flags)
{
    auto process = new_process();
    if (!process)
        return nullptr;

    auto *parent = current_process();
    {
        uctx::RawSpinLockUninterruptibleContext icu(process_list_lock);
        process->parent_pid = parent->pid;
        process->controlling_tty = parent->controlling_tty;
        process->signal_pack.inherit_mask_from(parent->signal_pack);
        move_process_session_unlocked(process, parent->session_id, parent->process_group_id);
    }
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
        abort_unstarted_process(process);
        return nullptr;
    }
    memory::KernelCommonAllocatorV->deallocate(header);

    /// create thread
    thread_t *thd = new_thread(process);
    if (!thd)
    {
        abort_unstarted_process(process);
        return nullptr;
    }
    process->main_thread = thd;
    thd->attributes |= thread_attributes::main;
    thd->state = thread_state::ready;

    auto process_args = copy_args(path, args, envp);
    if (process_args == nullptr)
    {
        abort_unstarted_process(process);
        return nullptr;
    }
    process_args->program_header = exec_info.program_header;
    process_args->program_header_entry_size = exec_info.program_header_entry_size;
    process_args->program_header_count = exec_info.program_header_count;
    process_args->base_address = exec_info.base_address;
    process_args->hwcap = exec_info.hwcap;

    arch::task::create_thread(thd, (void *)befor_run_process, reinterpret_cast<u64>(start_func),
                              reinterpret_cast<u64>(process_args), 0,
                              reinterpret_cast<u64>(exec_info.entry_start_address));

    thd->user_stack_top = exec_info.stack_top;
    thd->user_stack_bottom = exec_info.stack_bottom;
    paging.map_kernel_space();

    if (flags & create_process_flags::real_time_rr)
        thd->attributes |= thread_attributes::real_time;
    if ((flags & create_process_flags::deferred_start) == 0)
        start_process(process);

    return process;
}

void start_process(process_t *process)
{
    if (process == nullptr || process->main_thread == nullptr)
        return;
    bool expected = false;
    if (!process->main_thread_started.compare_exchange_strong(expected, true))
        return;

    auto *thread = process->main_thread;
    if (thread->attributes & thread_attributes::real_time)
        scheduler::add(thread, scheduler::scheduler_class::round_robin);
    else
        scheduler::add(thread, scheduler::scheduler_class::cfs);
}

void abort_unstarted_process(process_t *process)
{
    if (process == nullptr || process->main_thread_started.load())
        return;

    process->resource.clear();
    process->bootstrap_root_directory.reset();
    process->bootstrap_current_directory.reset();
    if (process->main_thread != nullptr)
    {
        process->main_thread->state = thread_state::destroy;
        delete_thread(process->main_thread);
        process->main_thread = nullptr;
    }
    process->attributes |= process_attributes::no_thread;
    process->wait_queue.do_wake_up();
    delete_process(process);
}

process_t *create_kernel_process(thread_start_func start_func, void *arg, flag_t flags)
{
    auto process = new_kernel_process();
    if (!process)
        return nullptr;

    auto *parent = current_process();
    {
        uctx::RawSpinLockUninterruptibleContext icu(process_list_lock);
        process->parent_pid = parent->pid;
        process->controlling_tty = parent->controlling_tty;
        process->signal_pack.inherit_mask_from(parent->signal_pack);
        move_process_session_unlocked(process, parent->session_id, parent->process_group_id);
    }
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
    auto *parent = current_process();
    auto process = copy_process(parent);
    if (!process)
        return -1;

    if (process->resource.clone_fork_bindings(parent->resource) != NA_STATUS_OK)
    {
        delete_process(process);
        return -1;
    }
    process->bootstrap_root_directory = parent->bootstrap_root_directory;
    process->bootstrap_current_directory = parent->bootstrap_current_directory;
    process->console_in_handle = parent->console_in_handle;
    process->console_out_handle = parent->console_out_handle;
    process->console_err_handle = parent->console_err_handle;

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
    process_args->program_header = exec_info.program_header;
    process_args->program_header_entry_size = exec_info.program_header_entry_size;
    process_args->program_header_count = exec_info.program_header_count;
    process_args->base_address = exec_info.base_address;
    process_args->hwcap = exec_info.hwcap;
    thd->user_stack_top = exec_info.stack_top;
    thd->user_stack_bottom = exec_info.stack_bottom;

    befor_run_process(start_func, process_args, 0, exec_info.entry_start_address);
    return 0;
}

void thread_t::wake_from_sleep(timeclock::microsecond_t) noexcept
{
    scheduler::update_state(this, thread_state::ready);
}

void do_sleep(const timeclock::time &time)
{
    timeclock::time t = time;
    auto us = t.tv_nsec / 1000 + t.tv_sec * 1000 * 1000;
    uctx::UninterruptibleContext icu;

    if (us != 0)
    {
        scheduler::update_state(current(), thread_state::stop);
        (void)timer::schedule_after(us, timer::timer_handler::bind<&thread_t::wake_from_sleep>(*current()));
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
namespace
{
void cleanup_process_job_control(process_t *process);
void notify_parent_of_exit(process_t *process);
} // namespace

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
        naos::ipc::collect_orphaned_channels();
        process->attributes |= process_attributes::no_thread;
        if (process == get_init_process())
        {
            trace::panic("init process startup fail");
        }
        notify_parent_of_exit(process);
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
    if (ret != 0)
    {
        trace::debug("process ", process->pid, " exit with code ", ret);
    }
    process->ret_val = ret;
    cleanup_process_job_control(process);
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

namespace
{
struct child_wait_context
{
    process_t *parent;
    u64 generation;
};

bool child_wait_generation_changed(const child_wait_context *context)
{
    return context == nullptr || context->parent == nullptr ||
           context->parent->child_wait_generation.load() != context->generation;
}

struct child_selection
{
    process_t *process = nullptr;
    bool has_child = false;
};

bool matches_wait_pid(const process_t *parent, const process_t *child, i64 requested_pid)
{
    if (requested_pid == -1)
        return true;
    if (requested_pid > 0)
        return child->pid == static_cast<process_id>(requested_pid);
    if (requested_pid == 0)
        return child->process_group_id == parent->process_group_id;
    if (requested_pid == std::numeric_limits<i64>::min())
        return false;
    return child->process_group_id == static_cast<group_id>(-requested_pid);
}

child_selection select_child(process_t *parent, i64 requested_pid)
{
    child_selection selection;
    uctx::RawSpinLockUninterruptibleContext icu(process_list_lock);
    for (auto item : *global_process_map)
    {
        auto *child = item.value;
        if (child == nullptr || child->parent_pid != parent->pid ||
            (child->attributes.load() & process_attributes::destroy) || !matches_wait_pid(parent, child, requested_pid))
            continue;

        selection.has_child = true;
        if ((child->attributes.load() & process_attributes::no_thread) && !child->wait_claimed.load() &&
            selection.process == nullptr)
            selection.process = child;
    }
    return selection;
}

process_t *reserve_child(process_t *parent, i64 requested_pid, bool exited_only)
{
    uctx::RawSpinLockUninterruptibleContext icu(process_list_lock);
    for (auto item : *global_process_map)
    {
        auto *child = item.value;
        if (child == nullptr || child->parent_pid != parent->pid ||
            (child->attributes.load() & process_attributes::destroy) ||
            !matches_wait_pid(parent, child, requested_pid) || child->wait_claimed.load() ||
            (exited_only && !(child->attributes.load() & process_attributes::no_thread)))
            continue;
        child->wait_claimed.store(true);
        child->wait_counter++;
        return child;
    }
    return nullptr;
}

void notify_parent_of_exit(process_t *process)
{
    process_t *parent = nullptr;
    {
        uctx::RawSpinLockUninterruptibleContext icu(process_list_lock);
        if (global_process_map != nullptr)
            parent = global_process_map->get(process->parent_pid).value_or(nullptr);
        if (parent != nullptr)
            parent->child_wait_generation.fetch_add(1);
    }
    if (parent != nullptr)
        parent->child_wait_queue.do_wake_up();
}

u64 reap_waited_child(process_t *process, i64 &ret, process_id &waited_pid)
{
    ret = static_cast<i64>(na_process_wait_status_exit(static_cast<i64>(process->ret_val)));
    waited_pid = process->pid;
    if (--process->wait_counter == 0)
    {
        notify_parent_of_exit(process);
        process->attributes |= process_attributes::destroy;
        delete_process(process);
    }
    return 0;
}

} // namespace

i64 wait_process_handle(process_t *parent, process_t *target, flag_t flags, i64 &ret, process_id &waited_pid)
{
    if (parent == nullptr || target == nullptr || target->parent_pid != parent->pid || target->reap_pending.load())
        return ECHILD;

    uctx::UninterruptibleContext icu;
    if (target->attributes.load() & process_attributes::no_thread)
    {
        auto *reserved = reserve_child(parent, target->pid, true);
        return reserved == target ? static_cast<i64>(reap_waited_child(reserved, ret, waited_pid)) : ECHILD;
    }
    if (flags & NA_PROCESS_WAIT_FLAG_NOHANG) // WNOHANG
    {
        waited_pid = 0;
        return 0;
    }

    auto *reserved = reserve_child(parent, target->pid, false);
    if (reserved == nullptr)
        return ECHILD;
    reserved->wait_queue.do_wait([reserved] { return reserved->attributes & process_attributes::no_thread; });
    return static_cast<i64>(reap_waited_child(reserved, ret, waited_pid));
}

i64 open_process_handle(process_t *caller, i64 requested_pid, khandle &object)
{
    object.reset();
    if (caller == nullptr || global_process_map == nullptr || requested_pid < 0 ||
        static_cast<u64>(requested_pid) > max_process_id)
        return ECHILD;

    uctx::RawSpinLockUninterruptibleContext icu(process_list_lock);
    auto *target =
        requested_pid == 0 ? caller : global_process_map->get(static_cast<process_id>(requested_pid)).value_or(nullptr);
    if (target == nullptr || target->reap_pending.load() || (target != caller && target->parent_pid != caller->pid))
        return ECHILD;
    object = handle_t<process_object>::make(target);
    return object ? 0 : EFAILED;
}

i64 wait_process_children(process_t *parent, i64 requested_pid, flag_t flags, i64 &ret, process_id &waited_pid)
{
    if (parent == nullptr || global_process_map == nullptr)
        return ECHILD;

    uctx::UninterruptibleContext icu;

    const bool wait_any = requested_pid <= 0;
    for (;;)
    {
        child_wait_context context{parent, parent->child_wait_generation.load()};
        auto selection = select_child(parent, requested_pid);
        if (selection.process != nullptr)
        {
            auto target = reserve_child(parent, requested_pid, true);
            if (target != nullptr)
                return reap_waited_child(target, ret, waited_pid);
            continue;
        }
        if (!selection.has_child)
            return ECHILD;
        if (flags & NA_PROCESS_WAIT_FLAG_NOHANG) // WNOHANG
        {
            waited_pid = 0;
            return 0;
        }

        if (wait_any)
        {
            parent->child_wait_queue.do_wait([&context] { return child_wait_generation_changed(&context); });
        }
        else
        {
            auto target = reserve_child(parent, requested_pid, false);
            if (target == nullptr)
                return ECHILD;
            target->wait_queue.do_wait([target] { return target->attributes & process_attributes::no_thread; });
            return reap_waited_child(target, ret, waited_pid);
        }
    }
}

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
    thd->wait_queue.do_wait([thd] { return thd->state == thread_state::destroy; });

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

void stop_thread(thread_t *thread, flag_t flags)
{
    (void)flags;
    if (thread == nullptr || thread->state == thread_state::destroy)
        return;
    thread->attributes |= thread_attributes::job_control_stopped;
    if (thread->state == thread_state::ready || thread->state == thread_state::running)
        scheduler::update_state(thread, thread_state::stop);
}

void continue_thread(thread_t *thread, flag_t flags)
{
    (void)flags;
    if (thread == nullptr || thread->state == thread_state::destroy)
        return;
    thread->attributes &= ~(thread_attributes::job_control_stopped);
    if (thread->state == thread_state::stop || thread->state == thread_state::running)
        scheduler::update_state(thread, thread_state::ready);
}

void stop_process(process_t *process, flag_t flags)
{
    (void)flags;
    if (process == nullptr || (process->attributes.load() & process_attributes::no_thread))
        return;

    process->attributes |= process_attributes::job_control_stopped;
    uctx::RawSpinLockUninterruptibleContext icu(process->thread_list_lock);
    auto &list = *(thread_list_t *)process->thread_list;
    for (auto *thread : list)
        stop_thread(thread, 0);
}

void continue_process(process_t *process, flag_t flags)
{
    (void)flags;
    if (process == nullptr)
        return;

    process->attributes &= ~(process_attributes::job_control_stopped);
    uctx::RawSpinLockUninterruptibleContext icu(process->thread_list_lock);
    auto &list = *(thread_list_t *)process->thread_list;
    for (auto *thread : list)
        continue_thread(thread, 0);
}

namespace
{
bool process_is_live(const process_t *process)
{
    return process != nullptr && !(process->attributes.load() & process_attributes::no_thread);
}

bool process_group_exists_unlocked(::session_id session_id, group_id process_group, const process_t *exclude = nullptr)
{
    auto *group = find_process_group_unlocked(process_group);
    if (group == nullptr || group->session != session_id)
        return false;
    if (exclude == nullptr)
        return group->members.size() != 0;
    if (!group->members.has(exclude->pid))
        return group->members.size() != 0;
    return group->members.size() > 1;
}

process_t *find_session_leader_unlocked(::session_id session_id)
{
    auto *session = find_session_unlocked(session_id);
    auto leader = session == nullptr ? nullptr : session->members.get(session_id).value_or(nullptr);
    if (process_is_live(leader) && leader->pid == leader->session_id)
        return leader;
    return nullptr;
}

void detach_session_tty_unlocked(::session_id session_id, dev::tty::tty_core *tty)
{
    auto *session = find_session_unlocked(session_id);
    if (session == nullptr)
        return;

    for (auto item : session->members)
    {
        auto *process = item.value;
        if (process->controlling_tty == tty)
            process->controlling_tty = nullptr;
    }

    if (session->controlling_tty == tty)
    {
        session->controlling_tty = nullptr;
        session->foreground_process_group = 0;
        auto *leader = find_session_leader_unlocked(session_id);
        if (leader != nullptr)
            leader->foreground_process_group = 0;
        tty->set_foreground_process_group(0);
        tty->set_session_id(0);
    }
}

void cleanup_process_job_control(process_t *process)
{
    if (process == nullptr)
        return;

    auto old_attributes = process->attributes.fetch_or(process_attributes::job_control_cleanup_done);
    if (old_attributes & process_attributes::job_control_cleanup_done)
        return;

    uctx::RawSpinLockUninterruptibleContext icu(process_list_lock);

    auto *tty = process->controlling_tty;
    if (tty != nullptr && process->pid == process->session_id)
    {
        detach_session_tty_unlocked(process->session_id, tty);
    }
    else if (tty != nullptr)
    {
        process->controlling_tty = nullptr;
    }

    auto *session = find_session_unlocked(process->session_id);
    if (session != nullptr && session->foreground_process_group == process->process_group_id &&
        !process_group_exists_unlocked(process->session_id, process->process_group_id, process))
    {
        session->foreground_process_group = 0;
        auto *leader = find_session_leader_unlocked(process->session_id);
        if (leader != nullptr)
            leader->foreground_process_group = 0;
        if (session->controlling_tty != nullptr)
            session->controlling_tty->set_foreground_process_group(0);
    }

    unregister_process_job_control_unlocked(process);
}
} // namespace

int setpgid(process_t *caller, process_id pid, group_id pgid)
{
    if (caller == nullptr || global_process_map == nullptr)
        return EPARAM;

    uctx::RawSpinLockUninterruptibleContext icu(process_list_lock);

    auto *target = pid == 0 ? caller : global_process_map->get(pid).value_or(nullptr);
    if (!process_is_live(target))
        return ENOEXIST;
    if (target != caller && target->parent_pid != caller->pid)
        return EPERMISSION;
    if (target->session_id != caller->session_id)
        return EPERMISSION;
    if (target->pid == target->session_id)
        return EPERMISSION;

    if (pgid == 0)
        pgid = target->pid;

    if (!process_group_exists_unlocked(target->session_id, pgid) && pgid != target->pid)
        return ENOEXIST;

    move_process_group_unlocked(target, pgid);
    auto *session = find_session_unlocked(target->session_id);
    if (session != nullptr)
    {
        auto *leader = find_session_leader_unlocked(target->session_id);
        if (leader != nullptr)
            leader->foreground_process_group = session->foreground_process_group;
    }

    return OK;
}

bool get_job_control_info(const process_t *process, job_control_info &info)
{
    if (process == nullptr || global_process_map == nullptr)
        return false;

    uctx::RawSpinLockUninterruptibleContext icu(process_list_lock);
    info.session = process->session_id;
    info.process_group = process->process_group_id;
    info.foreground_process_group = process->session == nullptr ? 0 : process->session->foreground_process_group;
    info.has_controlling_tty = process->session != nullptr && process->session->controlling_tty != nullptr;
    return true;
}

i64 setsid(process_t *process)
{
    if (process == nullptr || global_process_map == nullptr)
        return EPARAM;

    uctx::RawSpinLockUninterruptibleContext icu(process_list_lock);
    if (!process_is_live(process))
        return ENOEXIST;
    if (process->process_group_id == process->pid)
        return EPERMISSION;

    process->controlling_tty = nullptr;
    process->foreground_process_group = 0;
    move_process_session_unlocked(process, process->pid, process->pid);
    return process->session_id;
}

int attach_controlling_tty(process_t *process, dev::tty::tty_core *tty, bool force)
{
    if (process == nullptr || tty == nullptr || global_process_map == nullptr)
        return EPARAM;

    uctx::RawSpinLockUninterruptibleContext icu(process_list_lock);
    if (!process_is_live(process))
        return ENOEXIST;
    if (process->pid != process->session_id)
        return EPERMISSION;
    if (process->controlling_tty != nullptr && process->controlling_tty != tty && !force)
        return ERESOURCE_NOT_NULL;

    ::session_id foreign_session = tty->session_id();
    auto *foreign = find_session_unlocked(foreign_session);
    if (foreign == nullptr || foreign->controlling_tty != tty || foreign_session == process->session_id)
        foreign_session = 0;

    if (foreign_session != 0 && !force)
        return EPERMISSION;
    if (foreign_session != 0)
        detach_session_tty_unlocked(foreign_session, tty);

    if (process->controlling_tty != nullptr && process->controlling_tty != tty)
        detach_session_tty_unlocked(process->session_id, process->controlling_tty);

    auto *session = find_session_unlocked(process->session_id);
    if (session == nullptr)
        return ENOEXIST;

    process->controlling_tty = tty;
    session->controlling_tty = tty;
    session->foreground_process_group = process->process_group_id;
    process->foreground_process_group = process->process_group_id;
    tty->set_session_id(process->session_id);
    tty->set_foreground_process_group(process->process_group_id);
    return OK;
}

void detach_controlling_tty(process_t *process)
{
    if (process == nullptr || global_process_map == nullptr)
        return;

    uctx::RawSpinLockUninterruptibleContext icu(process_list_lock);
    auto *tty = process->controlling_tty;
    if (tty == nullptr)
        return;

    if (process->pid == process->session_id)
        detach_session_tty_unlocked(process->session_id, tty);
    else
        process->controlling_tty = nullptr;
}

i64 get_foreground_process_group(dev::tty::tty_core *tty)
{
    if (tty == nullptr || global_process_map == nullptr)
        return EPARAM;

    uctx::RawSpinLockUninterruptibleContext icu(process_list_lock);
    auto *session = find_session_unlocked(tty->session_id());
    if (session != nullptr && session->controlling_tty == tty)
        return session->foreground_process_group;
    return ENOEXIST;
}

int set_foreground_process_group(process_t *process, dev::tty::tty_core *tty, group_id pgid)
{
    if (process == nullptr || tty == nullptr || pgid == 0 || global_process_map == nullptr)
        return EPARAM;

    const auto job_control = check_tty_job_control(process, tty, false, true);
    if (job_control != 0)
        return static_cast<int>(job_control);

    uctx::RawSpinLockUninterruptibleContext icu(process_list_lock);
    if (!process_is_live(process) || process->controlling_tty != tty)
        return EPERMISSION;

    auto *session = find_session_unlocked(process->session_id);
    if (session == nullptr || session->controlling_tty != tty)
        return ENOEXIST;
    auto *leader = find_session_leader_unlocked(process->session_id);
    if (leader == nullptr)
        return ENOEXIST;
    if (!process_group_exists_unlocked(process->session_id, pgid))
        return ENOEXIST;

    session->foreground_process_group = pgid;
    leader->foreground_process_group = pgid;
    tty->set_foreground_process_group(pgid);
    return OK;
}

i64 check_tty_job_control(process_t *process, dev::tty::tty_core *tty, bool input, bool tostop)
{
    if (process == nullptr || tty == nullptr)
        return EPARAM;
    if (process->controlling_tty != tty || process->session_id != tty->session_id())
        return OK;

    const auto foreground_group = tty->foreground_process_group();
    if (foreground_group == 0 || foreground_group == process->process_group_id)
        return OK;
    if (!input && !tostop)
        return OK;

    const auto signal_number = input ? signal::sigttin : signal::sigttou;
    if (input && process->signal_pack.is_ignored_or_blocked(signal_number))
        return EIO;
    if (!input && process->signal_pack.is_ignored_or_blocked(signal_number))
        return OK;

    const auto result = send_signal_to_process_group(process->process_group_id, signal_number);
    return result < 0 ? result : EINTR;
}

dev::tty::tty_core *get_controlling_tty(process_t *process)
{
    return process == nullptr ? nullptr : process->controlling_tty;
}

i64 send_signal_to_process_group(group_id process_group, signal_num_t num, i64 error, i64 code, i64 status)
{
    if (global_process_group_map == nullptr || process_group == 0 || num >= max_signal_count)
        return EPARAM;

    {
        freelibcxx::vector<process_t *> recipients(memory::KernelCommonAllocatorV);
        {
            uctx::RawSpinLockUninterruptibleContext icu(process_list_lock);
            auto *group = find_process_group_unlocked(process_group);
            if (group == nullptr)
                return ENOEXIST;
            for (auto item : group->members)
            {
                auto *process = item.value;
                if (process_is_live(process))
                    recipients.push_back(process);
            }
        }

        i64 count = 0;
        for (auto *process : recipients)
        {
            if (!process_is_live(process))
                continue;
            if (num != 0)
                process->signal_pack.send(process, num, error, code, status);
            count++;
        }

        return count == 0 ? ENOEXIST : count;
    }
}

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
