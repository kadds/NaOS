#include "kernel/iommu/firmware_inventory.hpp"

namespace iommu::intel_vtd
{
using namespace arch::acpi_boot;
namespace
{
constexpr u64 dmar_header_bytes = 48;
constexpr u64 dmar_structure_header_bytes = 4;
constexpr u64 dmar_device_scope_header_bytes = 6;
constexpr u64 page_bytes = 4096;

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
bool address_fits_dmar_width(u64 address, u8 host_address_width)
{
    return host_address_width == 63 || (address >> (host_address_width + 1)) == 0;
}

bool parse_device_scopes(const u8 *bytes, u64 offset, u64 structure_end, bool retain, firmware_inventory &result,
                         u32 &scope_start, u32 &scope_count, error &failure)
{
    failure = error::none;
    scope_start = static_cast<u32>(result.dmar_device_scopes.size());
    scope_count = 0;
    while (offset < structure_end)
    {
        if (structure_end - offset < dmar_device_scope_header_bytes)
        {
            failure = error::malformed_subtable;
            return false;
        }

        const u8 type = bytes[offset];
        const u8 length = bytes[offset + 1];
        const u8 flags = bytes[offset + 2];
        const u8 reserved = bytes[offset + 3];
        const u8 enumeration_id = bytes[offset + 4];
        const u8 start_bus = bytes[offset + 5];
        if (length < dmar_device_scope_header_bytes + 2 || length > structure_end - offset || type < 1 || type > 5 ||
            reserved != 0 || (flags & 0xe0) != 0 || ((type == 1 || type == 2) && enumeration_id != 0))
        {
            failure = error::malformed_subtable;
            return false;
        }

        const u64 path_bytes = length - dmar_device_scope_header_bytes;
        if ((path_bytes & 1) != 0)
        {
            failure = error::malformed_subtable;
            return false;
        }
        const u64 path_count = path_bytes / 2;
        for (u64 path_index = 0; path_index < path_count; path_index++)
        {
            const u64 path_offset = offset + dmar_device_scope_header_bytes + path_index * 2;
            if (bytes[path_offset] >= 32 || bytes[path_offset + 1] >= 8)
            {
                failure = error::malformed_subtable;
                return false;
            }
        }

        if (scope_count >= max_dmar_inventory_entries)
        {
            failure = error::too_many_tables;
            return false;
        }

        if (retain)
        {
            if (!ensure_inventory_capacity(result.dmar_device_scopes, result.dmar_device_scopes.size() + 1,
                                           max_dmar_inventory_entries, failure) ||
                !ensure_inventory_capacity(result.dmar_device_paths, result.dmar_device_paths.size() + path_count,
                                           max_dmar_scope_paths, failure))
                return false;

            const u32 path_start = static_cast<u32>(result.dmar_device_paths.size());
            for (u64 path_index = 0; path_index < path_count; path_index++)
            {
                const u64 path_offset = offset + dmar_device_scope_header_bytes + path_index * 2;
                result.dmar_device_paths.push_back(dmar_device_path{bytes[path_offset], bytes[path_offset + 1]});
            }
            result.dmar_device_scopes.push_back(
                dmar_device_scope{path_start, static_cast<u16>(path_count), type, flags, enumeration_id, start_bus});
        }
        scope_count++;
        offset += length;
    }
    return offset == structure_end;
}

bool dmar_range_is_admitted(const physical_source &source, u64 base, u64 length)
{
    return source.device_range_valid != nullptr && source.device_range_valid(source.context, base, length);
}

bool dmar_register_range_is_admitted(const physical_source &source, u64 base, u64 length)
{
    if (source.iommu_register_range_valid != nullptr)
        return source.iommu_register_range_valid(source.context, base, length);
    return dmar_range_is_admitted(source, base, length);
}
bool dmar_segment_is_present(const firmware_inventory &result, u16 segment)
{
    for (const auto &unit : result.dmar_units)
    {
        if (unit.segment == segment)
            return true;
    }
    return false;
}
bool parse_dmar_inventory(const physical_source &source, const table_record &table, firmware_inventory &result,
                          error &failure)
{
    failure = error::none;
    const auto fail = [&result, &failure](error reason) {
        result.dmar_units.clear();
        result.dmar_reserved_regions.clear();
        result.dmar_device_scopes.clear();
        result.dmar_device_paths.clear();
        result.dmar_host_address_width = 0;
        result.dmar_flags = 0;
        result.dmar_state = dmar_inventory_state::invalid;
        result.dmar_error = reason;
        failure = error::none;
        return false;
    };

    if (table.snapshot == nullptr || table.length < dmar_header_bytes || table.revision != 1)
        return fail(error::invalid_length);

    const auto *bytes = reinterpret_cast<const u8 *>(table.snapshot);
    const u8 host_address_width = bytes[36];
    const u8 flags = bytes[37];
    if (host_address_width > 63 || (flags & 0xf8) != 0 || ((flags & 0x02) != 0 && (flags & 0x01) == 0))
        return fail(error::malformed_subtable);
    for (u64 index = 38; index < dmar_header_bytes; index++)
    {
        if (bytes[index] != 0)
            return fail(error::malformed_subtable);
    }

    result.dmar_host_address_width = host_address_width;
    result.dmar_flags = flags;
    u64 offset = dmar_header_bytes;
    u16 previous_type = 0;
    bool has_previous_type = false;
    u64 structure_count = 0;
    while (offset < table.length)
    {
        result.dmar_error_offset = offset;
        if (table.length - offset < dmar_structure_header_bytes || ++structure_count > max_dmar_inventory_entries)
            return fail(error::malformed_subtable);

        const u16 type = load_u16(bytes + offset);
        const u16 length = load_u16(bytes + offset + 2);
        if (type > 6 || length < dmar_structure_header_bytes || length > table.length - offset ||
            (has_previous_type && type < previous_type))
            return fail(error::malformed_subtable);
        previous_type = type;
        has_previous_type = true;
        const u64 structure_end = offset + length;

        switch (type)
        {
            case 0: {
                constexpr u64 fixed_bytes = 16;
                if (length < fixed_bytes || result.dmar_units.size() >= max_dmar_inventory_entries)
                    return fail(error::invalid_length);
                const u8 unit_flags = bytes[offset + 4];
                const u8 register_size = bytes[offset + 5];
                const u16 segment = load_u16(bytes + offset + 6);
                const u64 register_base = load_u64(bytes + offset + 8);
                if ((unit_flags & 0xfe) != 0 || (register_size & 0xf0) != 0 || register_base == 0 ||
                    (register_base & (page_bytes - 1)) != 0)
                    return fail(error::malformed_subtable);
                const u64 register_length = page_bytes << (register_size & 0x0f);
                if (add_overflows(register_base, register_length) ||
                    !address_fits_dmar_width(register_base + register_length - 1, host_address_width))
                    return fail(error::invalid_address);
                if (!dmar_register_range_is_admitted(source, register_base, register_length))
                {
                    result.dmar_error_address = register_base;
                    result.dmar_error_length = register_length;
                    return fail(error::invalid_address);
                }

                for (const auto &existing : result.dmar_units)
                {
                    const u64 existing_end = existing.register_base + existing.register_length;
                    const u64 current_end = register_base + register_length;
                    if (register_base < existing_end && existing.register_base < current_end)
                        return fail(error::duplicate_entry);
                    if (existing.segment == segment && existing.include_all && (unit_flags & 1) == 0)
                        return fail(error::malformed_subtable);
                    if (existing.segment == segment && existing.include_all && (unit_flags & 1) != 0)
                        return fail(error::duplicate_entry);
                }

                u32 scope_start = 0;
                u32 scope_count = 0;
                if (!parse_device_scopes(bytes, offset + fixed_bytes, structure_end, true, result, scope_start,
                                         scope_count, failure))
                    return fail(failure == error::none ? error::malformed_subtable : failure);
                if ((unit_flags & 1) != 0)
                {
                    for (u32 scope_index = 0; scope_index < scope_count; ++scope_index)
                    {
                        const auto &scope = result.dmar_device_scopes.at(scope_start + scope_index);
                        if (scope.type == dmar_device_scope_type_pci_endpoint ||
                            scope.type == dmar_device_scope_type_pci_subhierarchy)
                            return fail(error::malformed_subtable);
                    }
                }
                if (!ensure_inventory_capacity(result.dmar_units, result.dmar_units.size() + 1,
                                               max_dmar_inventory_entries, failure))
                    return fail(failure);
                result.dmar_units.push_back(
                    dmar_hardware_unit{register_base, register_length, scope_start, scope_count, segment, unit_flags,
                                       static_cast<u8>(register_size & 0x0f), (unit_flags & 1) != 0});
                break;
            }
            case 1: {
                constexpr u64 fixed_bytes = 24;
                if (length < fixed_bytes || load_u16(bytes + offset + 4) != 0)
                    return fail(error::malformed_subtable);
                const u16 segment = load_u16(bytes + offset + 6);
                const u64 base = load_u64(bytes + offset + 8);
                const u64 limit = load_u64(bytes + offset + 16);
                if (!dmar_segment_is_present(result, segment) || (base & (page_bytes - 1)) != 0 || limit < base ||
                    limit == ~u64(0) || ((limit + 1) & (page_bytes - 1)) != 0 ||
                    !address_fits_dmar_width(limit, host_address_width))
                    return fail(error::invalid_address);
                const u64 region_length = limit - base + 1;
                if (!dmar_range_is_admitted(source, base, region_length))
                {
                    result.dmar_error_address = base;
                    result.dmar_error_length = region_length;
                    return fail(error::invalid_address);
                }

                u32 scope_start = 0;
                u32 scope_count = 0;
                if (!parse_device_scopes(bytes, offset + fixed_bytes, structure_end, true, result, scope_start,
                                         scope_count, failure))
                    return fail(failure == error::none ? error::malformed_subtable : failure);
                if (scope_count == 0)
                    return fail(error::malformed_subtable);
                if (!ensure_inventory_capacity(result.dmar_reserved_regions, result.dmar_reserved_regions.size() + 1,
                                               max_dmar_inventory_entries, failure))
                    return fail(failure);
                result.dmar_reserved_regions.push_back(
                    dmar_reserved_region{base, region_length, scope_start, scope_count, segment});
                break;
            }
            case 2: {
                constexpr u64 fixed_bytes = 8;
                if (length < fixed_bytes || (bytes[offset + 4] & 0xfe) != 0 || bytes[offset + 5] != 0)
                    return fail(error::malformed_subtable);
                const u16 segment = load_u16(bytes + offset + 6);
                if (!dmar_segment_is_present(result, segment))
                    return fail(error::invalid_address);
                u32 scope_start = 0;
                u32 scope_count = 0;
                if (!parse_device_scopes(bytes, offset + fixed_bytes, structure_end, false, result, scope_start,
                                         scope_count, failure))
                    return fail(failure == error::none ? error::malformed_subtable : failure);
                if ((bytes[offset + 4] & 1) == 0 && scope_count == 0)
                    return fail(error::malformed_subtable);
                break;
            }
            case 3: {
                constexpr u64 fixed_bytes = 20;
                if (length != fixed_bytes || load_u32(bytes + offset + 4) != 0)
                    return fail(error::malformed_subtable);
                const u64 register_base = load_u64(bytes + offset + 8);
                bool found_unit = false;
                for (const auto &unit : result.dmar_units)
                    found_unit = found_unit || unit.register_base == register_base;
                if (!found_unit)
                    return fail(error::invalid_address);
                break;
            }
            case 4: {
                constexpr u64 name_offset = 8;
                if (length <= name_offset + 1 || bytes[offset + 4] != 0 || bytes[offset + 5] != 0 ||
                    bytes[offset + 6] != 0 || bytes[structure_end - 1] != 0)
                    return fail(error::malformed_subtable);
                for (u64 index = offset + name_offset; index + 1 < structure_end; index++)
                {
                    if (bytes[index] == 0)
                        return fail(error::malformed_subtable);
                }
                break;
            }
            case 5:
            case 6: {
                constexpr u64 fixed_bytes = 8;
                if (length < fixed_bytes)
                    return fail(error::invalid_length);
                if (type == 5 && ((bytes[offset + 4] & 0xfe) != 0 || bytes[offset + 5] != 0))
                    return fail(error::malformed_subtable);
                if (type == 6 && load_u16(bytes + offset + 4) != 0)
                    return fail(error::malformed_subtable);
                const u16 segment = load_u16(bytes + offset + 6);
                if (!dmar_segment_is_present(result, segment))
                    return fail(error::invalid_address);
                u32 scope_start = 0;
                u32 scope_count = 0;
                if (!parse_device_scopes(bytes, offset + fixed_bytes, structure_end, false, result, scope_start,
                                         scope_count, failure))
                    return fail(failure == error::none ? error::malformed_subtable : failure);
                if (scope_count == 0)
                    return fail(error::malformed_subtable);
                break;
            }
            default:
                return fail(error::malformed_subtable);
        }
        offset = structure_end;
    }

    if (result.dmar_units.empty())
        return fail(error::malformed_subtable);
    result.dmar_state = dmar_inventory_state::valid;
    result.dmar_error = error::none;
    result.dmar_error_offset = 0;
    result.dmar_error_address = 0;
    result.dmar_error_length = 0;
    return true;
}

} // namespace

bool parse_firmware_inventory(const physical_source &source, const table_record &table, firmware_inventory &inventory,
                              error &failure)
{
    return parse_dmar_inventory(source, table, inventory, failure);
}
namespace
{
bool pci_scope_contains_function(const firmware_inventory &inventory, const dmar_device_scope &scope, u16 segment,
                                 u8 target_bus, u8 target_device, u8 target_function,
                                 bridge_info_reader read_bridge_info, void *context, bool &matches,
                                 source_resolution_error &failure)
{
    matches = false;
    if ((scope.type != dmar_device_scope_type_pci_endpoint && scope.type != dmar_device_scope_type_pci_subhierarchy) ||
        scope.path_count == 0 || scope.path_start > inventory.dmar_device_paths.size() ||
        scope.path_count > inventory.dmar_device_paths.size() - scope.path_start)
    {
        failure = source_resolution_error::invalid_path;
        return false;
    }

    u8 bus = scope.start_bus;
    u8 parent_subordinate_bus = 0xff;
    for (u32 path_index = 0; path_index < scope.path_count; ++path_index)
    {
        if (bus > parent_subordinate_bus)
        {
            failure = source_resolution_error::invalid_path;
            return false;
        }
        const auto &path = inventory.dmar_device_paths.at(scope.path_start + path_index);
        if (path.device >= 32 || path.function >= 8)
        {
            failure = source_resolution_error::invalid_path;
            return false;
        }

        const bool last_path_element = path_index + 1 == scope.path_count;
        if (scope.type == dmar_device_scope_type_pci_endpoint && last_path_element)
        {
            matches = bus == target_bus && path.device == target_device && path.function == target_function;
            return true;
        }

        if (read_bridge_info == nullptr)
        {
            failure = source_resolution_error::config_reader_unavailable;
            return false;
        }
        u8 header_type = 0;
        u8 primary_bus = 0;
        u8 secondary_bus = 0;
        u8 subordinate_bus = 0;
        if (!read_bridge_info(context, segment, bus, path.device, path.function, header_type, primary_bus,
                              secondary_bus, subordinate_bus))
        {
            failure = source_resolution_error::config_read_failed;
            return false;
        }
        if ((header_type & 0x7f) != 1 || primary_bus != bus || secondary_bus <= bus ||
            subordinate_bus < secondary_bus || (path_index != 0 && subordinate_bus > parent_subordinate_bus))
        {
            failure = source_resolution_error::invalid_path;
            return false;
        }

        if (scope.type == dmar_device_scope_type_pci_subhierarchy && last_path_element)
        {
            matches = (bus == target_bus && path.device == target_device && path.function == target_function) ||
                      (target_bus >= secondary_bus && target_bus <= subordinate_bus);
            return true;
        }
        parent_subordinate_bus = subordinate_bus;
        bus = secondary_bus;
    }

    failure = source_resolution_error::invalid_path;
    return false;
}
} // namespace

bool resolve_ioapic_requester_id(const firmware_inventory &inventory, const dmar_hardware_unit &unit, u8 enumeration_id,
                                 bridge_info_reader read_bridge_info, void *context, u16 &requester_id,
                                 source_resolution_error &failure)
{
    failure = source_resolution_error::none;
    requester_id = 0;
    if (inventory.dmar_state != dmar_inventory_state::valid)
    {
        failure = source_resolution_error::invalid_inventory;
        return false;
    }
    if (unit.device_scope_start > inventory.dmar_device_scopes.size() ||
        unit.device_scope_count > inventory.dmar_device_scopes.size() - unit.device_scope_start)
    {
        failure = source_resolution_error::invalid_scope_range;
        return false;
    }

    const dmar_device_scope *matched_scope = nullptr;
    for (u32 index = 0; index < unit.device_scope_count; ++index)
    {
        const auto &scope = inventory.dmar_device_scopes.at(unit.device_scope_start + index);
        if (scope.type != dmar_device_scope_type_ioapic || scope.enumeration_id != enumeration_id)
            continue;
        if (matched_scope != nullptr)
        {
            failure = source_resolution_error::ambiguous_scope;
            return false;
        }
        matched_scope = &scope;
    }
    if (matched_scope == nullptr)
    {
        failure = source_resolution_error::scope_not_found;
        return false;
    }

    if (matched_scope->path_count == 0 || matched_scope->path_start > inventory.dmar_device_paths.size() ||
        matched_scope->path_count > inventory.dmar_device_paths.size() - matched_scope->path_start)
    {
        failure = source_resolution_error::invalid_path;
        return false;
    }

    u8 bus = matched_scope->start_bus;
    for (u32 index = 0; index + 1 < matched_scope->path_count; ++index)
    {
        const auto &bridge = inventory.dmar_device_paths.at(matched_scope->path_start + index);
        if (bridge.device >= 32 || bridge.function >= 8)
        {
            failure = source_resolution_error::invalid_path;
            return false;
        }
        if (read_bridge_info == nullptr)
        {
            failure = source_resolution_error::config_reader_unavailable;
            return false;
        }

        u8 header_type = 0;
        u8 primary_bus = 0;
        u8 secondary_bus = 0;
        u8 subordinate_bus = 0;
        if (!read_bridge_info(context, unit.segment, bus, bridge.device, bridge.function, header_type, primary_bus,
                              secondary_bus, subordinate_bus))
        {
            failure = source_resolution_error::config_read_failed;
            return false;
        }
        if ((header_type & 0x7f) != 1 || primary_bus != bus || secondary_bus <= bus || subordinate_bus < secondary_bus)
        {
            failure = source_resolution_error::invalid_path;
            return false;
        }
        bus = secondary_bus;
    }

    const auto &endpoint = inventory.dmar_device_paths.at(matched_scope->path_start + matched_scope->path_count - 1);
    if (endpoint.device >= 32 || endpoint.function >= 8)
    {
        failure = source_resolution_error::invalid_path;
        return false;
    }
    requester_id =
        static_cast<u16>(static_cast<u16>(bus) << 8 | static_cast<u16>(endpoint.device) << 3 | endpoint.function);
    return true;
}

bool resolve_pci_device_unit(const firmware_inventory &inventory, u16 segment, u8 bus, u8 device, u8 function,
                             bridge_info_reader read_bridge_info, void *context, u32 &unit_index, u16 &requester_id,
                             source_resolution_error &failure)
{
    failure = source_resolution_error::none;
    unit_index = ~u32{0};
    requester_id = 0;
    if (inventory.dmar_state != dmar_inventory_state::valid || device >= 32 || function >= 8)
    {
        failure = source_resolution_error::invalid_inventory;
        return false;
    }

    const u32 encoded_requester_id = (static_cast<u32>(bus) << 8) | (static_cast<u32>(device) << 3) | function;
    const auto *selected_unit = static_cast<const dmar_hardware_unit *>(nullptr);
    u32 selected_index = ~u32{0};
    const auto *include_all_unit = static_cast<const dmar_hardware_unit *>(nullptr);
    u32 include_all_index = ~u32{0};
    bool include_all_seen = false;

    for (u32 index = 0; index < inventory.dmar_units.size(); ++index)
    {
        const auto &unit = inventory.dmar_units.at(index);
        if (unit.segment != segment)
            continue;
        if (unit.include_all != ((unit.flags & 1u) != 0))
        {
            failure = source_resolution_error::invalid_inventory;
            return false;
        }
        if (include_all_seen)
        {
            failure = source_resolution_error::invalid_inventory_order;
            return false;
        }
        if (unit.device_scope_start > inventory.dmar_device_scopes.size() ||
            unit.device_scope_count > inventory.dmar_device_scopes.size() - unit.device_scope_start)
        {
            failure = source_resolution_error::invalid_scope_range;
            return false;
        }

        if (unit.include_all)
        {
            include_all_seen = true;
            include_all_unit = &unit;
            include_all_index = index;
            for (u32 scope_index = 0; scope_index < unit.device_scope_count; ++scope_index)
            {
                const auto &scope = inventory.dmar_device_scopes.at(unit.device_scope_start + scope_index);
                if (scope.type == dmar_device_scope_type_pci_endpoint ||
                    scope.type == dmar_device_scope_type_pci_subhierarchy)
                {
                    failure = source_resolution_error::invalid_include_all_scope;
                    return false;
                }
            }
            continue;
        }

        for (u32 scope_index = 0; scope_index < unit.device_scope_count; ++scope_index)
        {
            const auto &scope = inventory.dmar_device_scopes.at(unit.device_scope_start + scope_index);
            if (scope.type != dmar_device_scope_type_pci_endpoint &&
                scope.type != dmar_device_scope_type_pci_subhierarchy)
                continue;

            bool matches = false;
            if (!pci_scope_contains_function(inventory, scope, segment, bus, device, function, read_bridge_info,
                                             context, matches, failure))
                return false;
            if (!matches)
                continue;
            if (selected_unit != nullptr && selected_index != index)
            {
                failure = source_resolution_error::ambiguous_unit;
                return false;
            }
            selected_unit = &unit;
            selected_index = index;
        }
    }

    if (selected_unit == nullptr)
    {
        if (include_all_unit == nullptr)
        {
            failure = source_resolution_error::unit_not_found;
            return false;
        }
        selected_index = include_all_index;
    }

    unit_index = selected_index;
    requester_id = static_cast<u16>(encoded_requester_id);
    return true;
}

} // namespace iommu::intel_vtd
