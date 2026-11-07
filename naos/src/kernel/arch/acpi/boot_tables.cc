#include "kernel/arch/acpi/boot_tables.hpp"

#include "kernel/mm/memory.hpp"
#include "kernel/mm/new.hpp"
#if defined(__STDC_HOSTED__) && __STDC_HOSTED__
#include <cstring>
#endif

namespace arch::acpi_boot
{
namespace
{
constexpr u64 rsdp_v1_bytes = 20;
constexpr u64 rsdp_v2_min_bytes = 36;
constexpr u64 sdt_header_bytes = 36;
constexpr u64 fadt_dsdt_offset = 40;
constexpr u64 fadt_facs_offset = 36;
constexpr u64 fadt_xfacs_offset = 132;
constexpr u64 fadt_xdsdt_offset = 140;
constexpr u64 facs_header_bytes = 8;
constexpr u64 facs_alignment = 64;
constexpr u64 mcfg_allocation_bytes = 16;
u64 next_pci_fabric_generation = 1;

bool add_overflows(u64 left, u64 right) { return right > ~u64(0) - left; }

template <typename T> bool ensure_append_capacity(freelibcxx::vector<T> &values, error &failure)
{
    if (values.size() == ~size_t{0})
    {
        failure = error::allocation_failed;
        return false;
    }
    const size_t required = values.size() + 1;
    if (values.capacity() < required)
        values.ensure(required);
    if (values.capacity() < required)
    {
        failure = error::allocation_failed;
        return false;
    }
    return true;
}

bool mapped(const physical_source &source, u64 address, u64 length, const u8 *&bytes, error &failure)
{
    if (length == 0 || add_overflows(address, length) || source.map == nullptr || source.range_valid == nullptr ||
        !source.range_valid(source.context, address, length))
    {
        failure = error::invalid_address;
        return false;
    }
    bytes = source.map(source.context, address, length);
    if (bytes == nullptr)
    {
        failure = error::invalid_address;
        return false;
    }
    return true;
}

u8 checksum(const u8 *bytes, u64 length)
{
    u8 result = 0;
    for (u64 index = 0; index < length; index++)
        result = static_cast<u8>(result + bytes[index]);
    return result;
}

bool printable_signature(const u8 *bytes)
{
    for (u64 index = 0; index < 4; index++)
    {
        const auto value = bytes[index];
        if (!((value >= 'A' && value <= 'Z') || (value >= '0' && value <= '9') || value == '_' || value == ' '))
            return false;
    }
    return true;
}

u32 load_u32(const u8 *bytes)
{
    return static_cast<u32>(bytes[0]) | static_cast<u32>(bytes[1]) << 8 | static_cast<u32>(bytes[2]) << 16 |
           static_cast<u32>(bytes[3]) << 24;
}

u64 load_u64(const u8 *bytes)
{
    return static_cast<u64>(load_u32(bytes)) | static_cast<u64>(load_u32(bytes + 4)) << 32;
}

bool same_signature(const char *left, const char *right)
{
    return left[0] == right[0] && left[1] == right[1] && left[2] == right[2] && left[3] == right[3];
}

table_record *find_physical(snapshot &result, u64 physical)
{
    for (auto &table : result.tables)
    {
        if (table.physical_address == physical)
            return &table;
    }
    return nullptr;
}

u64 result_catalog_bytes(const snapshot &result);

bool copy_record(const physical_source &source, u64 physical, const table_header &header, snapshot &result,
                 u32 root_ordinal, u32 aml_load_ordinal, table_record *&record, error &failure)
{
    if (result.tables.size() >= max_table_count)
    {
        failure = error::too_many_tables;
        return false;
    }
    if (!ensure_append_capacity(result.tables, failure))
        return false;
    if (result.tables.size() != 0 && find_physical(result, physical) != nullptr)
    {
        failure = error::duplicate_entry;
        return false;
    }
    if (header.length > max_catalog_bytes - result_catalog_bytes(result))
    {
        failure = error::catalog_too_large;
        return false;
    }
    if (memory::MemoryAllocatorV == nullptr)
    {
        failure = error::allocation_failed;
        return false;
    }
    auto *copy = static_cast<byte *>(memory::MemoryAllocatorV->allocate(header.length, alignof(u64)));
    if (copy == nullptr)
    {
        failure = error::allocation_failed;
        return false;
    }
    memcpy(copy, header.bytes, header.length);
    table_record value;
    value.object_id = result.tables.size() + 1;
    value.signature = signature_value(header.signature);
    value.revision = header.revision;
    value.length = header.length;
    value.physical_address = physical;
    value.snapshot = copy;
    value.root_ordinal = root_ordinal;
    value.aml_load_ordinal = aml_load_ordinal;
    result.tables.push_back(value);
    record = &result.tables[result.tables.size() - 1];
    return true;
}

u64 result_catalog_bytes(const snapshot &result)
{
    u64 total = 0;
    for (const auto &table : result.tables)
    {
        if (table.snapshot == nullptr)
            continue;
        if (table.length > ~u64(0) - total)
            return ~u64(0);
        total += table.length;
    }
    return total;
}

bool describe_facs(u64 physical, const table_header &header, snapshot &result, table_record *&record, error &failure)
{
    if (result.tables.size() >= max_table_count)
    {
        failure = error::too_many_tables;
        return false;
    }
    if (!ensure_append_capacity(result.tables, failure))
        return false;
    if (find_physical(result, physical) != nullptr)
    {
        failure = error::duplicate_entry;
        return false;
    }

    table_record value;
    value.object_id = result.tables.size() + 1;
    value.signature = signature_value("FACS");
    value.revision = 0;
    value.length = header.length;
    value.physical_address = physical;
    value.snapshot = nullptr;
    value.has_checksum = false;
    value.root_ordinal = 0xffffffffu;
    value.aml_load_ordinal = 0xffffffffu;
    result.tables.push_back(value);
    record = &result.tables[result.tables.size() - 1];
    return true;
}

bool validate_facs(const physical_source &source, u64 address, table_header &result, error &failure)
{
    if ((address & (facs_alignment - 1)) != 0 || source.device_range_valid == nullptr ||
        !source.device_range_valid(source.context, address, facs_header_bytes))
    {
        failure = error::invalid_address;
        return false;
    }
    const u8 *prefix = nullptr;
    if (!mapped(source, address, facs_header_bytes, prefix, failure) || memcmp(prefix, "FACS", 4) != 0)
    {
        failure = error::invalid_header;
        return false;
    }
    const u32 length = load_u32(prefix + 4);
    if (length < 64 || length > max_table_bytes)
    {
        failure = error::invalid_length;
        return false;
    }
    if (!source.device_range_valid(source.context, address, length))
    {
        failure = error::invalid_address;
        return false;
    }
    const u8 *bytes = nullptr;
    if (!mapped(source, address, length, bytes, failure))
        return false;
    memcpy(result.signature, "FACS", 4);
    result.signature[4] = '\0';
    result.revision = 0;
    result.length = length;
    result.physical_address = address;
    result.bytes = bytes;
    return true;
}

bool add_indirect_table(const physical_source &source, u64 address, const char *expected, snapshot &result,
                        u32 aml_load_ordinal, table_record *&record, error &failure)
{
    if (address == 0)
    {
        failure = error::invalid_address;
        return false;
    }
    table_header header{};
    if (!validate_table(source, address, header, failure) || !same_signature(header.signature, expected))
    {
        failure = error::invalid_header;
        return false;
    }
    return copy_record(source, address, header, result, 0xffffffffu, aml_load_ordinal, record, failure);
}

} // namespace

u32 signature_value(const char signature[5])
{
    return static_cast<u32>(static_cast<u8>(signature[0])) | static_cast<u32>(static_cast<u8>(signature[1])) << 8 |
           static_cast<u32>(static_cast<u8>(signature[2])) << 16 |
           static_cast<u32>(static_cast<u8>(signature[3])) << 24;
}

bool validate_rsdp(const physical_source &source, u64 address, rsdp_info &result, error &failure)
{
    const u8 *v1 = nullptr;
    if (!mapped(source, address, rsdp_v1_bytes, v1, failure))
        return false;
    if (memcmp(v1, "RSD PTR ", 8) != 0 || checksum(v1, rsdp_v1_bytes) != 0)
    {
        failure = error::invalid_checksum;
        return false;
    }
    result = {};
    result.revision = v1[15];
    result.length = rsdp_v1_bytes;
    result.root_address = load_u32(v1 + 16);
    result.uses_xsdt = false;
    if (result.revision < 2)
    {
        if (result.root_address == 0)
        {
            failure = error::no_root;
            return false;
        }
        return true;
    }

    const u8 *v2_prefix = nullptr;
    if (!mapped(source, address, rsdp_v2_min_bytes, v2_prefix, failure))
        return false;
    const u32 length = load_u32(v2_prefix + 20);
    if (length < rsdp_v2_min_bytes || length > max_table_bytes)
    {
        failure = error::invalid_length;
        return false;
    }
    const u8 *v2 = nullptr;
    if (!mapped(source, address, length, v2, failure) || checksum(v2, length) != 0)
    {
        failure = error::invalid_checksum;
        return false;
    }
    result.length = length;
    result.root_address = load_u64(v2 + 24);
    result.uses_xsdt = result.root_address != 0;
    if (result.root_address == 0)
    {
        result.root_address = load_u32(v2 + 16);
        result.uses_xsdt = false;
    }
    if (result.root_address == 0)
    {
        failure = error::no_root;
        return false;
    }
    return true;
}

bool validate_table(const physical_source &source, u64 address, table_header &result, error &failure)
{
    const u8 *prefix = nullptr;
    if (!mapped(source, address, sdt_header_bytes, prefix, failure))
        return false;
    if (!printable_signature(prefix))
    {
        failure = error::invalid_header;
        return false;
    }
    const u32 length = load_u32(prefix + 4);
    if (length < sdt_header_bytes || length > max_table_bytes)
    {
        failure = error::invalid_length;
        return false;
    }
    const u8 *bytes = nullptr;
    if (!mapped(source, address, length, bytes, failure))
        return false;
    if (checksum(bytes, length) != 0)
    {
        failure = error::invalid_checksum;
        return false;
    }
    memcpy(result.signature, bytes, 4);
    result.signature[4] = '\0';
    result.revision = bytes[8];
    result.length = length;
    result.physical_address = address;
    result.bytes = bytes;
    return true;
}

bool build_snapshot(const physical_source &source, u64 rsdp_address, u64 boot_epoch, u64 generation, snapshot &result,
                    error &failure)
{
    if (boot_epoch == 0 || generation == 0)
    {
        failure = error::invalid_generation;
        return false;
    }
    result.tables.clear();
    result.relations.clear();
    result.mcfg_regions.clear();
    result.boot_epoch = boot_epoch;
    result.firmware_generation = generation;
    // PCI discovery has its own lifecycle. It starts a separate monotonic
    // generation even when the initial snapshot happens to be built from
    // firmware generation one; future fabric rescans must not reuse the
    // firmware namespace's counter.
    if (next_pci_fabric_generation == 0)
    {
        failure = error::invalid_generation;
        return false;
    }
    result.pci_fabric_generation = next_pci_fabric_generation;
    next_pci_fabric_generation = next_pci_fabric_generation == ~u64(0) ? 0 : next_pci_fabric_generation + 1;

    rsdp_info rsdp{};
    if (!validate_rsdp(source, rsdp_address, rsdp, failure))
        return false;
    result.uses_xsdt = rsdp.uses_xsdt;

    const u8 *rsdp_bytes = nullptr;
    if (!mapped(source, rsdp_address, rsdp.length, rsdp_bytes, failure))
        return false;
    table_header rsdp_header{};
    memcpy(rsdp_header.signature, "RSDP", 4);
    rsdp_header.revision = rsdp.revision;
    rsdp_header.length = rsdp.length;
    rsdp_header.physical_address = rsdp_address;
    rsdp_header.bytes = rsdp_bytes;
    table_record *rsdp_record = nullptr;
    if (!copy_record(source, rsdp_address, rsdp_header, result, 0xffffffffu, 0xffffffffu, rsdp_record, failure))
        return false;
    rsdp_record->has_checksum = true;
    result.rsdp_object_id = rsdp_record->object_id;

    table_header root{};
    if (!validate_table(source, rsdp.root_address, root, failure))
        return false;
    const char *expected_root = rsdp.uses_xsdt ? "XSDT" : "RSDT";
    if (!same_signature(root.signature, expected_root))
    {
        failure = error::invalid_root;
        return false;
    }
    table_record *root_record = nullptr;
    if (!copy_record(source, rsdp.root_address, root, result, 0xffffffffu, 0xffffffffu, root_record, failure))
        return false;
    result.root_object_id = root_record->object_id;

    const u64 entry_bytes = root.length - sdt_header_bytes;
    const u64 entry_size = rsdp.uses_xsdt ? 8 : 4;
    if (entry_bytes % entry_size != 0)
    {
        failure = error::invalid_root;
        return false;
    }
    const u64 entry_count = entry_bytes / entry_size;
    u32 ssdt_ordinal = 1;
    const u64 root_object_id = root_record->object_id;
    u64 fadt_physical = 0;
    u64 mcfg_physical = 0;
    for (u64 index = 0; index < entry_count; index++)
    {
        const u64 offset = sdt_header_bytes + index * entry_size;
        const u64 address = rsdp.uses_xsdt ? load_u64(root.bytes + offset) : load_u32(root.bytes + offset);
        if (address == 0)
        {
            failure = error::invalid_address;
            return false;
        }
        table_header child{};
        if (!validate_table(source, address, child, failure))
            return false;
        table_record *record = nullptr;
        const bool is_ssdt = same_signature(child.signature, "SSDT");
        if (!copy_record(source, address, child, result, static_cast<u32>(index),
                         is_ssdt ? ssdt_ordinal++ : 0xffffffffu, record, failure))
            return false;
        if (!ensure_append_capacity(result.relations, failure))
            return false;
        result.relations.push_back(
            relation{root_object_id, record->object_id, relation_kind::root_child, static_cast<u32>(index)});
        if (same_signature(child.signature, "FACP"))
        {
            if (fadt_physical != 0)
            {
                failure = error::duplicate_entry;
                return false;
            }
            fadt_physical = address;
        }
        else if (same_signature(child.signature, "MCFG"))
        {
            if (mcfg_physical != 0)
            {
                failure = error::duplicate_entry;
                return false;
            }
            mcfg_physical = address;
        }
    }

    const table_record *mcfg = mcfg_physical == 0 ? nullptr : find_physical(result, mcfg_physical);
    if (mcfg != nullptr)
    {
        if (mcfg->length < 44 || (mcfg->length - 44) % mcfg_allocation_bytes != 0)
        {
            failure = error::invalid_length;
            return false;
        }
        for (u64 offset = 44; offset < mcfg->length; offset += mcfg_allocation_bytes)
        {
            u64 base = load_u64(reinterpret_cast<const u8 *>(mcfg->snapshot + offset));
            u16 segment = static_cast<u16>(static_cast<u8>(mcfg->snapshot[offset + 8])) |
                          static_cast<u16>(static_cast<u8>(mcfg->snapshot[offset + 9])) << 8;
            const u8 start_bus = static_cast<u8>(mcfg->snapshot[offset + 10]);
            const u8 end_bus = static_cast<u8>(mcfg->snapshot[offset + 11]);
            const u64 bus_count = static_cast<u64>(end_bus) - start_bus + 1;
            if (base == 0 || (base & 0xfff) != 0 || start_bus > end_bus || bus_count > (~u64(0) >> 20) ||
                base > ~u64(0) - (bus_count << 20))
            {
                failure = error::invalid_address;
                return false;
            }
            const auto range_valid =
                source.device_range_valid != nullptr ? source.device_range_valid : source.range_valid;
            if (range_valid == nullptr || !range_valid(source.context, base, bus_count << 20))
            {
                failure = error::invalid_address;
                return false;
            }
            for (const auto &existing : result.mcfg_regions)
            {
                if (existing.segment == segment && !(end_bus < existing.start_bus || start_bus > existing.end_bus))
                {
                    failure = error::duplicate_entry;
                    return false;
                }
                const u64 existing_bytes = (static_cast<u64>(existing.end_bus) - existing.start_bus + 1) << 20;
                const u64 existing_end = existing.base + existing_bytes;
                const u64 current_end = base + (bus_count << 20);
                if (base < existing_end && existing.base < current_end)
                {
                    // Different segment numbers do not make overlapping ECAM
                    // physical pages safe.  Reject the alias before any
                    // user-visible discovery mapping can be minted.
                    failure = error::duplicate_entry;
                    return false;
                }
            }
            if (!ensure_append_capacity(result.mcfg_regions, failure))
                return false;
            result.mcfg_regions.push_back(mcfg_region{base, segment, start_bus, end_bus, 0});
        }
    }

    const table_record *fadt_entry = fadt_physical == 0 ? nullptr : find_physical(result, fadt_physical);
    if (fadt_entry != nullptr)
    {
        // Adding an indirect DSDT/FACS record may grow result.tables and
        // invalidate pointers into its storage. Keep the FADT record by value
        // while following those references; its snapshot byte buffer is
        // separately allocated and remains stable.
        const table_record fadt = *fadt_entry;
        if (fadt.length < fadt_dsdt_offset + sizeof(u32))
        {
            failure = error::invalid_length;
            return false;
        }
        const u64 legacy_dsdt = load_u32(reinterpret_cast<const u8 *>(fadt.snapshot + fadt_dsdt_offset));
        u64 dsdt_address = legacy_dsdt;
        u64 facs_address = load_u32(reinterpret_cast<const u8 *>(fadt.snapshot + fadt_facs_offset));
        if (fadt.length >= fadt_xdsdt_offset + sizeof(u64))
        {
            const u64 extended_dsdt = load_u64(reinterpret_cast<const u8 *>(fadt.snapshot + fadt_xdsdt_offset));
            if (extended_dsdt != 0)
                dsdt_address = extended_dsdt;
        }
        if (fadt.length >= fadt_xfacs_offset + sizeof(u64))
        {
            const u64 extended_facs = load_u64(reinterpret_cast<const u8 *>(fadt.snapshot + fadt_xfacs_offset));
            if (extended_facs != 0)
                facs_address = extended_facs;
        }
        if (dsdt_address != 0)
        {
            table_record *dsdt = find_physical(result, dsdt_address);
            if (dsdt == nullptr)
            {
                if (!add_indirect_table(source, dsdt_address, "DSDT", result, 0, dsdt, failure))
                    return false;
            }
            else if (dsdt->signature != signature_value("DSDT"))
            {
                failure = error::invalid_header;
                return false;
            }
            dsdt->aml_load_ordinal = 0;
            if (!ensure_append_capacity(result.relations, failure))
                return false;
            result.relations.push_back(relation{fadt.object_id, dsdt->object_id, relation_kind::fadt_dsdt, 0});
        }
        if (facs_address != 0)
        {
            table_header facs_header{};
            if (!validate_facs(source, facs_address, facs_header, failure))
                return false;

            table_record *facs = find_physical(result, facs_address);
            if (facs == nullptr)
            {
                if (!describe_facs(facs_address, facs_header, result, facs, failure))
                    return false;
            }
            else if (facs->signature != signature_value("FACS") || facs->length != facs_header.length)
            {
                failure = error::invalid_header;
                return false;
            }
            if (!ensure_append_capacity(result.relations, failure))
                return false;
            result.relations.push_back(relation{fadt.object_id, facs->object_id, relation_kind::fadt_facs, 0});
        }
    }
    failure = error::none;
    return true;
}

bool read_u32(const table_header &table, u64 offset, u32 &value)
{
    if (table.bytes == nullptr || offset > table.length || sizeof(u32) > table.length - offset)
        return false;
    value = load_u32(table.bytes + offset);
    return true;
}

bool read_u64(const table_header &table, u64 offset, u64 &value)
{
    if (table.bytes == nullptr || offset > table.length || sizeof(u64) > table.length - offset)
        return false;
    value = load_u64(table.bytes + offset);
    return true;
}

bool madt_local_apic_base(const u8 *bytes, u64 length, u64 physical_address_width, u64 &base)
{
    base = 0;
    if (bytes == nullptr || length < 44 || memcmp(bytes, "APIC", 4) != 0 || physical_address_width < 12 ||
        physical_address_width > 64)
        return false;

    u64 resolved_base = load_u32(bytes + 36);
    bool override_seen = false;
    for (u64 offset = 44; offset < length;)
    {
        if (length - offset < 2)
            return false;
        const u8 type = bytes[offset];
        const u8 entry_length = bytes[offset + 1];
        if (entry_length < 2 || entry_length > length - offset)
            return false;

        if (type == 5)
        {
            if (override_seen || entry_length != 12 || bytes[offset + 2] != 0 || bytes[offset + 3] != 0)
                return false;
            resolved_base = load_u64(bytes + offset + 4);
            override_seen = true;
        }
        offset += entry_length;
    }

    if (resolved_base == 0 || (resolved_base & 0xfff) != 0 ||
        (physical_address_width < 64 && resolved_base >= (u64{1} << physical_address_width)))
        return false;
    base = resolved_base;
    return true;
}

} // namespace arch::acpi_boot
