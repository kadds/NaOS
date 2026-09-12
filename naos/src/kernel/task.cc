#include "kernel/task.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/arch/mm.hpp"
#include "kernel/arch/paging.hpp"
#include "kernel/arch/task.hpp"

#include "kernel/handle.hpp"
#include "kernel/ipc/channel.hpp"
#include "kernel/kobject.hpp"
#include "kernel/mm/data_plane.hpp"
#include "kernel/mm/list_node_cache.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/mm/slab.hpp"

#include "freelibcxx/hash_map.hpp"
#include "freelibcxx/string.hpp"
#include "freelibcxx/vector.hpp"
#include "kernel/log.hpp"
#include "kernel/terminal.hpp"
#include "kernel/terminal_identity.hpp"
#include "kernel/terminal_views.hpp"
#include "kernel/time.hpp"
#include "kernel/types.hpp"
#include "kernel/util/id_generator.hpp"
#include "naos/generated/system/InputEventSource.hpp"
#include "naos/generated/system/Framebuffer.hpp"
#include "naos/generated/system/TerminalDriverFactory.hpp"
#include "naos/generated/system_uapi.h"

#include "kernel/input_event_source.hpp"
#include "kernel/service_directory.hpp"

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
#include <utility>

#include "kernel/dev/framebuffer.hpp"

KLOG_MODULE(kernel);
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
    dev::tty::terminal_identity *controlling_terminal = nullptr;
    handle_t<dev::tty::terminal_identity> controlling_terminal_ref;
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
constexpr u64 process_name_capacity = 12;

void set_process_name(process_t &process, const char *path)
{
    const char *name = path;
    if (name != nullptr)
    {
        for (const char *cursor = path; *cursor != '\0'; cursor++)
        {
            if (*cursor == '/')
                name = cursor + 1;
        }
    }
    if (name == nullptr || *name == '\0')
        name = "process";

    const u64 length = strlen(name) < process_name_capacity ? strlen(name) : process_name_capacity;
    memcpy(process.name, name, length);
    process.name[length] = '\0';
}

