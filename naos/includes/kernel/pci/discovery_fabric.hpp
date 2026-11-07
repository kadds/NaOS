#pragma once

#include "kernel/arch/mm.hpp"
#include "kernel/kobject.hpp"
#include "kernel/pci/driver_grant.hpp"
#include "kernel/pci/firmware.hpp"
#include "kernel/pci/osc_authority.hpp"
#include "kernel/pci/root_resource_authority.hpp"
#include "kernel/physical_resource.hpp"
#include <atomic>

namespace pci
{
struct root_binding_request
{
    u64 region_index = 0;
    u64 platform_root_id = 0;
    u64 boot_epoch = 0;
    u64 firmware_generation = 0;
    u64 aml_namespace_generation = 0;
    u64 platform_topology_generation = 0;
    u64 pci_fabric_generation = 0;
    u32 segment = 0;
    u32 start_bus = 0;
    u32 end_bus = 0;
};

struct ecam_scope
{
    u64 base = 0;
    u64 length = 0;
    u32 start_bus = 0;
    u32 end_bus = 0;
};

struct root_binding
{
    u64 region_index = 0;
    u64 platform_root_id = 0;
    u64 boot_epoch = 0;
    u64 firmware_generation = 0;
    u64 aml_namespace_generation = 0;
    u64 platform_topology_generation = 0;
    u64 pci_fabric_generation = 0;
    u32 segment = 0;
    ecam_scope ecam{};
    root_resource_attestation resources{};
};

struct function_identity
{
    u64 platform_root_id = 0;
    u32 segment = 0;
    u32 bus = 0;
    u32 device = 0;
    u32 function = 0;
    u64 boot_epoch = 0;
    u64 firmware_generation = 0;
    u64 aml_namespace_generation = 0;
    u64 platform_topology_generation = 0;
    u64 pci_fabric_generation = 0;
};

/// Convert a checked MCFG allocation into one bus-window subrange. ECAM is
/// 1 MiB per bus, so the result stays page-aligned and can be delegated as a
/// child physical-resource lease without exposing the rest of the allocation.
inline bool scope_ecam_region(const ecam_region &region, u32 start_bus, u32 end_bus, ecam_scope &result)
{
    constexpr u64 ecam_bus_bytes = 1ULL << 20;
    if (region.base == 0 || (region.base & (memory::page_size - 1)) != 0 || region.start_bus > region.end_bus ||
        start_bus > end_bus || start_bus < region.start_bus || end_bus > region.end_bus)
        return false;

    const u64 region_bus_count = static_cast<u64>(region.end_bus) - region.start_bus + 1;
    const u64 region_bytes = region_bus_count * ecam_bus_bytes;
    if (region.base > ~u64(0) - region_bytes)
        return false;

    const u64 bus_offset = static_cast<u64>(start_bus - region.start_bus) * ecam_bus_bytes;
    const u64 bus_count = static_cast<u64>(end_bus) - start_bus + 1;
    const u64 bytes = bus_count * ecam_bus_bytes;
    if (region.base > ~u64(0) - bus_offset)
        return false;
    const u64 base = region.base + bus_offset;
    if (base > ~u64(0) - bytes)
        return false;

    result = {base, bytes, start_bus, end_bus};
    return true;
}

/// Validate the platform root claim against the kernel-owned boot snapshot
/// and one exact MCFG allocation. The platform generations remain opaque;
/// pcied supplies them only after validating PlatformPciTopology's snapshot.
inline bool make_root_binding(const firmware_inventory &inventory, const ecam_region &region,
                              const root_binding_request &request, const root_resource_attestation &resources,
                              root_binding &result)
{
    if (inventory.boot_epoch == 0 || inventory.firmware_generation == 0 || inventory.pci_fabric_generation == 0 ||
        inventory.region(request.region_index) != &region || request.platform_root_id == 0 ||
        request.aml_namespace_generation == 0 || request.platform_topology_generation == 0 ||
        request.segment != region.segment || request.boot_epoch != inventory.boot_epoch ||
        request.firmware_generation != inventory.firmware_generation ||
        request.pci_fabric_generation != inventory.pci_fabric_generation || region.resource_lease_id == 0)
        return false;
    if (resources.root_id != request.platform_root_id || resources.boot_epoch != request.boot_epoch ||
        resources.firmware_generation != request.firmware_generation ||
        resources.aml_namespace_generation != request.aml_namespace_generation ||
        resources.platform_topology_generation != request.platform_topology_generation ||
        resources.pci_fabric_generation != request.pci_fabric_generation || resources.segment != request.segment ||
        resources.start_bus != request.start_bus || resources.end_bus != request.end_bus ||
        resources.memory_window_count == 0 || resources.memory_window_count > max_root_resource_windows)
        return false;

    ecam_scope ecam{};
    if (!scope_ecam_region(region, request.start_bus, request.end_bus, ecam))
        return false;

    result = {request.region_index,
              request.platform_root_id,
              request.boot_epoch,
              request.firmware_generation,
              request.aml_namespace_generation,
              request.platform_topology_generation,
              request.pci_fabric_generation,
              request.segment,
              ecam,
              resources};
    return true;
}

enum class function_selector_error : u8
{
    none,
    invalid_argument,
    out_of_range,
    outside_root,
};

inline bool valid_read_only_pci_config_region(u32 mode, u64 offset, u64 length)
{
    constexpr u64 config_space_bytes = 4096;
    return mode == 0 && length != 0 && offset < config_space_bytes && length <= config_space_bytes - offset;
}

enum class config_read_error : u8
{
    none,
    invalid_width,
    misaligned,
    out_of_range,
};

inline constexpr config_read_error validate_config_read(u32 offset, u32 width) noexcept
{
    constexpr u32 config_space_bytes = 4096;
    if (width != 1 && width != 2 && width != 4)
        return config_read_error::invalid_width;
    if (offset >= config_space_bytes || width > config_space_bytes - offset)
        return config_read_error::out_of_range;
    if ((offset % width) != 0)
        return config_read_error::misaligned;
    return config_read_error::none;
}

inline function_selector_error validate_function_selector(u32 start_bus, u32 end_bus, u32 bus, u32 device, u32 function)
{
    if (start_bus > end_bus || end_bus > 0xff || device >= 32 || function >= 8)
        return function_selector_error::invalid_argument;
    if (bus > 0xff)
        return function_selector_error::out_of_range;
    if (bus < start_bus || bus > end_bus)
        return function_selector_error::outside_root;
    return function_selector_error::none;
}

/// Resolve one read-only ECAM function page within a previously validated
/// root-bus/MCFG intersection.
inline bool scope_ecam_function_page(const ecam_region &region, u32 root_start_bus, u32 root_end_bus, u32 bus,
                                     u32 device, u32 function, ecam_scope &result)
{
    ecam_scope root_scope{};
    if (region.resource_lease_id == 0 ||
        validate_function_selector(root_start_bus, root_end_bus, bus, device, function) !=
            function_selector_error::none ||
        !scope_ecam_region(region, root_start_bus, root_end_bus, root_scope))
        return false;

    constexpr u64 ecam_bus_bytes = 1ULL << 20;
    constexpr u64 function_bytes = 1ULL << 12;
    const u64 bus_offset = static_cast<u64>(bus - region.start_bus) * ecam_bus_bytes;
    const u64 device_offset = static_cast<u64>(device) << 15;
    const u64 function_offset = static_cast<u64>(function) << 12;
    if (region.base > ~u64(0) - bus_offset || region.base + bus_offset > ~u64(0) - device_offset ||
        region.base + bus_offset + device_offset > ~u64(0) - function_offset)
        return false;

    const u64 base = region.base + bus_offset + device_offset + function_offset;
    if (base > ~u64(0) - function_bytes || base < root_scope.base ||
        base + function_bytes > root_scope.base + root_scope.length)
        return false;
    result = {base, function_bytes, bus, bus};
    return true;
}

using ecam_dword_reader = bool (*)(void *context, u64 function_page_base, u32 offset, u32 &value);

/// Resolve a root-relative PCI `_ADR` path. Every intermediate function must
/// be a present PCI-to-PCI bridge with a valid secondary/subordinate range;
/// the final result is exactly one 4 KiB function page.
inline bool resolve_ecam_function_path(const ecam_region &region, u32 root_start_bus, u32 root_end_bus,
                                       const u8 *device_function_path, u32 path_length, ecam_dword_reader read_dword,
                                       void *context, ecam_scope &result)
{
    constexpr u32 max_bridge_path = 16;
    if (path_length > max_bridge_path || (path_length != 0 && device_function_path == nullptr))
        return false;
    if (path_length == 0)
        return scope_ecam_function_page(region, root_start_bus, root_end_bus, root_start_bus, 0, 0, result);

    u32 bus = root_start_bus;
    for (u32 index = 0; index < path_length; ++index)
    {
        const u8 address = device_function_path[index];
        const u32 device = address & 0x1fu;
        const u32 function = address >> 5;
        ecam_scope current_function{};
        if (!scope_ecam_function_page(region, root_start_bus, root_end_bus, bus, device, function, current_function))
            return false;
        if (index + 1 == path_length)
        {
            result = current_function;
            return true;
        }
        if (read_dword == nullptr)
            return false;

        u32 identity = 0;
        u32 class_code = 0;
        u32 bus_numbers = 0;
        if (!read_dword(context, current_function.base, 0, identity) || static_cast<u16>(identity) == 0 ||
            static_cast<u16>(identity) == 0xffff || !read_dword(context, current_function.base, 8, class_code) ||
            (class_code >> 24) != 0x06 || ((class_code >> 16) & 0xffu) != 0x04 ||
            !read_dword(context, current_function.base, 0x18, bus_numbers))
            return false;

        const u32 primary_bus = bus_numbers & 0xffu;
        const u32 secondary_bus = (bus_numbers >> 8) & 0xffu;
        const u32 subordinate_bus = (bus_numbers >> 16) & 0xffu;
        if (primary_bus != bus || secondary_bus <= bus || secondary_bus > subordinate_bus ||
            secondary_bus < root_start_bus || subordinate_bus > root_end_bus)
            return false;
        bus = secondary_bus;
    }
    return false;
}

class discovery_fabric final : public kobject
{
  public:
    static constexpr type_e type_of() { return type_e::pci_discovery_fabric; }

