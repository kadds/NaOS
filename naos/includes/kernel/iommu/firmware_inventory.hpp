#pragma once

#include "kernel/arch/acpi/boot_tables.hpp"

namespace iommu::intel_vtd
{
inline constexpr u64 max_dmar_scope_paths = 8192;
inline constexpr u64 max_dmar_inventory_entries = 8192;

enum class dmar_inventory_state : u8
{
    absent,
    valid,
    invalid,
};

struct dmar_device_path
{
    u8 device = 0;
    u8 function = 0;
};

struct dmar_device_scope
{
    u32 path_start = 0;
    u16 path_count = 0;
    u8 type = 0;
    u8 flags = 0;
    u8 enumeration_id = 0;
    u8 start_bus = 0;
};

inline constexpr u8 dmar_device_scope_type_pci_endpoint = 1;
inline constexpr u8 dmar_device_scope_type_pci_subhierarchy = 2;
inline constexpr u8 dmar_device_scope_type_ioapic = 3;
inline constexpr u8 dmar_device_scope_type_hpet = 4;
inline constexpr u8 dmar_device_scope_type_acpi_namespace = 5;

struct dmar_hardware_unit
{
    u64 register_base = 0;
    u64 register_length = 0;
    u32 device_scope_start = 0;
    u32 device_scope_count = 0;
    u16 segment = 0;
    u8 flags = 0;
    u8 register_size = 0;
    bool include_all = false;
    u64 resource_lease_id = 0;
    u64 kernel_virtual_base = 0;
};

struct dmar_reserved_region
{
    u64 base = 0;
    u64 length = 0;
    u32 device_scope_start = 0;
    u32 device_scope_count = 0;
    u16 segment = 0;
};

struct firmware_inventory
{
    u64 firmware_generation = 0;
    freelibcxx::vector<dmar_hardware_unit> dmar_units;
    freelibcxx::vector<dmar_reserved_region> dmar_reserved_regions;
    freelibcxx::vector<dmar_device_scope> dmar_device_scopes;
    freelibcxx::vector<dmar_device_path> dmar_device_paths;
    u8 dmar_host_address_width = 0;
    u8 dmar_flags = 0;
    dmar_inventory_state dmar_state = dmar_inventory_state::absent;
    arch::acpi_boot::error dmar_error = arch::acpi_boot::error::none;
    u64 dmar_error_offset = 0;
    u64 dmar_error_address = 0;
    u64 dmar_error_length = 0;

    explicit firmware_inventory(freelibcxx::Allocator *allocator)
        : dmar_units(allocator)
        , dmar_reserved_regions(allocator)
        , dmar_device_scopes(allocator)
        , dmar_device_paths(allocator)
    {
    }
};

enum class source_resolution_error : u8
{
    none,
    invalid_inventory,
    invalid_scope_range,
    scope_not_found,
    ambiguous_scope,
    invalid_path,
    config_reader_unavailable,
    config_read_failed,
    unit_not_found,
    ambiguous_unit,
    invalid_inventory_order,
    invalid_include_all_scope,
};

using bridge_info_reader = bool (*)(void *context, u16 segment, u8 bus, u8 device, u8 function, u8 &header_type,
                                    u8 &primary_bus, u8 &secondary_bus, u8 &subordinate_bus);

bool parse_firmware_inventory(const arch::acpi_boot::physical_source &source,
                              const arch::acpi_boot::table_record &table, firmware_inventory &inventory,
                              arch::acpi_boot::error &failure);
bool resolve_ioapic_requester_id(const firmware_inventory &inventory, const dmar_hardware_unit &unit, u8 enumeration_id,
                                 bridge_info_reader read_bridge_info, void *context, u16 &requester_id,
                                 source_resolution_error &failure);
bool resolve_pci_device_unit(const firmware_inventory &inventory, u16 segment, u8 bus, u8 device, u8 function,
                             bridge_info_reader read_bridge_info, void *context, u32 &unit_index, u16 &requester_id,
                             source_resolution_error &failure);
} // namespace iommu::intel_vtd

namespace iommu::amd_vi
{
inline constexpr u64 max_ivrs_inventory_entries = 8192;

enum class ivrs_inventory_state : u8
{
    absent,
    valid,
    invalid,
};

struct amd_ivhd_device_entry
{
    u32 table_offset = 0;
    u16 device_id = 0;
    u16 source_id = 0;
    u8 type = 0;
    u8 data_setting = 0;
    u8 extended_data = 0;
    u8 handle = 0;
    u8 variety = 0;
    u16 length = 0;
};

struct amd_ivhd_record
{
    u32 device_entry_start = 0;
    u32 device_entry_count = 0;
    u32 ivmd_start = 0;
    u32 ivmd_count = 0;
    u64 register_base = 0;
    u64 resource_lease_id = 0;
    u64 kernel_virtual_base = 0;
    u64 extended_features = 0;
    u64 extended_features_2 = 0;
    u32 register_length = 0;
    u32 feature_info = 0;
    u32 attributes = 0;
    u16 device_id = 0;
    u16 capability_offset = 0;
    u16 segment = 0;
    u16 info = 0;
    u8 type = 0;
    u8 flags = 0;
    bool selected = false;
};

struct amd_ivmd_record
{
    u32 ivhd_record_index = 0;
    u64 base = 0;
    u64 length = 0;
    u16 device_id = 0;
    u16 auxiliary_data = 0;
    u16 segment = 0;
    u8 type = 0;
    u8 flags = 0;
};

struct firmware_inventory
{
    u64 firmware_generation = 0;
    freelibcxx::vector<amd_ivhd_record> amd_ivhd_records;
    freelibcxx::vector<amd_ivhd_device_entry> amd_ivhd_device_entries;
    freelibcxx::vector<amd_ivmd_record> amd_ivmd_records;
    u32 ivrs_info = 0;
    ivrs_inventory_state ivrs_state = ivrs_inventory_state::absent;
    arch::acpi_boot::error ivrs_error = arch::acpi_boot::error::none;
    u64 ivrs_error_offset = 0;

    explicit firmware_inventory(freelibcxx::Allocator *allocator)
        : amd_ivhd_records(allocator)
        , amd_ivhd_device_entries(allocator)
        , amd_ivmd_records(allocator)
    {
    }
};

enum class source_resolution_error : u8
{
    none,
    invalid_inventory,
    invalid_device_entry_range,
    malformed_device_entry_sequence,
    device_not_found,
    ambiguous_device,
    unsupported_device_alias,
};

bool parse_firmware_inventory(const arch::acpi_boot::physical_source &source,
                              const arch::acpi_boot::table_record &table, firmware_inventory &inventory,
                              arch::acpi_boot::error &failure);
bool resolve_pci_device_unit(const firmware_inventory &inventory, u16 segment, u8 bus, u8 device, u8 function,
                             u32 &unit_index, u16 &requester_id, source_resolution_error &failure);
bool resolve_ioapic_source(const firmware_inventory &inventory, u16 segment, u8 ioapic_id, u32 &unit_index,
                           u16 &requester_id, source_resolution_error &failure);
bool resolve_ioapic_source(const firmware_inventory &inventory, u16 segment, u8 ioapic_id, u32 &unit_index,
                           u16 &requester_id, u8 &dte_setting, source_resolution_error &failure);
bool resolve_ioapic_source(const firmware_inventory &inventory, u8 ioapic_id, u32 &unit_index, u16 &segment,
                           u16 &requester_id, u8 &dte_setting, source_resolution_error &failure);
} // namespace iommu::amd_vi
