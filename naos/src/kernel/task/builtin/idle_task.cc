#include "kernel/task/builtin/idle_task.hpp"
#include "kernel/arch/idt.hpp"
#include "kernel/ipc/invocation.hpp"
#include "kernel/kernel.hpp"
#include "kernel/log.hpp"
#include "kernel/mm/data_plane.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/scheduler.hpp"
#include "kernel/service_directory.hpp"
#include "kernel/smp.hpp"
#include "kernel/task.hpp"
#include "kernel/task/builtin/input_task.hpp"
#include "kernel/task/builtin/soft_irq_task.hpp"
#include "naos/generated/system/MemoryObject.hpp"

namespace task::builtin::vfsd
{
void main(task::thread_start_info_t *info);
}

namespace task::builtin::ramdiskd
{
void main(task::thread_start_info_t *info);
}

KLOG_MODULE(task);
namespace task::builtin::idle
{
namespace
{
/// Look up a boot module by its fixed authority token; returns
/// nullptr when the module was not provided at boot.
const named_boot_module *find_boot_module(kernel_start_args *boot_args, const char *name)
{
    if (boot_args == nullptr)
        return nullptr;
    for (u64 i = 0; i < boot_args->named_module_count; i++)
    {
        auto &module = boot_args->named_modules[i];
        if (module.size != 0 && strcmp(module.name, name) == 0)
            return &module;
    }
    return nullptr;
}

task::process_t *create_early_module(const named_boot_module &module, const char *path,
                                     task::thread_start_func entry)
{
    // kernel_start_args is packed: copy fields before binding them.
    const u64 image_addr = module.start;
    const u64 image_size = module.size;
    auto *image = memory::pa2va<byte *>(phy_addr_t::from(image_addr));
    if (image == nullptr || image_size == 0)
        KLOG_PANIC("boot module {} is invalid", path);
    KLOG_INFO("boot module {} found ({} bytes)", path, image_size);

    // Immutable zero-copy exec image over the relocated module bytes.
    auto image_object = handle_t<naos::data_plane::memory_object>::make(image, image_size);
    if (!image_object || image_object->size() != image_size)
        KLOG_PANIC("Can't create exec image for boot module {}", path);
    khandle image_backing(image_object.get_control());

    auto *process = task::create_process(std::move(image_object), std::move(image_backing), path, entry, nullptr,
                                         nullptr, create_process_flags::deferred_start);
    if (process == nullptr)
        KLOG_PANIC("Can't load ELF of boot module {}", path);
    return process;
}

/// Publish a boot module as a one-shot, read-only MemoryObject for the
/// userland boot manager.  The kernel owns the initial module bytes, but does
/// not decide when the filesystem worker or init process is created.
void publish_boot_module(const named_boot_module &module, const char *uri, u64 uri_size)
{
    const u64 image_addr = module.start;
    const u64 image_size = module.size;
    auto *image = memory::pa2va<byte *>(phy_addr_t::from(image_addr));
    if (image == nullptr || image_size == 0)
        KLOG_PANIC("boot module {} is invalid", module.name);

    auto image_object = handle_t<naos::data_plane::memory_object>::make(image, image_size);
    if (!image_object || image_object->size() != image_size)
        KLOG_PANIC("Can't publish boot module {}", module.name);
    khandle service_object(image_object.get_control());

    capability::metadata metadata;
    metadata.binding = NA_BINDING_MEMORY_OBJECT;
    metadata.protocol_uuid = naos::system::MemoryObject::protocol_uuid;
    metadata.scope = NA_SCOPE_MEMORY_OBJECT;
    metadata.revision = naos::system::MemoryObject::revision;
    metadata.meta_rights = NA_RIGHT_TRANSFER | NA_RIGHT_INSPECT | NA_RIGHT_WAIT;
    metadata.protocol_rights = NA_MEMORY_RIGHT_READ | NA_MEMORY_RIGHT_MAP | NA_MEMORY_RIGHT_INFO;
    metadata.view_offset = 0;
    metadata.view_length = image_size;
    const auto status = service::register_kernel_service(uri, uri_size, std::move(service_object), metadata, true);
    if (status != 0)
        KLOG_PANIC("Unable to publish boot module {} (status {})", module.name, status);
    KLOG_INFO("boot module {} published service={}", module.name, uri);
}

void start_early_module(task::process_t *process)
{
    if (process == nullptr)
        return;
    if (task::setsid(process) < 0)
        KLOG_WARN("Unable to create boot module session");
    task::start_process(process);
}

/// Launch the early service from its dedicated Multiboot module
/// (USERSPACE_FILESYSTEM_ADR §5.1.1-§5.1.4). The kernel neither opens
/// /bin/init nor resolves filesystem data; any module failure stops the boot
/// instead of falling back to kernel VFS.
task::process_t *launch_vfsd(const named_boot_module &module)
{
    auto *vfsd_process = create_early_module(module, "/vfsd", builtin::vfsd::main);
    return vfsd_process;
}

/// Launch the optional ramdiskd block-backend boot module (VFS ADR §7.1).
/// ramdiskd owns its ramdisk storage, LBA policy, and public IDL endpoints.
void launch_blockd(const named_boot_module &module)
{
    auto *ramdiskd_process = create_early_module(module, "/ramdiskd", builtin::ramdiskd::main);
    start_early_module(ramdiskd_process);
}

} // namespace

std::atomic_bool is_init = false;
void main(void *arg)
{
    KLOG_DEBUG("idle task running at cpu {}", cpu::current().id());
    if (cpu::current().is_bsp())
    {
        log::start_workers();
        auto p = task::create_kernel_process(builtin::softirq::main, 0, create_thread_flags::real_time_rr);
        KLOG_DEBUG("softirqd created tid={}", p->main_thread->tid);
        kassert(p->pid == 1, "BUG check failed.");
        is_init = true;
        task::create_kernel_process(builtin::input::main, 0, create_thread_flags::real_time_rr);
        naos::ipc::init_kernel_dispatch_worker();
        naos::data_plane::init_pager_worker();

        // The early service owns the root namespace and spawns init.  Phase 4
        // deliberately has no kernel-rootfs fallback: booting without vfsd
        // is a configuration error, rather than silently selecting the old
        // global_root/native_directory path.
        auto *boot_args = ::kernel_args;
        if (boot_args == nullptr)
            KLOG_PANIC("kernel boot arguments missing; vfsd is required");
        // Unknown self-declared boot modules are ignored but logged so new
        // tokens are diagnosable at debug level. The fixed authorities are
        // the only modules that participate in the boot contract.
        if (boot_args != nullptr)
        {
            for (u64 i = 0; i < boot_args->named_module_count; i++)
            {
                auto &module = boot_args->named_modules[i];
                if (strcmp(module.name, "vfsd") != 0 && strcmp(module.name, "blockd") != 0 &&
                    strcmp(module.name, "rootfsd") != 0 && strcmp(module.name, "init") != 0 &&
                    strcmp(module.name, "rootimage") != 0)
                    KLOG_DEBUG("ignoring unknown boot module '{}'", module.name);
            }
        }
        for (u64 i = 0; i < boot_args->named_module_count; i++)
        {
            auto &entry = boot_args->named_modules[i];
            const byte *entry_bytes = memory::pa2va<byte *>(phy_addr_t::from(entry.start));
            KLOG_INFO("mod-table[{}] name={} start={:x} size={:x} head={:x}{:x}{:x}{:x}", i,
                      static_cast<const char *>(entry.name), static_cast<u64>(entry.start),
                      static_cast<u64>(entry.size), static_cast<u64>(entry_bytes[0]), static_cast<u64>(entry_bytes[1]),
                      static_cast<u64>(entry_bytes[2]), static_cast<u64>(entry_bytes[3]));
        }
        const named_boot_module *vfsd_module = find_boot_module(boot_args, "vfsd");
        if (vfsd_module == nullptr)
            KLOG_PANIC("vfsd boot module missing; kernel VFS fallback disabled");
        KLOG_INFO("starting vfsd early module");
        auto *vfsd_process = launch_vfsd(*vfsd_module);
        // vfsd is the trusted userland boot manager in both modes.  It owns
        // the decision to create the filesystem worker and the eventual init;
        // the kernel only treats the manager as the root process for fatal
        // lifecycle handling.
        set_init_process(vfsd_process);

        // Publish every one-shot image before starting vfsd. Otherwise
        // scheduler timing can let vfsd consume the rootfsd URI before the
        // later init/root-image registrations.
        const named_boot_module *blockd_module = find_boot_module(boot_args, "blockd");
        const named_boot_module *rootfsd_module = find_boot_module(boot_args, "rootfsd");
        const named_boot_module *init_module = find_boot_module(boot_args, "init");
        const named_boot_module *root_image_module = find_boot_module(boot_args, "rootimage");
        if (blockd_module == nullptr || rootfsd_module == nullptr || init_module == nullptr ||
            root_image_module == nullptr)
            KLOG_PANIC("mounted-root boot requires blockd, rootfsd, init, and rootimage modules");
        publish_boot_module(*rootfsd_module, service::boot_module_rootfsd_uri,
                            sizeof(service::boot_module_rootfsd_uri) - 1);
        publish_boot_module(*init_module, service::boot_module_init_uri,
                            sizeof(service::boot_module_init_uri) - 1);
        publish_boot_module(*root_image_module, service::boot_root_image_uri,
                            sizeof(service::boot_root_image_uri) - 1);
        launch_blockd(*blockd_module);
        // All boot resources are now visible in ServiceDirectory, so the
        // userland manager cannot observe a partially published bootstrap.
        start_early_module(vfsd_process);
    }
    else
    {
        while (!is_init)
        {
            cpu_pause();
        }
        /// soft irq process
        auto t = task::create_thread(task::find_pid(1), builtin::softirq::main, nullptr, 0,
                                     create_thread_flags::real_time_rr);
        KLOG_DEBUG("softirqd created tid={}", t->tid);
    }

    task::thread_yield();
    while (1)
    {
        kassert(arch::idt::is_enable(), "Bug check failed.");
        cpu_halt();
    }
}
} // namespace task::builtin::idle
