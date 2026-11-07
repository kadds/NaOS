#include "kernel/iommu/firmware_inventory.hpp"

namespace iommu::amd_vi
{
using namespace arch::acpi_boot;
namespace
{
constexpr u64 ivrs_header_bytes = 48;
constexpr u64 ivrs_definition_header_bytes = 4;
constexpr u64 amd_iommu_register_bytes = 0x4000;
constexpr u32 amd_iommu_performance_register_bytes = 0x80000;
constexpr u64 amd_efr_performance_counter_support = 1ull << 9;
constexpr u32 amd_feature_report_counter_banks_mask = 0x3f << 17;
constexpr u32 amd_feature_report_counters_per_bank_mask = 0x0f << 13;

bool add_overflows(u64 left, u64 right) { return right > ~u64{0} - left; }

u32 load_u32(const u8 *bytes)
{
    return static_cast<u32>(bytes[0]) | static_cast<u32>(bytes[1]) << 8 | static_cast<u32>(bytes[2]) << 16 |
           static_cast<u32>(bytes[3]) << 24;
}

u16 load_u16(const u8 *bytes) { return static_cast<u16>(bytes[0]) | static_cast<u16>(bytes[1]) << 8; }

u64 load_u64(const u8 *bytes)
{
    return static_cast<u64>(load_u32(bytes)) | static_cast<u64>(load_u32(bytes + 4)) << 32;
}

template <typename T>
bool ensure_inventory_capacity(freelibcxx::vector<T> &entries, u64 required, u64 maximum, error &failure)
{
    if (required > maximum)
    {
        failure = error::too_many_tables;
        return false;
    }
    if (required > entries.capacity())
        entries.ensure(static_cast<size_t>(required));
    if (required > entries.capacity())
    {
        failure = error::allocation_failed;
        return false;
    }
    return true;
}
bool dmar_range_is_admitted(const physical_source &source, u64 base, u64 length)
{
    return source.device_range_valid != nullptr && source.device_range_valid(source.context, base, length);
}

bool iommu_register_range_is_admitted(const physical_source &source, u64 base, u64 length)
{
    if (source.iommu_register_range_valid != nullptr)
        return source.iommu_register_range_valid(source.context, base, length);
    return dmar_range_is_admitted(source, base, length);
}
u32 amd_ivhd_register_length(const amd_ivhd_record &record)
{
    const bool performance_counters_supported =
        record.type == 0x10 ? (record.feature_info &
                               (amd_feature_report_counter_banks_mask | amd_feature_report_counters_per_bank_mask)) != 0
                            : (record.extended_features & amd_efr_performance_counter_support) != 0;
    return performance_counters_supported ? amd_iommu_performance_register_bytes
                                          : static_cast<u32>(amd_iommu_register_bytes);
}
bool parse_ivhd_device_entries(const u8 *bytes, u64 offset, u64 block_end, amd_ivhd_record &record,
                               firmware_inventory &result, error &failure)
{
    bool range_open = false;
    u16 range_start = 0;
    record.device_entry_start = static_cast<u32>(result.amd_ivhd_device_entries.size());
    record.device_entry_count = 0;

    while (offset < block_end)
    {
        if (block_end - offset < 1 || result.amd_ivhd_device_entries.size() >= max_ivrs_inventory_entries)
        {
            failure = error::malformed_subtable;
            return false;
        }

        const u8 type = bytes[offset];
        if (range_open && type != 0x04)
        {
            failure = error::malformed_subtable;
            return false;
        }
        if (type == 0xf0 && record.type != 0x40)
        {
            failure = error::malformed_subtable;
            return false;
        }
        u64 length = 0;
        if (type < 0x80)
            length = 4ull << (type >> 6);
        else if (type == 0xf0)
        {
            if (block_end - offset < 22)
            {
                failure = error::malformed_subtable;
                return false;
            }
            length = 22ull + bytes[offset + 21];
        }
        else
        {
            failure = error::malformed_subtable;
            return false;
        }
        if (length > block_end - offset || length > 0xffffull || offset > 0xffffffffull)
        {
            failure = error::malformed_subtable;
            return false;
        }

        amd_ivhd_device_entry entry{};
        entry.table_offset = static_cast<u32>(offset);
        entry.type = type;
        entry.length = static_cast<u16>(length);
        if (type == 0xf0)
        {
            entry.device_id = load_u16(bytes + offset + 1);
            entry.source_id = entry.device_id;
            entry.data_setting = bytes[offset + 3];
            const u8 uid_format = bytes[offset + 20];
            const u8 uid_length = bytes[offset + 21];
            if (uid_format > 2 || (uid_format == 0 && uid_length != 0))
            {
                failure = error::malformed_subtable;
                return false;
            }
        }
        else
        {
            entry.device_id = load_u16(bytes + offset + 1);
            entry.data_setting = bytes[offset + 3];
            switch (type)
            {
                case 0x00:
                    if (entry.device_id != 0 || entry.data_setting != 0)
                    {
                        failure = error::malformed_subtable;
                        return false;
                    }
                    break;
                case 0x01:
                case 0x02:
                case 0x03:
                    break;
                case 0x04:
                    if (entry.data_setting != 0)
                    {
                        failure = error::malformed_subtable;
                        return false;
                    }
                    break;
                case 0x42:
                case 0x43:
                    entry.source_id = load_u16(bytes + offset + 5);
                    if (bytes[offset + 4] != 0 || bytes[offset + 7] != 0)
                    {
                        failure = error::malformed_subtable;
                        return false;
                    }
                    break;
                case 0x46:
                case 0x47:
                    entry.extended_data = bytes[offset + 4];
                    if (bytes[offset + 5] != 0 || bytes[offset + 6] != 0 || bytes[offset + 7] != 0)
                    {
                        failure = error::malformed_subtable;
                        return false;
                    }
                    break;
                case 0x48:
                    if (bytes[offset + 1] != 0 || bytes[offset + 2] != 0)
                    {
                        failure = error::malformed_subtable;
                        return false;
                    }
                    entry.handle = bytes[offset + 4];
                    entry.source_id = load_u16(bytes + offset + 5);
                    entry.variety = bytes[offset + 7];
                    if (entry.variety != 1 && entry.variety != 2)
                    {
                        failure = error::malformed_subtable;
                        return false;
                    }
                    break;
                default:
                    // Types 0x40-0x41 and 0x44-0x7f are reserved by the
                    // AMD IVHD device-entry tables; do not infer their size
                    // as permission to accept an unknown encoding.
                    failure = error::malformed_subtable;
                    return false;
            }
        }

        if (type == 0x03 || type == 0x43 || type == 0x47)
        {
            if (range_open)
            {
                failure = error::malformed_subtable;
                return false;
            }
            range_open = true;
            range_start = entry.device_id;
        }
        else if (type == 0x04)
        {
            if (!range_open || entry.device_id < range_start)
            {
                failure = error::malformed_subtable;
                return false;
            }
            range_open = false;
        }

        if (!ensure_inventory_capacity(result.amd_ivhd_device_entries, result.amd_ivhd_device_entries.size() + 1,
                                       max_ivrs_inventory_entries, failure))
            return false;
        result.amd_ivhd_device_entries.push_back(entry);
        record.device_entry_count++;
        offset += length;
    }

    if (range_open)
    {
        failure = error::malformed_subtable;
        return false;
    }
    return offset == block_end;
}
bool parse_ivrs_inventory(const physical_source &source, const table_record &table, firmware_inventory &result,
                          error &failure)
{
    failure = error::none;
    const auto fail = [&result, &failure](error reason) {
        result.ivrs_info = 0;
        result.amd_ivhd_records.clear();
        result.amd_ivhd_device_entries.clear();
        result.amd_ivmd_records.clear();
        result.ivrs_state = ivrs_inventory_state::invalid;
        result.ivrs_error = reason;
        failure = error::none;
        return false;
    };

    if (table.snapshot == nullptr || table.length < ivrs_header_bytes || (table.revision != 1 && table.revision != 2))
        return fail(error::invalid_length);

    const auto *bytes = reinterpret_cast<const u8 *>(table.snapshot);
    result.ivrs_info = load_u32(bytes + 36);
    for (u64 index = 40; index < ivrs_header_bytes; ++index)
    {
        if (bytes[index] != 0)
            return fail(error::malformed_subtable);
    }

    u64 offset = ivrs_header_bytes;
    u64 block_count = 0;
    u32 current_ivhd = ~u32{0};
    while (offset < table.length)
    {
        result.ivrs_error_offset = offset;
        if (table.length - offset < ivrs_definition_header_bytes || ++block_count > max_ivrs_inventory_entries)
            return fail(error::malformed_subtable);

        const u8 type = bytes[offset];
        const u8 flags = bytes[offset + 1];
        const u16 length = load_u16(bytes + offset + 2);
        if (length < ivrs_definition_header_bytes || length > table.length - offset)
            return fail(error::malformed_subtable);
        const u64 block_end = offset + length;

        if (type == 0x10 || type == 0x11 || type == 0x40)
        {
            const u64 fixed_bytes = type == 0x10 ? 24 : 40;
            const u8 reserved_ivhd_flag_mask = type == 0x10 ? 0 : 0xc0;
            if (length < fixed_bytes || result.amd_ivhd_records.size() >= max_ivrs_inventory_entries ||
                (flags & reserved_ivhd_flag_mask) != 0 ||
                ((type == 0x11 || type == 0x40) && (result.ivrs_info & 1u) == 0))
                return fail(error::malformed_subtable);

            amd_ivhd_record record{};
            record.type = type;
            record.flags = flags;
            record.device_id = load_u16(bytes + offset + 4);
            record.capability_offset = load_u16(bytes + offset + 6);
            record.register_base = load_u64(bytes + offset + 8);
            record.segment = load_u16(bytes + offset + 16);
            record.info = load_u16(bytes + offset + 18);
            if ((record.info & 0xe0e0) != 0)
                return fail(error::malformed_subtable);
            record.ivmd_start = static_cast<u32>(result.amd_ivmd_records.size());
            if (type == 0x10)
            {
                record.feature_info = load_u32(bytes + offset + 20);
                if ((result.ivrs_info & 1u) == 0 && record.feature_info != 0)
                    return fail(error::malformed_subtable);
                constexpr u32 feature_report_msi_num_ppr_mask = 0x1f << 23;
                if ((record.flags & 0x80) == 0 && (record.feature_info & feature_report_msi_num_ppr_mask) != 0)
                    return fail(error::malformed_subtable);
            }
            else
            {
                record.attributes = load_u32(bytes + offset + 20);
                record.extended_features = load_u64(bytes + offset + 24);
                record.extended_features_2 = load_u64(bytes + offset + 32);
                constexpr u32 reserved_iommu_attributes = 0xf0001fff;
                constexpr u32 attribute_msi_num_ppr_mask = 0x1f << 23;
                constexpr u64 extended_feature_ppr_support = 1ull << 1;
                if ((record.attributes & reserved_iommu_attributes) != 0 ||
                    ((record.extended_features & extended_feature_ppr_support) == 0 &&
                     (record.attributes & attribute_msi_num_ppr_mask) != 0))
                    return fail(error::malformed_subtable);
            }

            record.register_length = amd_ivhd_register_length(record);
            if (record.register_base == 0 || (record.register_base & (record.register_length - 1)) != 0 ||
                add_overflows(record.register_base, record.register_length) ||
                !iommu_register_range_is_admitted(source, record.register_base, record.register_length))
                return fail(error::invalid_address);

            for (u32 index = 0; index < result.amd_ivhd_records.size(); ++index)
            {
                auto &existing = result.amd_ivhd_records.at(index);
                const bool same_unit =
                    existing.segment == record.segment && existing.register_base == record.register_base;
                const u64 current_end = record.register_base + record.register_length;
                const u64 existing_end = existing.register_base + existing.register_length;
                if (same_unit)
                {
                    if (existing.device_id != record.device_id ||
                        existing.capability_offset != record.capability_offset || existing.info != record.info ||
                        existing.type == type)
                        return fail(error::duplicate_entry);
                    continue;
                }
                if (record.register_base < existing_end && existing.register_base < current_end)
                    return fail(error::duplicate_entry);
                if (existing.segment == record.segment && existing.device_id == record.device_id)
                    return fail(error::duplicate_entry);
            }

            if (!parse_ivhd_device_entries(bytes, offset + fixed_bytes, block_end, record, result, failure))
                return fail(failure == error::none ? error::malformed_subtable : failure);
            if (!ensure_inventory_capacity(result.amd_ivhd_records, result.amd_ivhd_records.size() + 1,
                                           max_ivrs_inventory_entries, failure))
                return fail(failure);

            record.selected = true;
            for (u32 index = 0; index < result.amd_ivhd_records.size(); ++index)
            {
                auto &existing = result.amd_ivhd_records.at(index);
                if (existing.segment != record.segment || existing.register_base != record.register_base)
                    continue;
                if (existing.type > type)
                    record.selected = false;
                else
                    existing.selected = false;
            }
            result.amd_ivhd_records.push_back(record);
            current_ivhd = static_cast<u32>(result.amd_ivhd_records.size() - 1);
        }
        else if (type >= 0x20 && type <= 0x22)
        {
            constexpr u64 fixed_bytes = 32;
            if (current_ivhd == ~u32{0} || length != fixed_bytes || (flags & 0xf0) != 0 ||
                result.amd_ivmd_records.size() >= max_ivrs_inventory_entries)
                return fail(error::malformed_subtable);
            const u16 device_id = load_u16(bytes + offset + 4);
            const u16 auxiliary_data = load_u16(bytes + offset + 6);
            const u16 segment = load_u16(bytes + offset + 8);
            for (u64 index = offset + 10; index < offset + 16; ++index)
            {
                if (bytes[index] != 0)
                    return fail(error::malformed_subtable);
            }
            const u64 base = load_u64(bytes + offset + 16);
            const u64 range_length = load_u64(bytes + offset + 24);
            if (range_length == 0 || add_overflows(base, range_length) ||
                ((flags & 8) == 0 && (flags & 1) != 0 && (flags & 6) == 0) ||
                (type == 0x20 && (device_id != 0 || auxiliary_data != 0 || segment != 0)) ||
                (type == 0x21 && auxiliary_data != 0) || (type == 0x22 && auxiliary_data < device_id))
                return fail(error::malformed_subtable);

            const auto &owner = result.amd_ivhd_records.at(current_ivhd);
            if (type != 0x20 && segment != owner.segment)
                return fail(error::malformed_subtable);
            if (!ensure_inventory_capacity(result.amd_ivmd_records, result.amd_ivmd_records.size() + 1,
                                           max_ivrs_inventory_entries, failure))
                return fail(failure);
            result.amd_ivmd_records.push_back(
                amd_ivmd_record{current_ivhd, base, range_length, device_id, auxiliary_data, segment, type, flags});
            result.amd_ivhd_records.at(current_ivhd).ivmd_count++;
        }
        else
            return fail(error::malformed_subtable);

        offset = block_end;
    }

    if (result.amd_ivhd_records.empty())
        return fail(error::malformed_subtable);
    result.ivrs_state = ivrs_inventory_state::valid;
    result.ivrs_error = error::none;
    result.ivrs_error_offset = 0;
    return true;
}

} // namespace

bool parse_firmware_inventory(const physical_source &source, const table_record &table, firmware_inventory &inventory,
                              error &failure)
{
    return parse_ivrs_inventory(source, table, inventory, failure);
}
namespace
{
bool ivhd_entry_span_valid(const firmware_inventory &inventory, const amd_ivhd_record &record)
{
    return record.device_entry_start <= inventory.amd_ivhd_device_entries.size() &&
           record.device_entry_count <= inventory.amd_ivhd_device_entries.size() - record.device_entry_start;
}

bool resolve_ivhd_pci_entry(const firmware_inventory &inventory, const amd_ivhd_record &record, u16 target_device_id,
                            bool &matched, u16 &requester_id, bool &alias_matched)
{
    matched = false;
    requester_id = target_device_id;
    alias_matched = false;
    if (!ivhd_entry_span_valid(inventory, record))
        return false;

    for (u32 offset = 0; offset < record.device_entry_count; ++offset)
    {
        const auto &entry = inventory.amd_ivhd_device_entries.at(record.device_entry_start + offset);
        bool entry_matches = false;
        u16 entry_requester_id = target_device_id;
        bool entry_is_alias = false;
        switch (entry.type)
        {
            case 0x01: // All devices.
                entry_matches = true;
                break;
            case 0x02: // Select.
            case 0x46: // Extended select.
                entry_matches = entry.device_id == target_device_id;
                break;
            case 0x42: // Alias select.
                entry_matches = entry.device_id == target_device_id;
                entry_requester_id = entry.source_id;
                entry_is_alias = true;
                break;
            case 0x03:   // Select range start.
            case 0x43:   // Alias range start.
            case 0x47: { // Extended select range start.
                if (offset + 1 >= record.device_entry_count)
                    return false;
                const auto &end = inventory.amd_ivhd_device_entries.at(record.device_entry_start + offset + 1);
                if (end.type != 0x04 || end.device_id < entry.device_id)
                    return false;
                entry_matches = target_device_id >= entry.device_id && target_device_id <= end.device_id;
                if (entry.type == 0x43)
                {
                    entry_requester_id = entry.source_id;
                    entry_is_alias = true;
                }
                ++offset;
                break;
            }
            case 0x00: // Alignment padding.
            case 0x04: // Range end, consumed with its start entry.
            case 0x48: // Special device, resolved separately for IOAPIC/HPET.
            case 0xf0: // ACPI namespace device, not a PCI BDF.
                break;
            default:
                return false;
        }

        if (!entry_matches)
            continue;
        if (matched)
            return false;
        matched = true;
        requester_id = entry_requester_id;
        alias_matched = entry_is_alias;
    }
    return true;
}
} // namespace

bool resolve_pci_device_unit(const firmware_inventory &inventory, u16 segment, u8 bus, u8 device, u8 function,
                             u32 &unit_index, u16 &requester_id, source_resolution_error &failure)
{
    failure = source_resolution_error::none;
    unit_index = ~u32{0};
    requester_id = 0;
    if (inventory.ivrs_state != ivrs_inventory_state::valid || device >= 32 || function >= 8)
    {
        failure = source_resolution_error::invalid_inventory;
        return false;
    }

    const u16 device_id = static_cast<u16>((static_cast<u16>(bus) << 8) | (static_cast<u16>(device) << 3) | function);
    bool unit_found = false;
    for (u32 index = 0; index < inventory.amd_ivhd_records.size(); ++index)
    {
        const auto &record = inventory.amd_ivhd_records.at(index);
        if (!record.selected || record.segment != segment)
            continue;
        if (!ivhd_entry_span_valid(inventory, record))
        {
            failure = source_resolution_error::invalid_device_entry_range;
            return false;
        }

        bool matched = false;
        bool alias_matched = false;
        u16 resolved_requester_id = device_id;
        if (!resolve_ivhd_pci_entry(inventory, record, device_id, matched, resolved_requester_id, alias_matched))
        {
            failure = source_resolution_error::malformed_device_entry_sequence;
            return false;
        }
        if (!matched)
            continue;
        if (alias_matched)
        {
            // A shared alias DTE also shares translation and interrupt state.
            // Until the runtime can prove and enforce the complete alias group,
            // do not hand the identity to a DMA or interrupt provider.
            failure = source_resolution_error::unsupported_device_alias;
            return false;
        }
        if (unit_found)
        {
            failure = source_resolution_error::ambiguous_device;
            return false;
        }
        unit_found = true;
        unit_index = index;
        requester_id = resolved_requester_id;
    }

    if (!unit_found)
    {
        failure = source_resolution_error::device_not_found;
        return false;
    }
    return true;
}

bool resolve_ioapic_source(const firmware_inventory &inventory, u16 segment, u8 ioapic_id, u32 &unit_index,
                           u16 &requester_id, u8 &dte_setting, source_resolution_error &failure)
{
    failure = source_resolution_error::none;
    unit_index = ~u32{0};
    requester_id = 0;
    dte_setting = 0;
    if (inventory.ivrs_state != ivrs_inventory_state::valid)
    {
        failure = source_resolution_error::invalid_inventory;
        return false;
    }

    bool unit_found = false;
    for (u32 index = 0; index < inventory.amd_ivhd_records.size(); ++index)
    {
        const auto &record = inventory.amd_ivhd_records.at(index);
        if (!record.selected || record.segment != segment)
            continue;
        if (!ivhd_entry_span_valid(inventory, record))
        {
            failure = source_resolution_error::invalid_device_entry_range;
            return false;
        }
        for (u32 entry_index = 0; entry_index < record.device_entry_count; ++entry_index)
        {
            const auto &entry = inventory.amd_ivhd_device_entries.at(record.device_entry_start + entry_index);
            if (entry.type != 0x48 || entry.variety != 1 || entry.handle != ioapic_id)
                continue;
            if (unit_found)
            {
                failure = source_resolution_error::ambiguous_device;
                return false;
            }
            unit_found = true;
            unit_index = index;
            requester_id = entry.source_id;
            dte_setting = entry.data_setting;
        }
    }

    if (!unit_found)
    {
        failure = source_resolution_error::device_not_found;
        return false;
    }
    return true;
}

bool resolve_ioapic_source(const firmware_inventory &inventory, u16 segment, u8 ioapic_id, u32 &unit_index,
                           u16 &requester_id, source_resolution_error &failure)
{
    u8 ignored_dte_setting = 0;
    return resolve_ioapic_source(inventory, segment, ioapic_id, unit_index, requester_id, ignored_dte_setting, failure);
}

bool resolve_ioapic_source(const firmware_inventory &inventory, u8 ioapic_id, u32 &unit_index, u16 &segment,
                           u16 &requester_id, u8 &dte_setting, source_resolution_error &failure)
{
    failure = source_resolution_error::none;
    unit_index = ~u32{0};
    segment = 0;
    requester_id = 0;
    dte_setting = 0;
    if (inventory.ivrs_state != ivrs_inventory_state::valid)
    {
        failure = source_resolution_error::invalid_inventory;
        return false;
    }

    for (u32 index = 0; index < inventory.amd_ivhd_records.size(); ++index)
    {
        const auto &record = inventory.amd_ivhd_records.at(index);
        if (!record.selected)
            continue;
        if (!ivhd_entry_span_valid(inventory, record))
        {
            failure = source_resolution_error::invalid_device_entry_range;
            return false;
        }
        for (u32 entry_index = 0; entry_index < record.device_entry_count; ++entry_index)
        {
            const auto &entry = inventory.amd_ivhd_device_entries.at(record.device_entry_start + entry_index);
            if (entry.type != 0x48 || entry.variety != 1 || entry.handle != ioapic_id)
                continue;
            if (unit_index != ~u32{0})
            {
                unit_index = ~u32{0};
                failure = source_resolution_error::ambiguous_device;
                return false;
            }
            unit_index = index;
            segment = record.segment;
            requester_id = entry.source_id;
            dte_setting = entry.data_setting;
        }
    }
    if (unit_index == ~u32{0})
    {
        failure = source_resolution_error::device_not_found;
        return false;
    }
    return true;
}

} // namespace iommu::amd_vi
