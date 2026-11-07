#pragma once

#include "freelibcxx/vector.hpp"
#include "kernel/common.hpp"
#include "kernel/pci/firmware.hpp"
#include "kernel/types.hpp"

namespace arch::acpi_boot
{

/// Limits are part of the FirmwareRoot admission contract.  A malformed
/// table is rejected before the kernel copies any bytes or follows another
/// firmware pointer.
inline constexpr u64 max_table_bytes = 1 * 1024 * 1024;
inline constexpr u64 max_catalog_bytes = 16 * 1024 * 1024;
inline constexpr u64 max_table_count = 1024;
inline constexpr u32 facs_object_signature =
    static_cast<u32>('F') | static_cast<u32>('A') << 8 | static_cast<u32>('C') << 16 | static_cast<u32>('S') << 24;
inline constexpr u32 firmware_object_flag_no_checksum = 1u << 0;
inline constexpr u32 firmware_object_flag_mutable_state = 1u << 1;

/// FACS is a mutable state block, not an immutable table snapshot. It can be
/// described in FirmwareRoot but must not be opened through the generic
/// read-only MemoryObject interface.
constexpr bool firmware_object_is_openable(u32 signature) noexcept { return signature != facs_object_signature; }

constexpr u32 firmware_object_flags(u32 signature, bool has_checksum) noexcept
{
    return (has_checksum ? 0u : firmware_object_flag_no_checksum) |
           (signature == facs_object_signature ? firmware_object_flag_mutable_state : 0u);
}

enum class error : u8
{
    none,
    no_root,
    invalid_rsdp,
    invalid_address,
    invalid_header,
    invalid_length,
    invalid_checksum,
    invalid_root,
    invalid_generation,
    duplicate_entry,
    malformed_subtable,
    too_many_tables,
    catalog_too_large,
    allocation_failed,
};

/// The parser never invents a physical mapping.  The kernel supplies this
/// callback after checking the range against its boot memory map.  A caller
/// that cannot prove the entire [address, address + length) range is valid
/// must return nullptr.
struct physical_source
{
    const u8 *(*map)(void *context, u64 address, u64 length);
    bool (*range_valid)(void *context, u64 address, u64 length);
    bool (*device_range_valid)(void *context, u64 address, u64 length);
    bool (*iommu_register_range_valid)(void *context, u64 address, u64 length);
    void *context;
};

struct rsdp_info
{
    u8 revision = 0;
    u32 length = 20;
    u64 root_address = 0;
    bool uses_xsdt = false;
};

struct table_header
{
    char signature[5]{};
    u8 revision = 0;
    u32 length = 0;
    u64 physical_address = 0;
    const u8 *bytes = nullptr;
};

enum class relation_kind : u8
{
    root_child,
    fadt_dsdt,
    fadt_facs,
};

struct relation
{
    u64 from = 0;
    u64 to = 0;
    relation_kind kind = relation_kind::root_child;
    u32 ordinal = 0;
};

using mcfg_region = pci::ecam_region;

struct table_record
{
    u64 object_id = 0;
    u32 signature = 0;
    u8 revision = 0;
    u64 length = 0;
    u64 physical_address = 0; // kernel-internal provenance; never sent to userland
    byte *snapshot = nullptr;
    bool has_checksum = true;
    u32 root_ordinal = 0;
    u32 aml_load_ordinal = 0xffffffffu;
};

struct snapshot
{
    u64 boot_epoch = 0;
    u64 firmware_generation = 0;
    u64 pci_fabric_generation = 0;
    u64 rsdp_object_id = 0;
    u64 root_object_id = 0;
    bool uses_xsdt = false;
    freelibcxx::vector<table_record> tables;
    freelibcxx::vector<relation> relations;
    freelibcxx::vector<mcfg_region> mcfg_regions;

    explicit snapshot(freelibcxx::Allocator *allocator)
        : tables(allocator)
        , relations(allocator)
        , mcfg_regions(allocator)
    {
    }
};

/// Validate the RSDP and return the selected RSDT/XSDT physical address.
/// This function does not map the root table and does not dereference any
/// address found in the structure.
bool validate_rsdp(const physical_source &source, u64 address, rsdp_info &result, error &failure);

/// Validate one SDT header, its bounded length and checksum.  The returned
/// view is valid only while the source mapping remains valid.
bool validate_table(const physical_source &source, u64 address, table_header &result, error &failure);

/// Build the immutable table/reference graph used by FirmwareRoot.  The
/// caller owns the allocator and the returned snapshot owns every copied
/// table buffer.  No AML is parsed here; DSDT/SSDT bytes are catalogued in
/// deterministic graph order for the user-space backend.
bool build_snapshot(const physical_source &source, u64 rsdp_address, u64 boot_epoch, u64 generation, snapshot &result,
                    error &failure);

/// Safe little-endian readers used by the kernel topology parser and tests.
bool read_u32(const table_header &table, u64 offset, u32 &value);
bool read_u64(const table_header &table, u64 offset, u64 &value);

/// Resolve the MADT Local APIC base, including a 64-bit type-5 address
/// override, and reject addresses outside the processor's physical width.
bool madt_local_apic_base(const u8 *bytes, u64 length, u64 physical_address_width, u64 &base);

u32 signature_value(const char signature[5]);

} // namespace arch::acpi_boot