capability::metadata stream_capability_metadata()
{
    capability::metadata metadata;
    metadata.binding = NA_BINDING_KERNEL_VIEW;
    metadata.protocol_uuid = naos::system::Stream::protocol_uuid;
    metadata.scope = NA_SCOPE_STREAM;
    metadata.revision = naos::system::Stream::revision;
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
    if (session == nullptr || session->members.size() != 0 || session->controlling_terminal != nullptr)
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
    process->controlling_terminal_ref = session->controlling_terminal_ref;
    process->controlling_terminal = session->controlling_terminal;
    process->foreground_process_group = session->foreground_process_group;
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
        if (session->controlling_terminal != nullptr)
            session->controlling_terminal->set_foreground_process_group(0);
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
    set_process_name(*process, "kernel");
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
    set_process_name(*process, "process");
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
    memcpy(process->name, p->name, sizeof(process->name));
    process->parent_pid = p->pid;
    process->session_id = p->session_id;
    process->process_group_id = p->process_group_id;
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
    while (thd->wait_queue_wake_refs.load(std::memory_order_acquire) != 0)
        cpu_pause();

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
    , main_thread(nullptr)
    , ret_val(0)
    , thread_list(nullptr)
    , schedule_data(nullptr)
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

std::atomic_bool is_init = false, init_ok = false;
bool has_init() { return is_init; }

void sync_current_kernel_space()
{
    auto *process = current_process();
    if (process == nullptr || process->mm_info == memory::kernel_vm_info)
        return;
    reinterpret_cast<mm_info_t *>(process->mm_info)->paging().map_kernel_space();
}

void init()
{
    process_t *process;
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
    KLOG_DEBUG("Idle process (pid={}) thread (tid={}) init", process->pid, thd->tid);

    if (cpu::current().is_bsp())
    {
        auto global_directory = handle_t<service::directory>::make();
        service::set_global_service_directory(global_directory);
        auto input_handle = dev::input::init_input_event_source();
        auto factory_handle = handle_t<dev::tty::terminal_driver_factory>::make();
        auto tty0read = handle_t<dev::tty::console_stream>::make(term::terminal_manager::kernel_console_index);
        auto tty0write = handle_t<dev::tty::console_stream>::make(term::terminal_manager::kernel_console_index);
        auto tty0err = handle_t<dev::tty::console_stream>::make(term::terminal_manager::kernel_console_index);
        kassert(tty0read && tty0write && tty0err, "unable to create bootstrap console streams");
        auto *init_process = current_process();
        capability::metadata input_meta;
        input_meta.binding = NA_BINDING_KERNEL_VIEW;
        input_meta.protocol_uuid = naos::system::InputEventSource::protocol_uuid;
        input_meta.scope = NA_SCOPE_INPUT_EVENT_SOURCE;
        input_meta.revision = naos::system::InputEventSource::revision;
        input_meta.meta_rights = NA_RIGHT_DUPLICATE | NA_RIGHT_TRANSFER | NA_RIGHT_WAIT | NA_RIGHT_INSPECT;
        input_meta.protocol_rights =
            NA_RIGHT_DUPLICATE | NA_RIGHT_TRANSFER | NA_RIGHT_WAIT | NA_RIGHT_INSPECT | NA_PROTOCOL_RIGHT_INVOKE;
        if (service::register_kernel_service(service::input_event_source_uri,
                                              sizeof(service::input_event_source_uri) - 1,
                                              handle_t<kobject>(input_handle.get_control()), input_meta) != 0)
            KLOG_PANIC("unable to publish input event source service");
        const auto input_event_source_handle =
            init_process->resource.install_native(std::move(input_handle), input_meta);
        kassert(input_event_source_handle != NA_HANDLE_INVALID, "unable to install input event source capability");

        capability::metadata factory_meta;
        factory_meta.binding = NA_BINDING_KERNEL_VIEW;
        factory_meta.protocol_uuid = naos::system::TerminalDriverFactory::protocol_uuid;
        factory_meta.scope = NA_SCOPE_TERMINAL_DRIVER_FACTORY;
        factory_meta.revision = naos::system::TerminalDriverFactory::revision;
        factory_meta.meta_rights = NA_RIGHT_TRANSFER | NA_RIGHT_WAIT | NA_RIGHT_INSPECT;
        factory_meta.protocol_rights = NA_RIGHT_TRANSFER | NA_RIGHT_WAIT | NA_RIGHT_INSPECT | NA_PROTOCOL_RIGHT_INVOKE;
        if (service::register_kernel_service(service::terminal_driver_factory_uri,
                                              sizeof(service::terminal_driver_factory_uri) - 1,
                                              handle_t<kobject>(factory_handle.get_control()), factory_meta) != 0)
            KLOG_PANIC("unable to publish terminal driver factory service");
        const auto terminal_driver_factory_handle =
            init_process->resource.install_native(std::move(factory_handle), factory_meta);
        kassert(terminal_driver_factory_handle != NA_HANDLE_INVALID,
                "unable to install terminal driver factory capability");

        auto *framebuffer_backend = term::get_framebuffer_backend();
        kassert(framebuffer_backend != nullptr, "unable to access early framebuffer backend");
        const auto &framebuffer = framebuffer_backend->fb();
        const u64 page_mask = memory::page_size - 1;
        const u64 physical_offset = reinterpret_cast<uintptr_t>(framebuffer.physical_addr()) & page_mask;
        const auto physical_base = phy_addr_t::from(
            memory::align_down(framebuffer.physical_addr(), memory::page_size));
        auto *kernel_view = memory::align_down(static_cast<byte *>(framebuffer.ptr), memory::page_size);
        const u64 framebuffer_bytes = memory::align_up(framebuffer_backend->frame_bytes() + physical_offset,
                                                       memory::page_size);
        auto framebuffer_handle = handle_t<dev::framebuffer::framebuffer_service>::make(
            physical_base, kernel_view, framebuffer_bytes, framebuffer.width, framebuffer.height, framebuffer.pitch,
            framebuffer.bbp, framebuffer.bbp == 32 ? 0 : 1);
        kassert(framebuffer_handle, "unable to create framebuffer service");
        capability::metadata framebuffer_meta;
        framebuffer_meta.binding = NA_BINDING_KERNEL_VIEW;
        framebuffer_meta.protocol_uuid = naos::system::Framebuffer::protocol_uuid;
        framebuffer_meta.scope = NA_SCOPE_FRAMEBUFFER;
        framebuffer_meta.revision = naos::system::Framebuffer::revision;
        framebuffer_meta.features = naos::system::Framebuffer::features;
        framebuffer_meta.meta_rights = NA_RIGHT_DUPLICATE | NA_RIGHT_TRANSFER | NA_RIGHT_WAIT | NA_RIGHT_INSPECT;
        framebuffer_meta.protocol_rights = NA_DISPLAY_RIGHT_WRITER | NA_PROTOCOL_RIGHT_INVOKE;
        const auto framebuffer_register_status =
            service::register_kernel_service(service::framebuffer_uri, sizeof(service::framebuffer_uri) - 1,
                                             handle_t<kobject>(framebuffer_handle.get_control()), framebuffer_meta);
        KLOG_INFO("framebuffer service registration status {}", framebuffer_register_status);
        if (framebuffer_register_status != 0)
            KLOG_PANIC("unable to publish framebuffer service");
        const auto framebuffer_capability =
            init_process->resource.install_native(std::move(framebuffer_handle), framebuffer_meta);
        kassert(framebuffer_capability != NA_HANDLE_INVALID, "unable to install framebuffer capability");
        const auto metadata = stream_capability_metadata();
        init_process->console_in_handle =
            init_process->resource.install_native(khandle(tty0read.get_control()), metadata);
        init_process->console_out_handle =
            init_process->resource.install_native(khandle(tty0write.get_control()), metadata);
        init_process->console_err_handle =
            init_process->resource.install_native(khandle(tty0err.get_control()), metadata);
        kassert(init_process->console_in_handle != NA_HANDLE_INVALID &&
                    init_process->console_out_handle != NA_HANDLE_INVALID &&
                    init_process->console_err_handle != NA_HANDLE_INVALID,
                "unable to install bootstrap console capabilities");
        (void)input_event_source_handle;
        (void)terminal_driver_factory_handle;
        is_init = true;
        term::get_terms()->switch_term(term::terminal_manager::user_terminal_index);

        bin_handle::init();
        init_ok = true;
    }
    while (!init_ok)
        cpu_pause();
}

thread_t *create_thread(process_t *process, thread_start_func start_func, void *entry, void *arg, flag_t flags,
                        void *tcb)
{
    if (process == nullptr || start_func == nullptr)
        return nullptr;
    thread_t *thd = new_thread(process);
    if (thd == nullptr)
        return nullptr;
    thd->state = thread_state::ready;

    auto &vma = ((mm_info_t *)process->mm_info)->vma();

    if (process->mm_info != memory::kernel_vm_info)
    {
        auto stack_vm = vma.allocate_map(memory::user_stack_maximum_size,
                                         memory::vm::flags::readable | memory::vm::flags::writeable |
                                             memory::vm::flags::expand | memory::vm::flags::user_mode,
                                         memory::vm::page_fault_method::common, 0);

        if (stack_vm == nullptr)
        {
            thd->state = thread_state::destroy;
            delete_thread(thd);
            return nullptr;
        }

        thd->user_stack_top = (void *)stack_vm->end;
        thd->user_stack_bottom = (void *)stack_vm->start;
    }

    thd->cpumask.mask = cpumask_none;

    thread_start_info_t *info = memory::New<thread_start_info_t>(memory::KernelCommonAllocatorV);
    if (info == nullptr)
    {
        if (process->mm_info != memory::kernel_vm_info && thd->user_stack_bottom != nullptr &&
            thd->user_stack_top != nullptr && thd->user_stack_top > thd->user_stack_bottom)
        {
            auto *mm_info = reinterpret_cast<mm_info_t *>(process->mm_info);
            (void)mm_info->unmap(reinterpret_cast<u64>(thd->user_stack_bottom),
                                 reinterpret_cast<u64>(thd->user_stack_top) -
                                     reinterpret_cast<u64>(thd->user_stack_bottom));
        }
        thd->state = thread_state::destroy;
        delete_thread(thd);
        return nullptr;
    }
    info->args = arg;
    info->userland_entry = entry;
    // create_thread enters a normal user function directly (there is no
    // synthetic return address on the stack).  Reserve one word so the
    // function observes the SysV x86-64 ABI entry alignment that it would
    // have after a call.  The ELF process entry path below deliberately uses
    // a different initial-stack layout and must remain 16-byte aligned.
    info->userland_stack_offset = sizeof(void *);
    info->tcb = tcb;

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
    // An exec replaces the address space and must let the new runtime build a
    // fresh TLS/TCB.  Do not pass an uninitialised pointer through
    // before_user_thread(), which would install arbitrary FS base state in a
    // forked child and make the first userland call fail nondeterministically.
    info->tcb = nullptr;

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

void copy_fd(process_t *new_proc, process_t *old_proc, flag_t flags)
{
    kassert(new_proc != old_proc, "2 parameter processes assert failed");

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

process_t *create_process(handle_t<naos::data_plane::memory_object> object, khandle backing, const char *path,
                          thread_start_func start_func, const char *const args[], const char *const envp[], flag_t flags)
{
    auto process = new_process();
    if (!process)
        return nullptr;

    auto *parent = current_process();
    {
        uctx::RawSpinLockUninterruptibleContext icu(process_list_lock);
        process->parent_pid = parent->pid;
        process->signal_pack.inherit_mask_from(parent->signal_pack);
        move_process_session_unlocked(process, parent->session_id, parent->process_group_id);
    }
    set_process_name(*process, path);

    copy_fd(process, current_process(), flags);

    auto mm_info = (mm_info_t *)process->mm_info;
    auto &paging = mm_info->paging();
    // read ELF header 128 bytes
    byte *header = (byte *)memory::KernelCommonAllocatorV->allocate(128, 8);
    bin_handle::execute_info exec_info;
    bool loaded = false;
    u64 header_read = 0;
    const auto header_status = object->read(0, header, 128, header_read);
    if (header_status != NA_STATUS_OK || header_read != 128)
        KLOG_WARN("read ELF header for {} returned {} object size {}", path, header_read, object->size());
    loaded = bin_handle::load(header, object, backing, mm_info, &exec_info);
    if (!loaded)
    {
        memory::KernelCommonAllocatorV->deallocate(header);
        KLOG_WARN("Can't load execute file for {}.", path);
        abort_unstarted_process(process);
        return nullptr;
    }
    memory::KernelCommonAllocatorV->deallocate(header);

    /// create thread
    thread_t *thd = new_thread(process);
    if (!thd)
    {
        KLOG_WARN("Can't allocate main thread for {}.", path);
        abort_unstarted_process(process);
        return nullptr;
    }
    process->main_thread = thd;
    thd->attributes |= thread_attributes::main;
    thd->state = thread_state::ready;

    auto process_args = copy_args(path, args, envp);
    if (process_args == nullptr)
    {
        KLOG_WARN("Can't copy process arguments for {}.", path);
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
        process->signal_pack.inherit_mask_from(parent->signal_pack);
        move_process_session_unlocked(process, parent->session_id, parent->process_group_id);
    }
    copy_fd(process, current_process(), flags);

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

int execve(handle_t<naos::data_plane::memory_object> object, khandle backing, const char *path,
           thread_start_func start_func, char *const argv[], char *const envp[])
{
    auto thd = current();
    auto process = thd->process;
    set_process_name(*process, path);
    auto process_args = copy_args(path, argv, envp);
    if (process_args == nullptr)
        return ENOMEM;

    // The address space is process-wide. Refuse an in-place exec while a
    // sibling user thread is live; replacing mm_info underneath it would
    // leave the sibling executing on freed stacks and mappings.
    {
        uctx::RawSpinLockUninterruptibleContext guard(process->thread_list_lock);
        for (auto *candidate : *(thread_list_t *)process->thread_list)
        {
            if (candidate != thd && candidate->state != thread_state::destroy)
            {
                memory::DeleteArray(memory::KernelCommonAllocatorV, process_args->data_ptr, process_args->size);
                memory::Delete(memory::KernelCommonAllocatorV, process_args);
                return EBUSY;
            }
        }
    }

    auto *old_mm_info = (mm_info_t *)process->mm_info;
    auto new_mm_info = memory::New<mm_info_t>(mm_info_t_allocator);
    if (new_mm_info == nullptr)
    {
        memory::DeleteArray(memory::KernelCommonAllocatorV, process_args->data_ptr, process_args->size);
        memory::Delete(memory::KernelCommonAllocatorV, process_args);
        return ENOMEM;
    }
    new_mm_info->paging().map_kernel_space();

    // read ELF header 128 bytes
    byte *header = (byte *)memory::KernelCommonAllocatorV->allocate(128, 8);
    if (header == nullptr)
    {
        memory::Delete(mm_info_t_allocator, new_mm_info);
        memory::DeleteArray(memory::KernelCommonAllocatorV, process_args->data_ptr, process_args->size);
        memory::Delete(memory::KernelCommonAllocatorV, process_args);
        return ENOMEM;
    }
    bin_handle::execute_info exec_info;
    u64 header_read = 0;
    const auto header_status = object->read(0, header, 128, header_read);
    if (header_status != NA_STATUS_OK || header_read != 128 ||
        !bin_handle::load(header, object, backing, new_mm_info, &exec_info))
    {
        memory::KernelCommonAllocatorV->deallocate(header);
        KLOG_INFO("Can't load execute file.");
        memory::DeleteArray(memory::KernelCommonAllocatorV, process_args->data_ptr, process_args->size);
        memory::Delete(memory::KernelCommonAllocatorV, process_args);
        memory::Delete(mm_info_t_allocator, new_mm_info);
        return ENOEXEC;
    }
    memory::KernelCommonAllocatorV->deallocate(header);
    process_args->program_header = exec_info.program_header;
    process_args->program_header_entry_size = exec_info.program_header_entry_size;
    process_args->program_header_count = exec_info.program_header_count;
    process_args->base_address = exec_info.base_address;
    process_args->hwcap = exec_info.hwcap;
    {
        uctx::UninterruptibleContext ctx;
        process->mm_info = new_mm_info;
        new_mm_info->paging().load();
    }
    memory::Delete(mm_info_t_allocator, old_mm_info);
    // enter_userland() does not return, so C++ destructors for locals on this
    // syscall stack are never run. Drop the temporary source references here;
    // PT_LOAD mappings already own the backing references they require.
    object.reset();
    backing.reset();
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

bool claim_thread_exit(thread_t *thd)
{
    const auto previous = thd->attributes.fetch_or(thread_attributes::exit_pending, std::memory_order_acq_rel);
    return (previous & thread_attributes::exit_pending) == 0;
}

void exit_process_inner(thread_t *thd);
namespace
{
void cleanup_process_job_control(process_t *process);
void notify_parent_of_child_state_change(process_t *process);
} // namespace

void exit_process_thread(process_t *process)
{
    uctx::RawSpinLockUninterruptibleController icu(process->thread_list_lock);
    auto &list = *(thread_list_t *)process->thread_list;

    icu.begin();
    thread_t *next = nullptr;
    for (auto candidate : list)
    {
        if (candidate->state != thread_state::destroy)
        {
            next = candidate;
            break;
        }
    }
    if (next != nullptr)
    {
        icu.end();
        exit_process_inner(next);
    }
    else if (list.empty())
    {
        icu.end();
        auto services = service::get_global_service_directory();
        if (services)
            services->cleanup_owner(process->pid);
        process->resource.clear();
        naos::ipc::collect_orphaned_channels();
        process->attributes |= process_attributes::no_thread;
        if (process == get_init_process())
        {
            KLOG_PANIC("init process startup fail");
        }
        notify_parent_of_child_state_change(process);
        process->wait_queue.do_wake_up();
    }
    else
    {
        // A concurrent thread-exit callback has already marked every
        // remaining thread destroy, but has not removed the last one yet.
        // Its callback will retry exit_process_thread after delete_thread().
        icu.end();
        return;
    }
}

void exit_process_inner(thread_t *thd)
{
    if (thd->state != thread_state::destroy)
    {
        if (!claim_thread_exit(thd))
            return;
        process_data_t *data = memory::New<process_data_t>(memory::KernelCommonAllocatorV);
        if (data == nullptr)
        {
            thd->attributes.fetch_and(~thread_attributes::exit_pending, std::memory_order_release);
            KLOG_WARN("unable to allocate process-exit cleanup for pid {} tid {}", thd->process->pid, thd->tid);
            return;
        }
        data->thd = thd;

        scheduler::remove(
            thd,
            [](u64 data) {
                auto *dt = reinterpret_cast<process_data_t *>(data);
                auto process = dt->thd->process;

                dt->thd->state = thread_state::destroy;
                dt->thd->attributes.fetch_and(~thread_attributes::exit_pending, std::memory_order_release);
                dt->thd->wait_queue.do_wake_up();
                delete_thread(dt->thd);

                memory::Delete<>(memory::KernelCommonAllocatorV, dt);
                exit_process_thread(process);
            },
            (u64)data);
    }
    else
    {
        // The process-exit walk is concurrent with a thread-exit callback.
        // The callback owns the eventual list removal and will retry the
        // process walk after it has completed.
        return;
    }
}

void exit_process(process_t *process, i64 ret, flag_t flags)
{
    const auto previous = process->attributes.fetch_or(process_attributes::exiting, std::memory_order_acq_rel);
    if (previous & process_attributes::exiting)
        return;

    // TODO: write core_dump from flags
    if (ret != 0)
    {
        KLOG_DEBUG("process {} exit with code {}", process->pid, ret);
    }
    process->ret_val = ret;
    cleanup_process_job_control(process);
    exit_process_thread(process);
}

NoReturn void do_exit(i64 ret)
{
    process_t *process = current_process();
    exit_process(process, ret, 0);
    for (;;)
    {
        scheduler::schedule();
        cpu_pause();
    }
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
    process_t *stopped_process = nullptr;
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

child_selection select_child(process_t *parent, i64 requested_pid, bool want_stop)
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
        if (want_stop && (child->attributes.load() & process_attributes::job_control_stopped) != 0 &&
            !child->wait_stop_reported.load() && !child->wait_claimed.load() && selection.stopped_process == nullptr)
            selection.stopped_process = child;
    }
    return selection;
}

process_t *reserve_child(process_t *parent, i64 requested_pid, bool exited_only, bool stopped_only = false)
{
    uctx::RawSpinLockUninterruptibleContext icu(process_list_lock);
    for (auto item : *global_process_map)
    {
        auto *child = item.value;
        if (child == nullptr || child->parent_pid != parent->pid ||
            (child->attributes.load() & process_attributes::destroy) ||
            !matches_wait_pid(parent, child, requested_pid) || child->wait_claimed.load() ||
            (exited_only && !(child->attributes.load() & process_attributes::no_thread)) ||
            (stopped_only && ((child->attributes.load() & process_attributes::job_control_stopped) == 0 ||
                              child->wait_stop_reported.load())))
            continue;
        child->wait_claimed.store(true);
        child->wait_counter++;
        return child;
    }
    return nullptr;
}

bool report_stopped_child(process_t *process, i64 &ret, process_id &waited_pid)
{
    if (process == nullptr || (process->attributes.load() & process_attributes::job_control_stopped) == 0 ||
        process->wait_stop_reported.load())
        return false;

    bool expected = false;
    if (!process->wait_stop_reported.compare_exchange_strong(expected, true))
        return false;
    process->wait_claimed.store(false);
    --process->wait_counter;
    ret = (process->last_stop_signal << 8) | 0x7f;
    waited_pid = process->pid;
    return true;
}

void notify_parent_of_child_state_change(process_t *process)
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
        notify_parent_of_child_state_change(process);
        process->attributes |= process_attributes::destroy;
        delete_process(process);
    }
    return 0;
}

} // namespace

i64 wait_process_handle(process_t *parent, process_t *target, flag_t flags, i64 &ret, process_id &waited_pid)
{
    return wait_process_handle(parent, target, flags, ret, waited_pid, nullptr, nullptr);
}

i64 wait_process_handle(process_t *parent, process_t *target, flag_t flags, i64 &ret, process_id &waited_pid,
                        freelibcxx::function_ref<bool()> interrupt,
                        freelibcxx::function_ref<void(wait_queue_t *)> register_wait_queue)
{
    if (parent == nullptr || target == nullptr || target->parent_pid != parent->pid || target->reap_pending.load())
        return ECHILD;

    uctx::UninterruptibleContext icu;
    const bool want_stop = (flags & NA_PROCESS_WAIT_FLAG_UNTRACED) != 0;
    const auto interrupted = [&]() { return interrupt != nullptr && interrupt(); };
    if (interrupted())
        return EINTR;
    auto stopped_unreported = [&]() {
        return want_stop && (target->attributes.load() & process_attributes::job_control_stopped) != 0 &&
               !target->wait_stop_reported.load();
    };
    if (target->attributes.load() & process_attributes::no_thread)
    {
        auto *reserved = reserve_child(parent, target->pid, true);
        return reserved == target ? static_cast<i64>(reap_waited_child(reserved, ret, waited_pid)) : ECHILD;
    }
    if (stopped_unreported())
    {
        bool expected = false;
        if (target->wait_stop_reported.compare_exchange_strong(expected, true))
        {
            ret = (target->last_stop_signal << 8) | 0x7f;
            waited_pid = target->pid;
            return 0;
        }
    }
    if (flags & NA_PROCESS_WAIT_FLAG_NOHANG) // WNOHANG
    {
        waited_pid = 0;
        return 0;
    }

    auto *reserved = reserve_child(parent, target->pid, false);
    if (reserved == nullptr)
        return ECHILD;
    if (register_wait_queue)
        register_wait_queue(&reserved->wait_queue);
    reserved->wait_queue.do_wait([reserved, stopped_unreported, interrupted] {
        return (reserved->attributes.load() & process_attributes::no_thread) != 0 || stopped_unreported() ||
               interrupted();
    });
    if (register_wait_queue)
        register_wait_queue(nullptr);
    if (interrupted())
    {
        reserved->wait_claimed.store(false);
        --reserved->wait_counter;
        return EINTR;
    }
    if (reserved->attributes.load() & process_attributes::no_thread)
        return static_cast<i64>(reap_waited_child(reserved, ret, waited_pid));
    if (stopped_unreported())
    {
        bool expected = false;
        if (reserved->wait_stop_reported.compare_exchange_strong(expected, true))
        {
            reserved->wait_claimed.store(false);
            if (--reserved->wait_counter == 0)
            {
                // No exit reap is pending; keep the process object alive.
            }
            ret = (reserved->last_stop_signal << 8) | 0x7f;
            waited_pid = reserved->pid;
            return 0;
        }
    }
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
    return wait_process_children(parent, requested_pid, flags, ret, waited_pid, nullptr, nullptr);
}

i64 wait_process_children(process_t *parent, i64 requested_pid, flag_t flags, i64 &ret, process_id &waited_pid,
                          freelibcxx::function_ref<bool()> interrupt,
                          freelibcxx::function_ref<void(wait_queue_t *)> register_wait_queue)
{
    if (parent == nullptr || global_process_map == nullptr)
        return ECHILD;

    uctx::UninterruptibleContext icu;

    const bool wait_any = requested_pid <= 0;
    const bool want_stop = (flags & NA_PROCESS_WAIT_FLAG_UNTRACED) != 0;
    const auto interrupted = [&]() { return interrupt != nullptr && interrupt(); };
    for (;;)
    {
        if (interrupted())
            return EINTR;
        child_wait_context context{parent, parent->child_wait_generation.load()};
        auto selection = select_child(parent, requested_pid, want_stop);
        if (selection.process != nullptr)
        {
            auto target = reserve_child(parent, requested_pid, true);
            if (target != nullptr)
                return reap_waited_child(target, ret, waited_pid);
            continue;
        }
        if (selection.stopped_process != nullptr)
        {
            auto target = reserve_child(parent, requested_pid, false, true);
            if (target != nullptr && report_stopped_child(target, ret, waited_pid))
                return 0;
            if (target != nullptr)
            {
                target->wait_claimed.store(false);
                --target->wait_counter;
            }
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
            if (register_wait_queue)
                register_wait_queue(&parent->child_wait_queue);
            parent->child_wait_queue.do_wait(
                [&context, interrupted] { return child_wait_generation_changed(&context) || interrupted(); });
            if (register_wait_queue)
                register_wait_queue(nullptr);
        }
        else
        {
            auto target = reserve_child(parent, requested_pid, false);
            if (target == nullptr)
                return ECHILD;
            if (register_wait_queue)
                register_wait_queue(&target->wait_queue);
            target->wait_queue.do_wait([target, want_stop, interrupted] {
                return (target->attributes.load() & process_attributes::no_thread) != 0 ||
                       (want_stop && (target->attributes.load() & process_attributes::job_control_stopped) != 0 &&
                        !target->wait_stop_reported.load()) ||
                       interrupted();
            });
            if (register_wait_queue)
                register_wait_queue(nullptr);
            if (interrupted())
            {
                target->wait_claimed.store(false);
                --target->wait_counter;
                return EINTR;
            }
            if (target->attributes.load() & process_attributes::no_thread)
                return reap_waited_child(target, ret, waited_pid);
            if (report_stopped_child(target, ret, waited_pid))
                return 0;
            target->wait_claimed.store(false);
            --target->wait_counter;
        }
    }
}

void exit_thread(thread_t *thd, i64 ret)
{
    KLOG_DEBUG("exit thread {} pid {} code {}", thd->tid, thd->process->pid, ret);
    if (!claim_thread_exit(thd))
        return;
    struct data_t
    {
        thread_t *thd;
        i64 ret;
    };
    data_t *data = memory::New<data_t>(memory::KernelCommonAllocatorV);
    if (data == nullptr)
    {
        thd->attributes.fetch_and(~thread_attributes::exit_pending, std::memory_order_release);
        KLOG_WARN("unable to allocate thread-exit cleanup for pid {} tid {}", thd->process->pid, thd->tid);
        return;
    }
    data->thd = thd;
    data->ret = ret;
    scheduler::remove(
        thd,
        [](u64 data) {
            auto *dt = reinterpret_cast<data_t *>(data);
            auto *process = dt->thd->process;
            if (!(dt->thd->attributes & thread_attributes::main) && process->mm_info != memory::kernel_vm_info &&
                dt->thd->user_stack_bottom != nullptr && dt->thd->user_stack_top != nullptr &&
                dt->thd->user_stack_top > dt->thd->user_stack_bottom)
            {
                auto *mm_info = reinterpret_cast<mm_info_t *>(process->mm_info);
                (void)mm_info->unmap(reinterpret_cast<u64>(dt->thd->user_stack_bottom),
                                     reinterpret_cast<u64>(dt->thd->user_stack_top) -
                                         reinterpret_cast<u64>(dt->thd->user_stack_bottom));
            }
            dt->thd->user_stack_top = (void *)dt->ret;
            dt->thd->state = thread_state::destroy;
            dt->thd->attributes.fetch_and(~thread_attributes::exit_pending, std::memory_order_release);
            if (dt->thd->attributes & thread_attributes::detached)
            {
                delete_thread(dt->thd);
            }
            else
            {
                dt->thd->wait_queue.do_wake_up();
            }
            memory::Delete<>(memory::KernelCommonAllocatorV, dt);
            if (process->attributes.load(std::memory_order_acquire) & process_attributes::exiting)
                exit_process_thread(process);
        },
        reinterpret_cast<u64>(data));
}

NoReturn void do_exit_thread(i64 ret)
{
    auto thd = current();
    exit_thread(thd, ret);
    for (;;)
    {
        scheduler::schedule();
        cpu_pause();
    }
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

    const auto previous = process->attributes.fetch_or(process_attributes::job_control_stopped);
    {
        uctx::RawSpinLockUninterruptibleContext icu(process->thread_list_lock);
        auto &list = *(thread_list_t *)process->thread_list;
        for (auto *thread : list)
            stop_thread(thread, 0);
    }
    process->wait_queue.do_wake_up();
    if ((previous & process_attributes::job_control_stopped) == 0)
    {
        notify_parent_of_child_state_change(process);
    }
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

void detach_session_terminal_unlocked(::session_id session_id, dev::tty::terminal_identity *terminal)
{
    auto *session = find_session_unlocked(session_id);
    if (session == nullptr)
        return;

    if (session->controlling_terminal == terminal)
    {
        session->controlling_terminal_ref.reset();
        session->controlling_terminal = nullptr;
        session->foreground_process_group = 0;
        terminal->set_foreground_process_group(0);
        terminal->set_session_id(0);

        for (auto item : session->members)
        {
            auto *process = item.value;
            process->controlling_terminal_ref.reset();
            process->controlling_terminal = nullptr;
            process->foreground_process_group = 0;
        }
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

    auto *session = find_session_unlocked(process->session_id);
    auto *terminal = session == nullptr ? nullptr : session->controlling_terminal;
    if (terminal != nullptr && process->pid == process->session_id)
    {
        detach_session_terminal_unlocked(process->session_id, terminal);
    }
    else if (terminal != nullptr)
    {
        process->controlling_terminal_ref.reset();
        process->controlling_terminal = nullptr;
    }

    if (session != nullptr && session->foreground_process_group == process->process_group_id &&
        !process_group_exists_unlocked(process->session_id, process->process_group_id, process))
    {
        session->foreground_process_group = 0;
        auto *leader = find_session_leader_unlocked(process->session_id);
        if (leader != nullptr)
            leader->foreground_process_group = 0;
        if (session->controlling_terminal != nullptr)
            session->controlling_terminal->set_foreground_process_group(0);
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
    info.has_controlling_tty = process->session != nullptr && process->session->controlling_terminal != nullptr;
    return true;
}

bool get_controlling_terminal_locator(const process_t *process, na_terminal_locator_t &locator)
{
    locator = {};
    if (process == nullptr || global_process_map == nullptr)
        return false;

    uctx::RawSpinLockUninterruptibleContext icu(process_list_lock);
    auto *session = find_session_unlocked(process->session_id);
    auto *terminal = session == nullptr ? nullptr : session->controlling_terminal;
    if (terminal == nullptr || !terminal->live())
        return false;
    locator.terminal_id = terminal->id();
    locator.generation = terminal->generation();
    for (u64 i = 0; i < sizeof(locator.token); i++)
        locator.token[i] = terminal->token()[i];
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

    process->controlling_terminal_ref.reset();
    process->controlling_terminal = nullptr;
    process->foreground_process_group = 0;
    move_process_session_unlocked(process, process->pid, process->pid);
    return process->session_id;
}

int attach_controlling_terminal(process_t *process, handle_t<dev::tty::terminal_identity> terminal_ref, bool force)
{
    auto *terminal = terminal_ref.operator&();
    if (process == nullptr || terminal == nullptr || global_process_map == nullptr)
        return EPARAM;

    uctx::RawSpinLockUninterruptibleContext icu(process_list_lock);
    if (!process_is_live(process))
        return ENOEXIST;
    if (process->pid != process->session_id)
        return EPERMISSION;
    auto *session = find_session_unlocked(process->session_id);
    if (session == nullptr)
        return ENOEXIST;
    if (session->controlling_terminal != nullptr && session->controlling_terminal != terminal && !force)
        return ERESOURCE_NOT_NULL;

    ::session_id foreign_session = terminal->session_id();
    auto *foreign = find_session_unlocked(foreign_session);
    if (foreign == nullptr || foreign->controlling_terminal != terminal || foreign_session == process->session_id)
        foreign_session = 0;

    if (foreign_session != 0 && !force)
        return EPERMISSION;
    if (foreign_session != 0)
        detach_session_terminal_unlocked(foreign_session, terminal);

    if (session->controlling_terminal != nullptr && session->controlling_terminal != terminal)
        detach_session_terminal_unlocked(process->session_id, session->controlling_terminal);

    session->controlling_terminal_ref = terminal_ref;
    session->controlling_terminal = terminal;
    session->foreground_process_group = process->process_group_id;
    for (auto item : session->members)
    {
        auto *member = item.value;
        member->controlling_terminal_ref = session->controlling_terminal_ref;
        member->controlling_terminal = session->controlling_terminal;
        member->foreground_process_group = session->foreground_process_group;
    }
    terminal->set_session_id(process->session_id);
    terminal->set_foreground_process_group(process->process_group_id);
    return OK;
}

void detach_controlling_terminal(process_t *process)
{
    if (process == nullptr || global_process_map == nullptr)
        return;

    uctx::RawSpinLockUninterruptibleContext icu(process_list_lock);
    auto *session = find_session_unlocked(process->session_id);
    auto *terminal = session == nullptr ? nullptr : session->controlling_terminal;
    if (terminal == nullptr)
        return;

    if (process->pid == process->session_id)
        detach_session_terminal_unlocked(process->session_id, terminal);
    else
    {
        process->controlling_terminal_ref.reset();
        process->controlling_terminal = nullptr;
    }
}

void detach_session_terminal(dev::tty::terminal_identity *terminal)
{
    if (terminal == nullptr || global_process_map == nullptr)
        return;

    uctx::RawSpinLockUninterruptibleContext icu(process_list_lock);
    const auto session_id = terminal->session_id();
    if (session_id != 0)
        detach_session_terminal_unlocked(session_id, terminal);
}

i64 get_foreground_process_group(dev::tty::terminal_identity *terminal)
{
    if (terminal == nullptr || global_process_map == nullptr)
        return EPARAM;

    uctx::RawSpinLockUninterruptibleContext icu(process_list_lock);
    auto *session = find_session_unlocked(terminal->session_id());
    if (session != nullptr && session->controlling_terminal == terminal)
        return session->foreground_process_group;
    return ENOEXIST;
}

int set_foreground_process_group(process_t *process, dev::tty::terminal_identity *terminal, group_id pgid)
{
    if (process == nullptr || terminal == nullptr || pgid == 0 || global_process_map == nullptr)
        return EPARAM;

    const auto job_control = check_terminal_job_control(process, terminal, false, true, false);
    if (job_control != 0)
        return static_cast<int>(job_control);

    uctx::RawSpinLockUninterruptibleContext icu(process_list_lock);
    if (!process_is_live(process) || process->session == nullptr || process->session->controlling_terminal != terminal)
        return EPERMISSION;

    auto *session = find_session_unlocked(process->session_id);
    if (session == nullptr || session->controlling_terminal != terminal)
        return ENOEXIST;
    auto *leader = find_session_leader_unlocked(process->session_id);
    if (leader == nullptr)
        return ENOEXIST;
    if (!process_group_exists_unlocked(process->session_id, pgid))
        return ENOEXIST;
    if (!terminal->try_set_foreground_process_group(pgid))
        return EAGAIN;

    session->foreground_process_group = pgid;
    leader->foreground_process_group = pgid;
    return OK;
}

i64 check_terminal_job_control(process_t *process, dev::tty::terminal_identity *terminal, bool input, bool tostop,
                               bool acquire_io_lease)
{
    if (process == nullptr || terminal == nullptr)
        return EPARAM;
    if (process->session == nullptr || process->session->controlling_terminal != terminal ||
        process->session_id != terminal->session_id())
        return OK;

    const auto foreground_group = terminal->foreground_process_group();
    const bool foreground = foreground_group == 0 || foreground_group == process->process_group_id;
    if (foreground)
    {
        if (!acquire_io_lease)
            return OK;
        return terminal->try_acquire_io_lease(process->process_group_id) ? OK : EAGAIN;
    }
    if (!input && !tostop)
    {
        if (!acquire_io_lease)
            return OK;
        return terminal->try_acquire_unrestricted_io_lease() ? OK : EAGAIN;
    }

    const auto signal_number = input ? signal::sigttin : signal::sigttou;
    if (input && process->signal_pack.is_ignored_or_blocked(signal_number))
        return EIO;
    if (!input && process->signal_pack.is_ignored_or_blocked(signal_number))
    {
        if (!acquire_io_lease)
            return OK;
        return terminal->try_acquire_unrestricted_io_lease() ? OK : EAGAIN;
    }

    const auto result = send_signal_to_process_group(process->process_group_id, signal_number);
    return result < 0 ? result : EINTR;
}

dev::tty::terminal_identity *get_controlling_terminal(process_t *process)
{
    return process == nullptr || process->session == nullptr ? nullptr : process->session->controlling_terminal;
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

ExportC void userland_return()
{
    // Keep the scheduler decision and the final return-path state check
    // atomic with respect to local interrupts.  Otherwise another interrupt
    // could mark this thread stopped after the check but before sysret.
    arch::idt::disable();
    scheduler::schedule();

    // A syscall must never return through sysret for a thread that has been
    // stopped or destroyed.  Exit is normally non-returning and switches
    // away through scheduler::remove(), but this guard also covers the case
    // where scheduling returned without changing the current context.
    auto thd = current();
    if (thd != nullptr && thd->state == thread_state::running && !(thd->attributes & thread_attributes::block_to_stop))
        return;

    for (;;)
    {
        scheduler::schedule();
        cpu_pause();
    }
}

void set_tcb(thread_t *t, void *p)
{
    t->tcb = p;
    // KLOG_INFO("process {} thread {} set tcb {}", t->process->pid, t->tid, log::hex(p));
    arch::task::update_fs(t);
}

void write_main_stack(thread_t *thread, main_stack_data_t stack)
{
    memcpy(thread->user_stack_top, &stack, sizeof(stack));
}

} // namespace task
