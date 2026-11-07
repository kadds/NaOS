#pragma once

#include "kernel/arch/acpi/boot_tables.hpp"
#include "kernel/iommu/firmware_inventory.hpp"

namespace naos_test::iommu_firmware
{
struct snapshot : arch::acpi_boot::snapshot, iommu::intel_vtd::firmware_inventory, iommu::amd_vi::firmware_inventory
{
    explicit snapshot(freelibcxx::Allocator *allocator)
        : arch::acpi_boot::snapshot(allocator)
        , iommu::intel_vtd::firmware_inventory(allocator)
        , iommu::amd_vi::firmware_inventory(allocator)
    {
    }

    using arch::acpi_boot::snapshot::firmware_generation;
};

inline void reset_provider_inventories(snapshot &result, u64 generation)
{
    auto &intel = static_cast<iommu::intel_vtd::firmware_inventory &>(result);
    intel.firmware_generation = generation;
    intel.dmar_units.clear();
    intel.dmar_reserved_regions.clear();
    intel.dmar_device_scopes.clear();
    intel.dmar_device_paths.clear();
    intel.dmar_host_address_width = 0;
    intel.dmar_flags = 0;
    intel.dmar_state = iommu::intel_vtd::dmar_inventory_state::absent;
    intel.dmar_error = arch::acpi_boot::error::none;
    intel.dmar_error_offset = 0;
    intel.dmar_error_address = 0;
    intel.dmar_error_length = 0;

    auto &amd = static_cast<iommu::amd_vi::firmware_inventory &>(result);
    amd.firmware_generation = generation;
    amd.amd_ivhd_records.clear();
    amd.amd_ivhd_device_entries.clear();
    amd.amd_ivmd_records.clear();
    amd.ivrs_info = 0;
    amd.ivrs_state = iommu::amd_vi::ivrs_inventory_state::absent;
    amd.ivrs_error = arch::acpi_boot::error::none;
    amd.ivrs_error_offset = 0;
}

inline bool build_snapshot(const arch::acpi_boot::physical_source &source, u64 rsdp_address, u64 boot_epoch,
                           u64 generation, snapshot &result, arch::acpi_boot::error &failure)
{
    auto &catalog = static_cast<arch::acpi_boot::snapshot &>(result);
    if (!arch::acpi_boot::build_snapshot(source, rsdp_address, boot_epoch, generation, catalog, failure))
        return false;
    reset_provider_inventories(result, generation);

    const arch::acpi_boot::table_record *dmar = nullptr;
    const arch::acpi_boot::table_record *ivrs = nullptr;
    bool duplicate_dmar = false;
    bool duplicate_ivrs = false;
    for (const auto &table : catalog.tables)
    {
        if (table.signature == arch::acpi_boot::signature_value("DMAR"))
        {
            duplicate_dmar = duplicate_dmar || dmar != nullptr;
            dmar = dmar == nullptr ? &table : dmar;
        }
        else if (table.signature == arch::acpi_boot::signature_value("IVRS"))
        {
            duplicate_ivrs = duplicate_ivrs || ivrs != nullptr;
            ivrs = ivrs == nullptr ? &table : ivrs;
        }
    }

    auto &intel = static_cast<iommu::intel_vtd::firmware_inventory &>(result);
    if (duplicate_dmar)
    {
        intel.dmar_state = iommu::intel_vtd::dmar_inventory_state::invalid;
        intel.dmar_error = arch::acpi_boot::error::duplicate_entry;
    }
    else if (dmar != nullptr)
    {
        arch::acpi_boot::error parse_error = arch::acpi_boot::error::none;
        (void)iommu::intel_vtd::parse_firmware_inventory(source, *dmar, intel, parse_error);
    }

    auto &amd = static_cast<iommu::amd_vi::firmware_inventory &>(result);
    if (duplicate_ivrs)
    {
        amd.ivrs_state = iommu::amd_vi::ivrs_inventory_state::invalid;
        amd.ivrs_error = arch::acpi_boot::error::duplicate_entry;
    }
    else if (ivrs != nullptr)
    {
        arch::acpi_boot::error parse_error = arch::acpi_boot::error::none;
        (void)iommu::amd_vi::parse_firmware_inventory(source, *ivrs, amd, parse_error);
    }

    failure = arch::acpi_boot::error::none;
    return true;
}
} // namespace naos_test::iommu_firmware
