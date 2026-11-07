#include "kernel/acpi/resource_authority.hpp"

#include "kernel/acpi/fixed_hardware.hpp"
#include "kernel/arch/acpi/acpi.hpp"
#include "kernel/mm/data_plane.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/pci/discovery_fabric.hpp"
#include "kernel/pci/topology_lifecycle.hpp"
#include "kernel/task.hpp"
#include "kernel/ucontext.hpp"

namespace acpi
{
namespace
{
constexpr u64 page_size = 4096;

bool add_overflows(u64 left, u64 right) { return right > ~u64(0) - left; }

bool overlaps(u64 first, u64 first_length, u64 second, u64 second_length)
{
    if (first_length == 0 || second_length == 0 || add_overflows(first, first_length) ||
        add_overflows(second, second_length))
        return true;
    return first < second + second_length && second < first + first_length;
}

bool table_range_contains(const arch::acpi_boot::snapshot &snapshot, u64 base, u64 length)
{
    for (const auto &table : snapshot.tables)
    {
        if (table.physical_address != 0 && overlaps(base, length, table.physical_address, table.length))
            return true;
    }
    return false;
}

u64 process_owner(const task::thread_t *thread)
{
    return thread == nullptr || thread->process == nullptr ? physical_resource::ledger::kernel_owner()
                                                           : thread->process->pid;
}

void release_region_lease(u64 lease_id)
{
    (void)arch::ACPI::unmap_resource_kernel(lease_id);
    (void)arch::ACPI::revoke_resource_kernel(lease_id);
}

bool read_ecam_dword(void *, u64 function_page_base, u32 offset, u32 &value)
{
    if (offset > 0xffc || (offset & 3u) != 0 || function_page_base > ~u64(0) - offset)
        return false;
    auto *address = memory::pa2va<const u8 *>(phy_addr_t::from(function_page_base + offset));
    value = *reinterpret_cast<volatile const u32 *>(address);
    return true;
}

} // namespace

bool resource_authority::valid_identity(u64 boot_epoch, u64 firmware_generation) const
{
    return snapshot_ != nullptr && boot_epoch != 0 && firmware_generation != 0 && boot_epoch == snapshot_->boot_epoch &&
           firmware_generation == snapshot_->firmware_generation;
}

bool resource_authority::bind_execution_thread(u64 boot_epoch, u64 firmware_generation, u64 owner,
                                               task::thread_t *registrar_thread, u64 execution_thread_id)
{
    if (!valid_identity(boot_epoch, firmware_generation) || owner == physical_resource::ledger::kernel_owner() ||
        process_owner(registrar_thread) != owner || registrar_thread == nullptr ||
        registrar_thread->process == nullptr || registrar_thread->process->main_thread != registrar_thread ||
        execution_thread_id == 0)
        return false;
    auto *execution_thread = task::find_tid(registrar_thread->process, execution_thread_id);
    if (execution_thread == nullptr || execution_thread->process != registrar_thread->process ||
        execution_thread == registrar_thread)
        return false;
    uctx::LockGuard_t<lock::mutex_t> guard(execution_thread_lock_);
    return detail::bind_executor_thread(execution_thread_binding_, owner, true, execution_thread_id);
}

void resource_authority::unbind_execution_thread(u64 owner, u64 thread_id)
{
    uctx::LockGuard_t<lock::mutex_t> guard(execution_thread_lock_);
    detail::unbind_executor_thread(execution_thread_binding_, owner, thread_id);
}

bool resource_authority::unbind_execution_owner(u64 owner)
{
    bool owner_revoked = false;
    {
        uctx::LockGuard_t<lock::mutex_t> guard(execution_thread_lock_);
        owner_revoked = detail::unbind_executor_owner(execution_thread_binding_, owner);
    }
    return detail::invalidate_topology_on_executor_owner_exit(owner_revoked, [this] {
        if (snapshot_ != nullptr)
            (void)pci::topology_lifetime().invalidate(snapshot_->boot_epoch, snapshot_->firmware_generation);
    });
}

bool resource_authority::is_bound_execution_thread(u64 owner, const task::thread_t *execution_thread) const
{
    if (owner == physical_resource::ledger::kernel_owner() || process_owner(execution_thread) != owner)
        return false;
    uctx::LockGuard_t<lock::mutex_t> guard(execution_thread_lock_);
    return detail::executor_thread_matches(execution_thread_binding_, owner, execution_thread->tid);
}

global_lock_operation_result resource_authority::acquire_global_lock(u64 boot_epoch, u64 firmware_generation, u64 owner,
                                                                     task::thread_t *execution_thread, u64 timeout_us)
{
    if (!valid_identity(boot_epoch, firmware_generation))
        return global_lock_operation_result::stale_generation;
    if (execution_thread == nullptr || process_owner(execution_thread) != owner ||
        !is_bound_execution_thread(owner, execution_thread))
        return global_lock_operation_result::access_denied;
    auto fixed = arch::ACPI::fixed_hardware();
    if (!fixed)
        return global_lock_operation_result::unsupported;
    return fixed->acquire_global_lock(execution_thread->process, execution_thread, timeout_us);
}

global_lock_operation_result resource_authority::release_global_lock(u64 boot_epoch, u64 firmware_generation, u64 owner,
                                                                     task::thread_t *execution_thread)
{
    if (!valid_identity(boot_epoch, firmware_generation))
        return global_lock_operation_result::stale_generation;
    if (execution_thread == nullptr || process_owner(execution_thread) != owner ||
        !is_bound_execution_thread(owner, execution_thread))
        return global_lock_operation_result::access_denied;
    auto fixed = arch::ACPI::fixed_hardware();
    if (!fixed)
        return global_lock_operation_result::unsupported;
    return fixed->release_global_lock(execution_thread->process, execution_thread);
}

bool resource_authority::bind_pci_config_root(u64 boot_epoch, u64 firmware_generation, u64 namespace_generation,
                                              u64 platform_topology_generation, u64 compatibility_profile_hash,
                                              u64 owner, u64 root_id, u32 segment, u32 start_bus, u32 end_bus,
                                              u64 &scope_id)
{
    scope_id = 0;
    if (!valid_identity(boot_epoch, firmware_generation) || namespace_generation == 0 ||
        platform_topology_generation == 0 || compatibility_profile_hash == 0 ||
        owner == physical_resource::ledger::kernel_owner() || root_id == 0 || segment > 0xffff || start_bus > end_bus ||
        end_bus > 0xff || snapshot_ == nullptr || ledger_ == nullptr)
        return false;
    pci::platform_generation_lineage lineage{};
    if (!pci::topology_lifetime().get_platform_generation_lineage(lineage) || lineage.boot_epoch != boot_epoch ||
        lineage.firmware_generation != firmware_generation ||
        lineage.aml_namespace_generation != namespace_generation ||
        lineage.platform_topology_generation != platform_topology_generation ||
        lineage.pci_fabric_generation != snapshot_->pci_fabric_generation)
        return false;

    const arch::acpi_boot::mcfg_region *selected_region = nullptr;
    for (const auto &region : snapshot_->mcfg_regions)
    {
        if (region.segment != segment || start_bus < region.start_bus || end_bus > region.end_bus ||
            region.resource_lease_id == 0)
            continue;
        if (selected_region != nullptr)
            return false;
        selected_region = &region;
    }
    if (selected_region == nullptr)
        return false;

    physical_resource::lease mcfg_lease{};
    if (!ledger_->lookup(selected_region->resource_lease_id, mcfg_lease) ||
        mcfg_lease.resource_kind != physical_resource::kind::pci_ecam ||
        mcfg_lease.lifecycle != physical_resource::state::leased ||
        mcfg_lease.owner != physical_resource::ledger::kernel_owner() || mcfg_lease.generation != firmware_generation)
        return false;

    uctx::LockGuard_t<lock::mutex_t> guard(pci_config_scope_lock_);
    for (u32 index = 0; index < pci_config_scope_count_; ++index)
    {
        auto &scope = pci_config_scopes_[index];
        if (scope.owner != owner || scope.aml_namespace_generation != namespace_generation || scope.root_id != root_id)
            continue;
        if (scope.segment != segment || scope.start_bus != start_bus || scope.mcfg_lease_id != mcfg_lease.id ||
            end_bus < scope.end_bus || scope.platform_topology_generation != platform_topology_generation ||
            scope.compatibility_profile_hash != compatibility_profile_hash)
            return false;
        scope.end_bus = end_bus;
        scope_id = scope.id;
        return true;
    }

    if (pci_config_scope_count_ == max_pci_config_scopes || next_pci_config_scope_id_ == 0)
        return false;
    pci_config_scope scope{};
    scope.id = next_pci_config_scope_id_++;
    scope.owner = owner;
    scope.mcfg_lease_id = mcfg_lease.id;
    static_cast<pci::root_feature_scope &>(scope) = pci::bind_root_feature_scope(
        root_id, snapshot_->boot_epoch, snapshot_->firmware_generation, namespace_generation,
        platform_topology_generation, snapshot_->pci_fabric_generation, compatibility_profile_hash, segment, start_bus,
        end_bus);
    pci_config_scopes_[pci_config_scope_count_++] = scope;
    scope_id = scope.id;
    return true;
}

bool resource_authority::bind_platform_generation(u64 boot_epoch, u64 firmware_generation, u64 namespace_generation,
                                                  u64 platform_topology_generation, u64 owner)
{
    if (!valid_identity(boot_epoch, firmware_generation) || namespace_generation == 0 ||
        platform_topology_generation == 0 || owner == physical_resource::ledger::kernel_owner() ||
        !detail::platform_generations_match_owner(owner, namespace_generation, platform_topology_generation))
        return false;

    {
        uctx::LockGuard_t<lock::mutex_t> guard(execution_thread_lock_);
        if (!detail::executor_owner_matches(execution_thread_binding_, owner))
            return false;
    }
    return pci::topology_lifetime().bind_platform_generation(boot_epoch, firmware_generation, namespace_generation,
                                                             platform_topology_generation);
}

na_status_t resource_authority::validate_pci_root_features(u64 owner, u64 pci_config_scope_id,
                                                           const pci::root_feature_evidence &evidence)
{
    if (snapshot_ == nullptr || ledger_ == nullptr || owner == physical_resource::ledger::kernel_owner() ||
        pci_config_scope_id == 0 || !valid_identity(evidence.boot_epoch, evidence.firmware_generation))
        return NA_STATUS_ACCESS_DENIED;

    pci_config_request request{};
    request.scope_id = pci_config_scope_id;
    request.root_id = evidence.root_id;
    request.segment = evidence.segment;
    request.root_start_bus = evidence.start_bus;
    request.root_end_bus = evidence.end_bus;
    pci_config_scope scope{};
    if (!find_pci_config_scope(request, owner, scope))
        return NA_STATUS_ACCESS_DENIED;
    if (scope.boot_epoch != snapshot_->boot_epoch || scope.firmware_generation != snapshot_->firmware_generation ||
        scope.aml_namespace_generation != evidence.aml_namespace_generation ||
        scope.pci_fabric_generation != snapshot_->pci_fabric_generation)
        return NA_STATUS_ACCESS_DENIED;

    const auto expected = static_cast<const pci::root_feature_scope &>(scope);
    const pci::root_feature_policy policy{pci::implemented_pci_osc_support_mask, pci::implemented_pci_osc_control_mask};
    const auto validation = pci::validate_root_feature_evidence(evidence, expected, policy);
    if (validation == pci::root_feature_error::no_control_granted)
        return NA_STATUS_OK;
    if (validation != pci::root_feature_error::none)
        return validation == pci::root_feature_error::unsupported_mask ? NA_STATUS_NOT_SUPPORTED
                                                                       : NA_STATUS_INVALID_ARGUMENT;

    // Evidence for a positive control grant is valid, but the kernel has no
    // native-control operation that can consume it yet. Keep the boundary
    // fail-closed without minting a dormant authority capability.
    return NA_STATUS_NOT_SUPPORTED;
}

na_status_t
resource_authority::authorize_pci_root_resources(u64 owner, u64 pci_config_scope_id,
                                                 const pci::root_resource_evidence &evidence,
                                                 handle_t<pci::root_resource_attestation_capability> &result)
{
    result.reset();
    if (snapshot_ == nullptr || ledger_ == nullptr || owner == physical_resource::ledger::kernel_owner() ||
        pci_config_scope_id == 0 || !valid_identity(evidence.boot_epoch, evidence.firmware_generation))
        return NA_STATUS_ACCESS_DENIED;

    pci_config_request request{};
    request.scope_id = pci_config_scope_id;
    request.root_id = evidence.root_id;
    request.segment = evidence.segment;
    request.root_start_bus = evidence.start_bus;
    request.root_end_bus = evidence.end_bus;
    pci_config_scope scope{};
    if (!find_pci_config_scope(request, owner, scope))
        return NA_STATUS_ACCESS_DENIED;
    if (scope.boot_epoch != snapshot_->boot_epoch || scope.firmware_generation != snapshot_->firmware_generation ||
        scope.aml_namespace_generation != evidence.aml_namespace_generation ||
        scope.pci_fabric_generation != snapshot_->pci_fabric_generation)
        return NA_STATUS_ACCESS_DENIED;

    pci::root_resource_attestation authority{};
    const auto validation =
        pci::validate_root_resource_evidence(evidence, static_cast<const pci::root_feature_scope &>(scope), authority);
    if (validation == pci::root_resource_error::missing_memory_window)
        return NA_STATUS_NOT_SUPPORTED;
    if (validation != pci::root_resource_error::none)
        return NA_STATUS_INVALID_ARGUMENT;

    result = handle_t<pci::root_resource_attestation_capability>::make(owner, authority);
    return result ? NA_STATUS_OK : NA_STATUS_RESOURCE_EXHAUSTED;
}

void resource_authority::revoke_pci_config_scopes(u64 owner)
{
    if (owner == physical_resource::ledger::kernel_owner())
        return;
    uctx::LockGuard_t<lock::mutex_t> guard(pci_config_scope_lock_);
    for (u32 index = 0; index < pci_config_scope_count_;)
    {
        if (pci_config_scopes_[index].owner != owner)
        {
            ++index;
            continue;
        }
        for (u32 move = index + 1; move < pci_config_scope_count_; ++move)
            pci_config_scopes_[move - 1] = pci_config_scopes_[move];
        --pci_config_scope_count_;
    }
}

bool resource_authority::find_pci_config_scope(const pci_config_request &requested, u64 owner,
                                               pci_config_scope &result) const
{
    if (requested.scope_id == 0 || requested.root_id == 0 ||
        requested.path_length > pci_config_request::max_path_length)
        return false;
    uctx::LockGuard_t<lock::mutex_t> guard(pci_config_scope_lock_);
    for (u32 index = 0; index < pci_config_scope_count_; ++index)
    {
        const auto &scope = pci_config_scopes_[index];
        if (scope.id == requested.scope_id && scope.owner == owner && scope.root_id == requested.root_id &&
            scope.aml_namespace_generation != 0 && scope.segment == requested.segment &&
            scope.start_bus == requested.root_start_bus && scope.end_bus == requested.root_end_bus)
        {
            result = scope;
            return true;
        }
    }
    return false;
}

bool resource_authority::resolve_pci_config_function(const pci_config_request &requested, u64 owner,
                                                     u64 namespace_generation, pci_config_scope &scope,
                                                     const arch::acpi_boot::mcfg_region *&region,
                                                     pci::ecam_scope &function_page, u32 &bus, u32 &device,
                                                     u32 &function) const
{
    region = nullptr;
    if (namespace_generation == 0 || !find_pci_config_scope(requested, owner, scope) ||
        scope.aml_namespace_generation != namespace_generation || snapshot_ == nullptr || ledger_ == nullptr)
        return false;

    physical_resource::lease mcfg_lease{};
    if (!ledger_->lookup(scope.mcfg_lease_id, mcfg_lease) ||
        mcfg_lease.resource_kind != physical_resource::kind::pci_ecam ||
        mcfg_lease.lifecycle != physical_resource::state::leased ||
        mcfg_lease.owner != physical_resource::ledger::kernel_owner() ||
        mcfg_lease.generation != snapshot_->firmware_generation)
        return false;

    for (const auto &candidate : snapshot_->mcfg_regions)
    {
        if (candidate.resource_lease_id != scope.mcfg_lease_id || candidate.segment != scope.segment)
            continue;
        if (region != nullptr)
            return false;
        region = &candidate;
    }
    if (region == nullptr)
        return false;

    if (!pci::resolve_ecam_function_path(*region, scope.start_bus, scope.end_bus, requested.device_function_path.data(),
                                         requested.path_length, read_ecam_dword, nullptr, function_page))
        return false;
    bus = function_page.start_bus;
    if (requested.path_length == 0)
    {
        device = 0;
        function = 0;
    }
    else
    {
        const u8 address = requested.device_function_path[requested.path_length - 1];
        device = address & 0x1fu;
        function = address >> 5;
    }
    return true;
}

bool resource_authority::lookup_live(const grant &requested, u64 owner, physical_resource::lease &lease) const
{
    if (ledger_ == nullptr || requested.lease_id == 0 || !ledger_->lookup(requested.lease_id, lease))
        return false;
    if (lease.lifecycle != physical_resource::state::leased && lease.lifecycle != physical_resource::state::mapped)
        return false;
    if (lease.generation != requested.resource_generation || lease.owner != owner ||
        lease.namespace_generation != requested.namespace_generation ||
        lease.device_binding != requested.parent_device_id)
        return false;
    return true;
}

bool resource_authority::authorize(region_space space, u32 mode, u64 boot_epoch, u64 firmware_generation,
                                   u64 namespace_generation, u64 parent_device_id,
                                   const pci_config_request &pci_request, u64 base, u64 length, u64 owner,
                                   task::thread_t *execution_thread, u64 execution_generation, grant &result)
{
    result = {};
    if (!valid_identity(boot_epoch, firmware_generation) || namespace_generation == 0 || ledger_ == nullptr ||
        !is_bound_execution_thread(owner, execution_thread) || length == 0 || add_overflows(base, length) ||
        static_cast<u32>(space) > static_cast<u32>(region_space::pci_config) ||
        !authorization_mode_allowed(space, mode))
        return false;
    if (space == region_space::pci_config)
    {
        if (!pci::valid_read_only_pci_config_region(mode, base, length))
            return false;
        pci_config_scope scope{};
        const arch::acpi_boot::mcfg_region *region = nullptr;
        pci::ecam_scope function_page{};
        u32 bus = 0;
        u32 device = 0;
        u32 function = 0;
        if (!resolve_pci_config_function(pci_request, owner, namespace_generation, scope, region, function_page, bus,
                                         device, function) ||
            function_page.base > ~u64(0) - base || function_page.base + base > ~u64(0) - length)
            return false;

        physical_resource::lease lease{};
        const u64 logical_base = function_page.base + base;
        if (!ledger_->reserve(scope.mcfg_lease_id, logical_base, length, function_page.base,
                              function_page.base + page_size, physical_resource::kind::pci_ecam,
                              physical_resource::cache::uncacheable, physical_resource::access::read, owner,
                              physical_resource::sharing::shared_read_only, firmware_generation, lease))
            return false;
        if (!ledger_->bind_namespace(lease.id, owner, namespace_generation) ||
            !ledger_->bind_device(lease.id, owner, parent_device_id) ||
            !ledger_->transition(lease.id, physical_resource::state::reserved, physical_resource::state::leased, owner))
        {
            (void)ledger_->revoke(lease.id, owner);
            return false;
        }

        result.lease_id = lease.id;
        result.resource_generation = lease.generation;
        result.namespace_generation = namespace_generation;
        result.parent_device_id = parent_device_id;
        result.logical_offset = base;
        result.mapping_bytes = page_size;
        result.pci_config_scope_id = scope.id;
        result.pci_segment = scope.segment;
        result.pci_bus = bus;
        result.pci_device = device;
        result.pci_function = function;
        result.space = region_space::pci_config;
        return true;
    }
    if (space == region_space::system_io && execution_thread == nullptr)
        return false;

    const u64 end = base + length;
    u64 page_base = base & ~(page_size - 1);
    if (end > ~u64(0) - (page_size - 1))
        return false;
    const u64 page_end = (end + page_size - 1) & ~(page_size - 1);
    const u64 page_bytes = page_end - page_base;
    const auto access_mode = mode == 0 ? physical_resource::access::read : physical_resource::access::read_write;

    physical_resource::kind direct_kind = physical_resource::kind::device_mmio;
    if (space == region_space::system_io)
    {
        if (end > 0x1'0000 || execution_thread == nullptr)
            return false;
        direct_kind = physical_resource::kind::io_port;
    }
    else
    {
        // AML may only name device/firmware-reserved bytes. The complete
        // page envelope is required for a direct mapping; table snapshots
        // are never generic region targets even when firmware labels their
        // backing range as device memory.
        if (!arch::ACPI::is_device_range(base, length) || table_range_contains(*snapshot_, base, length))
            return false;
    }

    physical_resource::lease lease{};
    if (space != region_space::system_io && (!arch::ACPI::is_device_range(page_base, page_bytes) ||
                                             table_range_contains(*snapshot_, page_base, page_bytes)))
        return false;
    if (space == region_space::system_io &&
        !task::direct_io_lease_generation_is_current(execution_thread, execution_generation))
        return false;
    const auto direct_access = space == region_space::system_io ? physical_resource::access::access : access_mode;
    if (resource_root_ == nullptr || ledger_ == nullptr ||
        !ledger_->reserve(*resource_root_, base, length, page_base, page_end, direct_kind,
                          physical_resource::cache::uncacheable, direct_access, owner,
                          physical_resource::sharing::exclusive, firmware_generation, lease))
        return false;
    if (!ledger_->bind_namespace(lease.id, owner, namespace_generation) ||
        !ledger_->bind_device(lease.id, owner, parent_device_id) ||
        !ledger_->transition(lease.id, physical_resource::state::reserved, physical_resource::state::leased, owner))
    {
        (void)ledger_->revoke(lease.id, owner);
        return false;
    }
    if (space == region_space::system_io && !ledger_->bind_thread(lease.id, owner, execution_thread->tid))
    {
        (void)ledger_->revoke(lease.id, owner);
        return false;
    }
    if (space == region_space::system_io &&
        !task::set_direct_io_lease(execution_thread, execution_generation, static_cast<u16>(base),
                                   static_cast<u32>(length)))
    {
        (void)task::release_direct_io_lease(execution_thread, execution_generation, static_cast<u16>(base),
                                            static_cast<u32>(length));
        (void)ledger_->revoke(lease.id, owner);
        return false;
    }

    result.lease_id = lease.id;
    result.resource_generation = lease.generation;
    result.namespace_generation = namespace_generation;
    result.parent_device_id = parent_device_id;
    result.logical_offset = base - page_base;
    result.mapping_bytes = space == region_space::system_memory ? page_bytes : 0;
    result.execution_thread_id = execution_thread == nullptr ? 0 : execution_thread->tid;
    result.space = space;
    return true;
}

bool resource_authority::map(const grant &requested, u64 boot_epoch, u64 firmware_generation, u64 namespace_generation,
                             u64 parent_device_id, const pci_config_request &pci_request, u64 base, u64 length,
                             u32 mode, handle_t<naos::data_plane::memory_object> &object, u64 owner,
                             task::thread_t *execution_thread, grant &result)
{
    object = {};
    result = {};
    if (!valid_identity(boot_epoch, firmware_generation) || namespace_generation == 0 ||
        requested.namespace_generation != namespace_generation || !is_read_only_mode(mode) || ledger_ == nullptr ||
        !is_bound_execution_thread(owner, execution_thread))
        return false;
    physical_resource::lease lease{};
    if (!lookup_live(requested, owner, lease) || lease.length != length)
        return false;

    u64 physical_page_base = lease.page_base;
    u64 object_bytes = lease.page_end - lease.page_base;
    u64 object_logical_offset = base - lease.page_base;
    if (requested.space == region_space::pci_config)
    {
        pci_config_scope scope{};
        const arch::acpi_boot::mcfg_region *region = nullptr;
        pci::ecam_scope function_page{};
        u32 bus = 0;
        u32 device = 0;
        u32 function = 0;
        if (!pci::valid_read_only_pci_config_region(mode, base, length) ||
            !resolve_pci_config_function(pci_request, owner, namespace_generation, scope, region, function_page, bus,
                                         device, function) ||
            scope.id != requested.pci_config_scope_id || scope.segment != requested.pci_segment ||
            function_page.base > ~u64(0) - base || function_page.base + base != lease.base ||
            lease.resource_kind != physical_resource::kind::pci_ecam || lease.mode != physical_resource::access::read ||
            lease.sharing_policy != physical_resource::sharing::shared_read_only)
            return false;
        physical_page_base = function_page.base;
        object_bytes = page_size;
        object_logical_offset = base;
    }
    else if (requested.space != region_space::system_memory ||
             lease.resource_kind != physical_resource::kind::device_mmio || lease.base != base)
    {
        return false;
    }

    if (!ledger_->map(lease.id, lease.base, lease.length, lease.resource_kind, lease.cache_type, lease.mode,
                      lease.generation, owner))
        return false;
    const u32 flags = mode == 0 ? NA_MEMORY_FLAG_READ_ONLY : 0;
    object = handle_t<naos::data_plane::memory_object>::make(
        phy_addr_t::from(physical_page_base), memory::pa2va<byte *>(phy_addr_t::from(physical_page_base)), object_bytes,
        flags, release_region_lease, lease.id);
    if (!object)
    {
        (void)ledger_->unmap_kernel(lease.id);
        (void)ledger_->revoke_kernel(lease.id);
        return false;
    }
    result = requested;
    result.mapping_bytes = object_bytes;
    result.logical_offset = object_logical_offset;
    result.execution_thread_id = requested.execution_thread_id;
    if (requested.space == region_space::pci_config)
    {
        result.pci_config_scope_id = requested.pci_config_scope_id;
        result.pci_segment = requested.pci_segment;
        // The user request carries the namespace path, not a caller-selected BDF.
        // Resolve it again above and publish only the kernel-derived selector.
        pci_config_scope scope{};
        const arch::acpi_boot::mcfg_region *region = nullptr;
        pci::ecam_scope function_page{};
        u32 bus = 0;
        u32 device = 0;
        u32 function = 0;
        if (!resolve_pci_config_function(pci_request, owner, namespace_generation, scope, region, function_page, bus,
                                         device, function))
        {
            object = {};
            return false;
        }
        result.pci_segment = scope.segment;
        result.pci_bus = bus;
        result.pci_device = device;
        result.pci_function = function;
    }
    return true;
}

bool resource_authority::release(u64 boot_epoch, u64 firmware_generation, u64 namespace_generation, u64 owner,
                                 task::thread_t *execution_thread, u64 execution_generation, u64 lease_id)
{
    if (!valid_identity(boot_epoch, firmware_generation) || namespace_generation == 0 || ledger_ == nullptr ||
        !is_bound_execution_thread(owner, execution_thread))
        return false;
    physical_resource::lease lease{};
    if (!ledger_->lookup(lease_id, lease))
        return true;
    if (lease.generation != firmware_generation || lease.owner != owner ||
        lease.namespace_generation != namespace_generation)
        return false;
    if (lease.resource_kind == physical_resource::kind::io_port &&
        (execution_thread == nullptr || lease.thread_binding != execution_thread->tid))
        return false;
    if (lease.resource_kind == physical_resource::kind::io_port && lease.mode == physical_resource::access::access)
    {
        if (!task::release_direct_io_lease(execution_thread, execution_generation, static_cast<u16>(lease.base),
                                           static_cast<u32>(lease.length)))
            return false;
    }
    return ledger_->revoke(lease_id, owner);
}

} // namespace acpi