    explicit discovery_fabric(const firmware_inventory *inventory, physical_resource::ledger *ledger,
                              physical_resource::root *resource_root)
        : kobject(type_of())
        , inventory_(inventory)
        , ledger_(ledger)
        , resource_root_(resource_root)
    {
    }

    const firmware_inventory *inventory() const { return inventory_; }
    physical_resource::ledger *ledger() const { return ledger_; }
    physical_resource::root *resource_root() const { return resource_root_; }
    bool capability_is_unique() const override { return false; }
    na_signal_t capability_signals() const override { return NA_SIGNAL_WRITABLE; }

  private:
    const firmware_inventory *inventory_;
    physical_resource::ledger *ledger_;
    physical_resource::root *resource_root_;
};

class fabric_admin final : public kobject
{
  public:
    static constexpr type_e type_of() { return type_e::pci_fabric_admin; }

    fabric_admin(const firmware_inventory *inventory, physical_resource::ledger *ledger,
                 physical_resource::root *resource_root, process_id owner, root_binding binding)
        : kobject(type_of())
        , inventory_(inventory)
        , ledger_(ledger)
        , resource_root_(resource_root)
        , owner_(owner)
        , binding_(binding)
    {
    }

    const firmware_inventory *inventory() const { return inventory_; }
    physical_resource::ledger *ledger() const { return ledger_; }
    physical_resource::root *resource_root() const { return resource_root_; }
    process_id owner() const { return owner_; }
    const root_binding &binding() const { return binding_; }
    bool capability_is_unique() const override { return true; }
    na_signal_t capability_signals() const override { return NA_SIGNAL_WRITABLE; }

