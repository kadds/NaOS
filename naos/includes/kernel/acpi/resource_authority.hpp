#pragma once

#include "kernel/acpi/execution_thread_binding.hpp"
#include "kernel/acpi/global_lock.hpp"
#include "kernel/arch/acpi/boot_tables.hpp"
#include "kernel/kobject.hpp"
#include "kernel/mutex.hpp"
#include "kernel/pci/discovery_fabric.hpp"
#include "kernel/pci/root_resource_authority.hpp"
#include "kernel/physical_resource.hpp"
#include <array>

namespace naos::data_plane
{
class memory_object;
}

namespace task
{
struct process_t;
struct thread_t;
} // namespace task

namespace acpi
{

class resource_authority final : public kobject
{
  public:
    enum class region_space : u32
    {
        system_memory = 0,
        system_io = 1,
        pci_config = 2,
    };

    struct grant
    {
        u64 lease_id = 0;
        u64 resource_generation = 0;
        u64 namespace_generation = 0;
        u64 logical_offset = 0;
        u64 mapping_bytes = 0;
        u64 parent_device_id = 0;
        u64 execution_thread_id = 0;
        u64 pci_config_scope_id = 0;
        u32 pci_segment = 0;
        u32 pci_bus = 0;
        u32 pci_device = 0;
        u32 pci_function = 0;
        region_space space = region_space::system_memory;
    };

    struct pci_config_request
    {
        static constexpr u32 max_path_length = 16;

        u64 scope_id = 0;
        u64 root_id = 0;
        u32 segment = 0;
        u32 root_start_bus = 0;
        u32 root_end_bus = 0;
        u32 path_length = 0;
        std::array<u8, max_path_length> device_function_path{};
    };

    static constexpr type_e type_of() { return type_e::acpi_resource_authority; }
    static constexpr bool is_read_only_mode(u32 mode) noexcept { return mode == 0; }
    static constexpr bool authorization_mode_allowed(region_space space, u32 mode) noexcept
    {
        return mode <= 1 && (space == region_space::system_io || is_read_only_mode(mode));
    }

    resource_authority(const arch::acpi_boot::snapshot *snapshot, physical_resource::root *resource_root)
        : kobject(type_of())
        , snapshot_(snapshot)
        , resource_root_(resource_root)
        , ledger_(resource_root == nullptr ? nullptr : resource_root->resources())
    {
    }

    const arch::acpi_boot::snapshot *snapshot() const { return snapshot_; }
    physical_resource::ledger *ledger() const { return ledger_; }

    bool bind_pci_config_root(u64 boot_epoch, u64 firmware_generation, u64 namespace_generation,
                              u64 platform_topology_generation, u64 compatibility_profile_hash, u64 owner, u64 root_id,
                              u32 segment, u32 start_bus, u32 end_bus, u64 &scope_id);
    bool bind_platform_generation(u64 boot_epoch, u64 firmware_generation, u64 namespace_generation,
                                  u64 platform_topology_generation, u64 owner);
    na_status_t validate_pci_root_features(u64 owner, u64 pci_config_scope_id,
                                           const pci::root_feature_evidence &evidence);
    na_status_t authorize_pci_root_resources(u64 owner, u64 pci_config_scope_id,
                                             const pci::root_resource_evidence &evidence,
                                             handle_t<pci::root_resource_attestation_capability> &result);
    bool bind_execution_thread(u64 boot_epoch, u64 firmware_generation, u64 owner, task::thread_t *registrar_thread,
                               u64 execution_thread_id);
    global_lock_operation_result acquire_global_lock(u64 boot_epoch, u64 firmware_generation, u64 owner,
                                                     task::thread_t *execution_thread, u64 timeout_us);
    global_lock_operation_result release_global_lock(u64 boot_epoch, u64 firmware_generation, u64 owner,
                                                     task::thread_t *execution_thread);
    void unbind_execution_thread(u64 owner, u64 thread_id);
    bool unbind_execution_owner(u64 owner);
    bool is_bound_execution_thread(u64 owner, const task::thread_t *execution_thread) const;
    void revoke_pci_config_scopes(u64 owner);

    bool authorize(region_space space, u32 mode, u64 boot_epoch, u64 firmware_generation, u64 namespace_generation,
                   u64 parent_device_id, const pci_config_request &pci_request, u64 base, u64 length, u64 owner,
                   task::thread_t *execution_thread, u64 execution_generation, grant &result);
    bool map(const grant &requested, u64 boot_epoch, u64 firmware_generation, u64 namespace_generation,
             u64 parent_device_id, const pci_config_request &pci_request, u64 base, u64 length, u32 mode,
             handle_t<naos::data_plane::memory_object> &object, u64 owner, task::thread_t *execution_thread,
             grant &result);
    bool release(u64 boot_epoch, u64 firmware_generation, u64 namespace_generation, u64 owner,
                 task::thread_t *execution_thread, u64 execution_generation, u64 lease_id);

    bool capability_is_unique() const override { return false; }
    na_signal_t capability_signals() const override { return NA_SIGNAL_WRITABLE; }

  private:
    bool valid_identity(u64 boot_epoch, u64 firmware_generation) const;
    bool lookup_live(const grant &requested, u64 owner, physical_resource::lease &lease) const;

    struct pci_config_scope : pci::root_feature_scope
    {
        u64 id = 0;
        u64 owner = 0;
        u64 mcfg_lease_id = 0;
    };

    bool find_pci_config_scope(const pci_config_request &requested, u64 owner, pci_config_scope &result) const;
    bool resolve_pci_config_function(const pci_config_request &requested, u64 owner, u64 namespace_generation,
                                     pci_config_scope &scope, const arch::acpi_boot::mcfg_region *&region,
                                     pci::ecam_scope &function_page, u32 &bus, u32 &device, u32 &function) const;

    const arch::acpi_boot::snapshot *snapshot_;
    physical_resource::root *resource_root_;
    physical_resource::ledger *ledger_;
    static constexpr u32 max_pci_config_scopes = 64;
    std::array<pci_config_scope, max_pci_config_scopes> pci_config_scopes_{};
    u32 pci_config_scope_count_ = 0;
    u64 next_pci_config_scope_id_ = 1;
    mutable lock::mutex_t pci_config_scope_lock_;
    detail::executor_thread_binding execution_thread_binding_{};
    mutable lock::mutex_t execution_thread_lock_;
};

} // namespace acpi
