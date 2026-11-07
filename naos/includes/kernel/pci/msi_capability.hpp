#pragma once

#include "kernel/pci/bar.hpp"
#include "kernel/types.hpp"
#include <array>

namespace pci
{

enum class msi_capability_error : u8
{
    none,
    config_reader_unavailable,
    config_read_failed,
    capability_list_unavailable,
    invalid_capability_pointer,
    capability_chain_too_long,
    duplicate_msi_capability,
    unsupported_message_count,
    truncated_capability,
    msi_capability_not_found,
};

struct msi_capability
{
    u8 offset = 0;
    u8 maximum_messages = 0;
    bool address_64_bit = false;
    bool per_vector_masking = false;
};

using pci_config_read32 = bool (*)(void *context, u16 offset, u32 &value);

enum class msix_capability_error : u8
{
    none,
    config_reader_unavailable,
    config_read_failed,
    capability_list_unavailable,
    invalid_capability_pointer,
    capability_chain_too_long,
    duplicate_msix_capability,
    capability_not_found,
    invalid_bar_index,
    invalid_capability,
    truncated_capability,
    invalid_bar_aperture,
    table_out_of_bounds,
    pba_out_of_bounds,
    table_pba_overlap,
};

struct msix_capability
{
    u8 offset = 0;
    u16 table_size = 0;
    bool enabled = false;
    bool function_masked = false;
    u8 table_bar = 0;
    u32 table_offset = 0;
    u8 pba_bar = 0;
    u32 pba_offset = 0;
};

struct msix_bar_aperture
{
    u64 base = 0;
    u64 length = 0;
};

struct msix_region
{
    u64 base = 0;
    u64 length = 0;
};

inline constexpr u32 max_msix_root_windows = 12;

struct msix_table_mapping_plan
{
    msix_region table{};
    bar_page_cover page_cover{};
    u64 page_count = 0;
    u64 table_offset = 0;
};

enum class msix_table_mapping_error : u8
{
    none,
    invalid_table,
    invalid_aperture,
    table_out_of_bounds,
    invalid_resource_flags,
    not_writable,
    invalid_root_windows,
    root_escape,
};

enum class msix_bar_mapping_error : u8
{
    none,
    invalid_bar_index,
    table_bar_reserved,
    config_read_failed,
    invalid_capability,
};

/// Walk a bounded conventional PCI capability list and describe its unique
/// MSI capability without trusting firmware-provided pointers or lengths.
inline bool find_msi_capability(pci_config_read32 read_config, void *context, msi_capability &result,
                                msi_capability_error &failure) noexcept
{
    result = {};
    failure = msi_capability_error::none;
    if (read_config == nullptr)
    {
        failure = msi_capability_error::config_reader_unavailable;
        return false;
    }

    constexpr u16 status_command_offset = 0x04;
    constexpr u16 capability_pointer_offset = 0x34;
    constexpr u16 capability_min_offset = 0x40;
    constexpr u16 config_space_bytes = 0x100;
    constexpr u32 capability_list_status_bit = 1u << 20;
    constexpr u8 msi_capability_id = 0x05;
    constexpr u8 max_capability_hops = 48;

    u32 command_status = 0;
    u32 pointer_register = 0;
    if (!read_config(context, status_command_offset, command_status) ||
        !read_config(context, capability_pointer_offset, pointer_register))
    {
        failure = msi_capability_error::config_read_failed;
        return false;
    }
    if ((command_status & capability_list_status_bit) == 0)
    {
        failure = msi_capability_error::capability_list_unavailable;
        return false;
    }

    bool visited[config_space_bytes / 4]{};
    msi_capability candidate{};
    bool found = false;
    u8 offset = static_cast<u8>(pointer_register & 0xff);
    for (u8 hop = 0; offset != 0 && hop < max_capability_hops; ++hop)
    {
        if (offset < capability_min_offset || offset > config_space_bytes - 4 || (offset & 3) != 0)
        {
            failure = msi_capability_error::invalid_capability_pointer;
            return false;
        }
        const u32 slot = offset / 4;
        if (visited[slot])
        {
            failure = msi_capability_error::invalid_capability_pointer;
            return false;
        }
        visited[slot] = true;

        u32 header = 0;
        if (!read_config(context, offset, header))
        {
            failure = msi_capability_error::config_read_failed;
            return false;
        }
        const u8 capability_id = static_cast<u8>(header & 0xff);
        const u8 next = static_cast<u8>((header >> 8) & 0xff);
        const u16 control = static_cast<u16>(header >> 16);
        if (capability_id == msi_capability_id)
        {
            if (found)
            {
                failure = msi_capability_error::duplicate_msi_capability;
                return false;
            }

            const u8 message_count_encoding = static_cast<u8>((control >> 1) & 0x7);
            if (message_count_encoding > 5)
            {
                failure = msi_capability_error::unsupported_message_count;
                return false;
            }
            const bool address_64_bit = (control & (1u << 7)) != 0;
            const bool per_vector_masking = (control & (1u << 8)) != 0;
            const u16 capability_bytes = static_cast<u16>(10 + (address_64_bit ? 4 : 0) + (per_vector_masking ? 8 : 0));
            if (offset > config_space_bytes - capability_bytes)
            {
                failure = msi_capability_error::truncated_capability;
                return false;
            }
            candidate = {offset, static_cast<u8>(1u << message_count_encoding), address_64_bit, per_vector_masking};
            found = true;
        }
        offset = next;
    }

    if (offset != 0)
    {
        failure = msi_capability_error::capability_chain_too_long;
        return false;
    }
    if (!found)
    {
        failure = msi_capability_error::msi_capability_not_found;
        return false;
    }
    result = candidate;
    return true;
}

/// Walk the conventional capability list and validate the unique MSI-X
/// capability. Table/PBA apertures are decoded separately once their BARs
/// have been checked against the function's kernel-held resource attestation.
inline bool find_msix_capability(pci_config_read32 read_config, void *context, msix_capability &result,
                                 msix_capability_error &failure) noexcept
{
    result = {};
    failure = msix_capability_error::none;
    if (read_config == nullptr)
    {
        failure = msix_capability_error::config_reader_unavailable;
        return false;
    }

    constexpr u16 status_command_offset = 0x04;
    constexpr u16 capability_pointer_offset = 0x34;
    constexpr u16 capability_min_offset = 0x40;
    constexpr u16 config_space_bytes = 0x100;
    constexpr u32 capability_list_status_bit = 1u << 20;
    constexpr u8 msix_capability_id = 0x11;
    constexpr u8 max_capability_hops = 48;
    constexpr u16 reserved_control_mask = 0x3800;
    constexpr u16 table_size_mask = 0x07ff;
    constexpr u16 function_mask_bit = 1u << 14;
    constexpr u16 enable_bit = 1u << 15;
    constexpr u32 bir_mask = 0x7;
    constexpr u32 region_offset_mask = ~bir_mask;
    constexpr u8 bar_count = 6;

    u32 command_status = 0;
    u32 pointer_register = 0;
    if (!read_config(context, status_command_offset, command_status) ||
        !read_config(context, capability_pointer_offset, pointer_register))
    {
        failure = msix_capability_error::config_read_failed;
        return false;
    }
    if ((command_status & capability_list_status_bit) == 0)
    {
        failure = msix_capability_error::capability_list_unavailable;
        return false;
    }

    std::array<bool, config_space_bytes / 4> visited{};
    msix_capability candidate{};
    bool found = false;
    u8 offset = static_cast<u8>(pointer_register & 0xff);
    for (u8 hop = 0; offset != 0 && hop < max_capability_hops; ++hop)
    {
        if (offset < capability_min_offset || offset > config_space_bytes - 4 || (offset & 3) != 0)
        {
            failure = msix_capability_error::invalid_capability_pointer;
            return false;
        }
        const u32 slot = offset / 4;
        if (visited[slot])
        {
            failure = msix_capability_error::invalid_capability_pointer;
            return false;
        }
        visited[slot] = true;

        u32 header = 0;
        if (!read_config(context, offset, header))
        {
            failure = msix_capability_error::config_read_failed;
            return false;
        }
        const u8 capability_id = static_cast<u8>(header & 0xff);
        const u8 next = static_cast<u8>((header >> 8) & 0xff);
        const u16 control = static_cast<u16>(header >> 16);
        if (capability_id == msix_capability_id)
        {
            if (found)
            {
                failure = msix_capability_error::duplicate_msix_capability;
                return false;
            }
            if (offset > config_space_bytes - 12)
            {
                failure = msix_capability_error::truncated_capability;
                return false;
            }
            if ((control & reserved_control_mask) != 0)
            {
                failure = msix_capability_error::invalid_capability;
                return false;
            }

            u32 table = 0;
            u32 pba = 0;
            if (!read_config(context, offset + 4, table) || !read_config(context, offset + 8, pba))
            {
                failure = msix_capability_error::config_read_failed;
                return false;
            }
            const u8 table_bar = static_cast<u8>(table & bir_mask);
            const u8 pba_bar = static_cast<u8>(pba & bir_mask);
            if (table_bar >= bar_count || pba_bar >= bar_count)
            {
                failure = msix_capability_error::invalid_bar_index;
                return false;
            }

            candidate.offset = offset;
            candidate.table_size = static_cast<u16>((control & table_size_mask) + 1);
            candidate.enabled = (control & enable_bit) != 0;
            candidate.function_masked = (control & function_mask_bit) != 0;
            candidate.table_bar = table_bar;
            candidate.table_offset = table & region_offset_mask;
            candidate.pba_bar = pba_bar;
            candidate.pba_offset = pba & region_offset_mask;
            found = true;
        }
        offset = next;
    }

    if (offset != 0)
    {
        failure = msix_capability_error::capability_chain_too_long;
        return false;
    }
    if (!found)
    {
        failure = msix_capability_error::capability_not_found;
        return false;
    }
    result = candidate;
    return true;
}

/// Resolve MSI-X table and PBA byte ranges only against already-attested BAR
/// apertures. Both the logical offsets and the resulting physical ranges are
/// checked; the table and PBA may not alias each other.
inline bool resolve_msix_regions(const msix_capability &capability, const std::array<msix_bar_aperture, 6> &bars,
                                 msix_region &table, msix_region &pba, msix_capability_error &failure) noexcept
{
    table = {};
    pba = {};
    failure = msix_capability_error::none;
    if (capability.offset < 0x40 || capability.offset > 0xf4 || (capability.offset & 3) != 0 ||
        capability.table_size == 0 || capability.table_size > 2048 || capability.table_bar >= bars.size() ||
        capability.pba_bar >= bars.size() || (capability.table_offset & 7) != 0 || (capability.pba_offset & 7) != 0)
    {
        failure = msix_capability_error::invalid_capability;
        return false;
    }

    constexpr u64 table_entry_bytes = 16;
    constexpr u64 pba_bitmap_bytes = 8;
    const u64 table_length = static_cast<u64>(capability.table_size) * table_entry_bytes;
    const u64 pba_length = ((static_cast<u64>(capability.table_size) + 63) / 64) * pba_bitmap_bytes;
    const auto &table_bar = bars[capability.table_bar];
    const auto &pba_bar = bars[capability.pba_bar];
    if (table_bar.base == 0 || table_bar.length == 0 || table_bar.base > ~u64{0} - table_bar.length ||
        (table_bar.base & 0xf) != 0)
    {
        failure = msix_capability_error::invalid_bar_aperture;
        return false;
    }
    if (pba_bar.base == 0 || pba_bar.length == 0 || pba_bar.base > ~u64{0} - pba_bar.length ||
        (pba_bar.base & 0xf) != 0)
    {
        failure = msix_capability_error::invalid_bar_aperture;
        return false;
    }
    if (capability.table_offset > table_bar.length || table_length > table_bar.length - capability.table_offset)
    {
        failure = msix_capability_error::table_out_of_bounds;
        return false;
    }
    if (capability.pba_offset > pba_bar.length || pba_length > pba_bar.length - capability.pba_offset)
    {
        failure = msix_capability_error::pba_out_of_bounds;
        return false;
    }
    if (table_bar.base > ~u64{0} - capability.table_offset || pba_bar.base > ~u64{0} - capability.pba_offset)
    {
        failure = msix_capability_error::invalid_capability;
        return false;
    }

    table = {table_bar.base + capability.table_offset, table_length};
    pba = {pba_bar.base + capability.pba_offset, pba_length};
    if (table.base > ~u64{0} - table.length || pba.base > ~u64{0} - pba.length)
    {
        table = {};
        pba = {};
        failure = msix_capability_error::invalid_capability;
        return false;
    }
    const u64 table_end = table.base + table.length;
    const u64 pba_end = pba.base + pba.length;
    if (table.base < pba_end && pba.base < table_end)
    {
        table = {};
        pba = {};
        failure = msix_capability_error::table_pba_overlap;
        return false;
    }
    return true;
}

/// Decide whether a full BAR mapping would expose the MSI-X message table to
/// a PCI function client. Malformed capability lists fail closed because the
/// table's owning BAR cannot be trusted or identified.
inline msix_bar_mapping_error validate_msix_bar_mapping(pci_config_read32 read_config, void *context,
                                                        u32 requested_bar) noexcept
{
    if (requested_bar >= 6)
        return msix_bar_mapping_error::invalid_bar_index;

    msix_capability capability{};
    msix_capability_error failure = msix_capability_error::none;
    if (find_msix_capability(read_config, context, capability, failure))
        return capability.table_bar == requested_bar ? msix_bar_mapping_error::table_bar_reserved
                                                     : msix_bar_mapping_error::none;

    if (failure == msix_capability_error::capability_not_found ||
        failure == msix_capability_error::capability_list_unavailable)
        return msix_bar_mapping_error::none;
    if (failure == msix_capability_error::config_reader_unavailable ||
        failure == msix_capability_error::config_read_failed)
        return msix_bar_mapping_error::config_read_failed;
    return msix_bar_mapping_error::invalid_capability;
}

/// Build the minimal kernel mapping for one MSI-X table after matching the
/// indexed BAR to its device `_CRS` window. The entire page envelope must be
/// writable and contained by one attested root memory window.
inline msix_table_mapping_error plan_msix_table_mapping(msix_region table, msix_bar_aperture bar, u32 resource_flags,
                                                        const memory_window *root_windows, u32 root_window_count,
                                                        msix_table_mapping_plan &result) noexcept
{
    result = {};
    if (table.length == 0 || table.base > ~u64{0} - table.length)
        return msix_table_mapping_error::invalid_table;
    if (bar.base == 0 || bar.length == 0 || bar.base > ~u64{0} - bar.length ||
        !valid_bar_aperture(bar.base, bar.length))
        return msix_table_mapping_error::invalid_aperture;
    if ((resource_flags & ~1u) != 0)
        return msix_table_mapping_error::invalid_resource_flags;
    if ((resource_flags & 1u) == 0)
        return msix_table_mapping_error::not_writable;
    if (root_windows == nullptr || root_window_count == 0 || root_window_count > max_msix_root_windows)
        return msix_table_mapping_error::invalid_root_windows;

    if (!bar_range_within_window(table.base, table.length, bar.base, bar.length))
        return msix_table_mapping_error::table_out_of_bounds;

    bool within_root = false;
    for (u32 index = 0; index < root_window_count; ++index)
    {
        if (bar_range_within_window(bar.base, bar.length, root_windows[index].base, root_windows[index].length))
        {
            within_root = true;
            break;
        }
    }
    if (!within_root)
        return msix_table_mapping_error::root_escape;

    bar_page_cover cover{};
    if (!make_bar_page_cover(table.base, table.length, cover) || (cover.end - cover.base) % bar_page_size != 0)
        return msix_table_mapping_error::invalid_table;
    result = {table, cover, (cover.end - cover.base) / bar_page_size, table.base - cover.base};
    return msix_table_mapping_error::none;
}

} // namespace pci