  private:
    const firmware_inventory *inventory_;
    physical_resource::ledger *ledger_;
    physical_resource::root *resource_root_;
    process_id owner_;
    root_binding binding_;
};

class function final : public kobject
{
  public:
    static constexpr type_e type_of() { return type_e::pci_function; }

    function(process_id owner, const firmware_inventory *inventory, physical_resource::ledger *ledger,
             physical_resource::root *resource_root, root_binding binding, function_identity identity,
             bool driver_grant = false, driver_function_profile driver_profile = {})
        : kobject(type_of())
        , owner_(owner)
        , inventory_(inventory)
        , ledger_(ledger)
        , resource_root_(resource_root)
        , binding_(binding)
        , identity_(identity)
        , driver_grant_(driver_grant)
        , driver_profile_(driver_profile)
    {
    }

    process_id owner() const { return owner_; }
    const firmware_inventory *inventory() const { return inventory_; }
    physical_resource::ledger *ledger() const { return ledger_; }
    physical_resource::root *resource_root() const { return resource_root_; }
    const root_binding &binding() const { return binding_; }
    const function_identity &identity() const { return identity_; }
    bool is_driver_grant() const { return driver_grant_; }
    const driver_function_profile &driver_profile() const { return driver_profile_; }
    bool bind_device_resources(const device_resource_attestation &authority)
    {
        u8 expected = 0;
        if (!device_resource_state_.compare_exchange_strong(expected, 1, std::memory_order_acq_rel,
                                                            std::memory_order_acquire))
            return false;
        device_resources_ = authority;
        device_resource_state_.store(2, std::memory_order_release);
        return true;
    }
    bool get_device_resources(device_resource_attestation &authority) const
    {
        if (device_resource_state_.load(std::memory_order_acquire) != 2)
            return false;
        authority = device_resources_;
        return true;
    }
    const device_resource_attestation *device_resources() const
    {
        return device_resource_state_.load(std::memory_order_acquire) == 2 ? &device_resources_ : nullptr;
    }
    bool capability_is_unique() const override { return true; }
    na_signal_t capability_signals() const override { return NA_SIGNAL_WRITABLE; }

  private:
    process_id owner_;
    const firmware_inventory *inventory_;
    physical_resource::ledger *ledger_;
    physical_resource::root *resource_root_;
    root_binding binding_;
    function_identity identity_;
    bool driver_grant_;
    driver_function_profile driver_profile_;
    std::atomic<u8> device_resource_state_{0}; // 0=empty, 1=publishing, 2=immutable authority ready
    device_resource_attestation device_resources_{};
};

} // namespace pci
