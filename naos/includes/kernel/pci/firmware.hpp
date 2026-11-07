#pragma once

#include "kernel/types.hpp"

namespace pci
{

/// Kernel-owned ECAM allocation data after firmware parsing. This deliberately
/// contains only the PCI configuration aperture and its resource lease, not an
/// ACPI table snapshot.
struct ecam_region
{
    u64 base = 0;
    u16 segment = 0;
    u8 start_bus = 0;
    u8 end_bus = 0;
    u64 resource_lease_id = 0;
};

/// Immutable, non-owning PCI view over the firmware-derived ECAM allocations.
/// The owner of the backing array must outlive every PCI capability minted from
/// this inventory.
struct firmware_inventory
{
    u64 boot_epoch = 0;
    u64 firmware_generation = 0;
    u64 pci_fabric_generation = 0;
    const ecam_region *mcfg_regions = nullptr;
    u64 mcfg_region_count = 0;

    const ecam_region *region(u64 index) const noexcept
    {
        return mcfg_regions != nullptr && index < mcfg_region_count ? &mcfg_regions[index] : nullptr;
    }
};

/// Resolve one requester bus to exactly one ECAM allocation. Ambiguous
/// overlapping allocations fail closed.
inline bool find_ecam_region_for_requester(const ecam_region *regions, u64 region_count, u16 segment, u8 bus,
                                           const ecam_region *&result) noexcept
{
    result = nullptr;
    if (region_count != 0 && regions == nullptr)
        return false;

    for (u64 index = 0; index < region_count; ++index)
    {
        const auto &region = regions[index];
        if (region.segment != segment || bus < region.start_bus || bus > region.end_bus)
            continue;
        if (result != nullptr)
        {
            result = nullptr;
            return false;
        }
        result = &region;
    }
    return result != nullptr;
}

} // namespace pci
