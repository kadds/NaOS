#include "kernel/pci/dma_quiesce.hpp"

namespace pci::dma_quiesce
{
namespace
{
constexpr u16 bus_master_enable = 1u << 2;
constexpr u8 multifunction_header = 1u << 7;
constexpr u32 device_count = 32;
constexpr u32 function_count = 8;

bool function_present(u16 vendor) { return vendor != 0 && vendor != 0xffff; }

error disable_bus_master(const config_ops &operations, u8 bus, u8 device, u8 function, result &scan_result)
{
    u16 command = 0;
    if (!operations.read_command(operations.context, bus, device, function, command))
        return error::command_read_failed;
    if ((command & bus_master_enable) == 0)
        return error::none;

    const auto disable = set_bus_master(operations, bus, device, function, false);
    if (disable != error::none)
        return disable;

    scan_result.bus_masters_disabled++;
    return error::none;
}

error write_bus_master(const config_ops &operations, u8 bus, u8 device, u8 function, bool enabled)
{
    if (device >= device_count || function >= function_count)
        return error::invalid_device_id;
    if (operations.read_command == nullptr || operations.write_command == nullptr)
        return error::invalid_operations;

    u16 command = 0;
    if (!operations.read_command(operations.context, bus, device, function, command))
        return error::command_read_failed;
    const bool currently_enabled = (command & bus_master_enable) != 0;
    if (currently_enabled == enabled)
        return error::none;

    const u16 updated_command =
        enabled ? static_cast<u16>(command | bus_master_enable) : static_cast<u16>(command & ~bus_master_enable);
    if (!operations.write_command(operations.context, bus, device, function, updated_command))
        return error::command_write_failed;

    u16 verified_command = 0;
    if (!operations.read_command(operations.context, bus, device, function, verified_command))
        return error::command_read_failed;
    const bool verified_enabled = (verified_command & bus_master_enable) != 0;
    if (verified_enabled != enabled)
        return enabled ? error::bus_master_still_disabled : error::bus_master_still_enabled;
    return error::none;
}

error translate_authority_denial(dma_authority::bus_master_denial denial)
{
    switch (denial)
    {
        case dma_authority::bus_master_denial::none:
            return error::none;
        case dma_authority::bus_master_denial::no_domain:
            return error::domain_required;
        case dma_authority::bus_master_denial::domain_not_ready:
            return error::domain_not_ready;
        case dma_authority::bus_master_denial::no_iommu:
            return error::no_iommu_policy;
        case dma_authority::bus_master_denial::state_uncertain:
            return error::domain_state_uncertain;
    }
    return error::domain_state_uncertain;
}
} // namespace

error set_bus_master(const config_ops &operations, u8 bus, u8 device, u8 function, bool enabled) noexcept
{
    if (device >= device_count || function >= function_count)
        return error::invalid_device_id;
    if (enabled)
        return error::domain_required;
    return write_bus_master(operations, bus, device, function, false);
}

error set_bus_master(const config_ops &operations, const bus_master_enable_authority &authority) noexcept
{
    if (operations.segment != authority.segment())
        return error::invalid_device_id;
    if (authority.device() >= device_count || authority.function() >= function_count)
        return error::invalid_device_id;
    const auto denial = translate_authority_denial(dma_authority::bus_master_enable_denial(authority.state()));
    if (denial != error::none)
        return denial;
    return write_bus_master(operations, authority.bus(), authority.device(), authority.function(), true);
}

error quiesce_region(const ecam_region &region, const config_ops &operations, result &scan_result) noexcept
{
    scan_result = {};
    if (region.start_bus > region.end_bus)
        return error::invalid_region;
    const u64 bus_count = static_cast<u64>(region.end_bus) - region.start_bus + 1;
    if (region.base == 0 || region.resource_lease_id == 0 || (region.base & 0xfffu) != 0 ||
        bus_count > (~u64(0) >> 20) || region.base > ~u64(0) - (bus_count << 20))
        return error::invalid_region;
    if (operations.read_identity == nullptr || operations.read_command == nullptr ||
        operations.write_command == nullptr)
        return error::invalid_operations;

    for (u32 bus = region.start_bus; bus <= region.end_bus; ++bus)
    {
        for (u32 device = 0; device < device_count; ++device)
        {
            u16 vendor = 0xffff;
            u8 header_type = 0;
            if (!operations.read_identity(operations.context, static_cast<u8>(bus), static_cast<u8>(device), 0, vendor,
                                          header_type))
                return error::config_read_failed;
            if (!function_present(vendor))
                continue;

            const u32 functions = (header_type & multifunction_header) != 0 ? function_count : 1;
            for (u32 function = 0; function < functions; ++function)
            {
                if (function != 0)
                {
                    vendor = 0xffff;
                    header_type = 0;
                    if (!operations.read_identity(operations.context, static_cast<u8>(bus), static_cast<u8>(device),
                                                  static_cast<u8>(function), vendor, header_type))
                        return error::config_read_failed;
                    if (!function_present(vendor))
                        continue;
                }

                scan_result.functions_seen++;
                const auto failure = disable_bus_master(operations, static_cast<u8>(bus), static_cast<u8>(device),
                                                        static_cast<u8>(function), scan_result);
                if (failure != error::none)
                    return failure;
            }
        }
    }
    return error::none;
}
} // namespace pci::dma_quiesce
