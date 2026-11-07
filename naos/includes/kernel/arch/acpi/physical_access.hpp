#pragma once

#include "kernel/kernel.hpp"

namespace arch::acpi_boot
{

/// Treat malformed or wrapping extents as conflicts so they cannot widen
/// firmware access when boot metadata is inconsistent.
constexpr bool overlaps_boot_owned_memory(const kernel_start_args &args, u64 address, u64 length) noexcept
{
    constexpr u64 max_address = ~u64{0};
    if (length == 0 || address > max_address - length || args.boot_module_count > max_boot_modules)
        return true;

    const u64 end = address + length;
    const auto conflicts = [=](u64 base, u64 extent) constexpr {
        if (extent == 0)
            return false;
        if (base > max_address - extent)
            return true;
        return address < base + extent && base < end;
    };

    if (conflicts(args.kernel_base, args.kernel_size) || conflicts(args.data_base, args.data_size))
        return true;
    for (u64 index = 0; index < args.boot_module_count; index++)
    {
        const auto &module = args.boot_modules[index];
        if (conflicts(module.start, module.size))
            return true;
    }
    return false;
}

/// Admit a complete physical range only when one recognized firmware-reserved
/// memory-map entry contains it and it does not overlap boot-owned memory.
inline bool is_device_range_valid(const kernel_start_args &args, const kernel_memory_map_item *map, u64 map_count,
                                  u64 address, u64 length) noexcept
{
    constexpr u64 max_address = ~u64{0};
    if (map == nullptr || map_count == 0 || length == 0 || address > max_address - length ||
        overlaps_boot_owned_memory(args, address, length))
        return false;

    const u64 end = address + length;
    bool found_containing_entry = false;
    for (u64 index = 0; index < map_count; index++)
    {
        const auto &item = map[index];
        if (item.len == 0)
            continue;
        if (item.addr > max_address - item.len)
            return false;
        const u64 item_end = item.addr + item.len;
        if (address >= item_end || item.addr >= end)
            continue;
        if (address < item.addr || end > item_end || found_containing_entry)
            return false;

        switch (item.map_type)
        {
            case map_type_t::reserved:
            case map_type_t::acpi:
            case map_type_t::nvs:
                found_containing_entry = true;
                break;
            case map_type_t::available:
            case map_type_t::badram:
            default:
                return false;
        }
    }
    return found_containing_entry;
}

/// Accept a DMAR-reported register aperture when it is outside allocator RAM
/// and all kernel/boot-owned ranges. Firmware memory maps commonly omit
/// chipset MMIO holes entirely; unlike a user-facing MMIO lease, this narrow
/// check only admits the register aperture into the internal IOMMU inventory.
inline bool is_iommu_register_range_valid(const kernel_start_args &args, const kernel_memory_map_item *map,
                                          u64 map_count, u64 address, u64 length) noexcept
{
    constexpr u64 max_address = ~u64{0};
    if (map == nullptr || map_count == 0 || length == 0 || address > max_address - length ||
        overlaps_boot_owned_memory(args, address, length))
        return false;

    const u64 end = address + length;
    for (u64 index = 0; index < map_count; index++)
    {
        const auto &item = map[index];
        if (item.len == 0)
            continue;
        if (item.addr > max_address - item.len)
            return false;
        const u64 item_end = item.addr + item.len;
        if (address >= item_end || item.addr >= end)
            continue;

        switch (item.map_type)
        {
            case map_type_t::reserved:
            case map_type_t::acpi:
            case map_type_t::nvs:
                break;
            case map_type_t::available:
            case map_type_t::badram:
            default:
                return false;
        }
    }
    return true;
}

} // namespace arch::acpi_boot
