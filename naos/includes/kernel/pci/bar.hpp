#pragma once

#include "kernel/types.hpp"

namespace pci
{
inline constexpr u8 bar_register_count = 6;
inline constexpr u64 bar_page_size = 4096;

enum class bar_decode_error : u8
{
    none,
    invalid_bar_count,
    invalid_index,
    io_space_unsupported,
    memory_below_1mb_unsupported,
    reserved_memory_type,
    missing_upper_register,
    unassigned,
};

struct bar_descriptor
{
    u64 base = 0;
    u8 first_register = 0;
    u8 register_count = 0;
    bool prefetchable = false;
};

struct bar_page_cover
{
    u64 base = 0;
    u64 end = 0;
};

enum class root_window_error : u8
{
    none,
    unsupported_resource_type,
    invalid_descriptor,
    invalid_range,
    invalid_translation,
};

struct memory_window
{
    u64 base = 0;
    u64 length = 0;
};

/// Convert a validated ACPI memory or I/O address-space descriptor into its
/// translated system range. This checks descriptor arithmetic only; callers
/// must still obtain the descriptor from the authorized platform-root authority.
inline root_window_error decode_root_address_window(u32 resource_type, u32 descriptor_revision, u32 general_flags,
                                                    u64 granularity, u64 minimum, u64 maximum, u64 translation_offset,
                                                    u64 length, memory_window &result) noexcept
{
    result = {};
    if (resource_type > 1)
        return root_window_error::unsupported_resource_type;
    if (descriptor_revision > 1 || (general_flags & 0xf0u) != 0 ||
        (granularity != ~u64{0} && ((granularity + 1) == 0 || ((granularity + 1) & granularity) != 0)))
        return root_window_error::invalid_descriptor;
    if (length == 0 || minimum > maximum || minimum > ~u64{0} - (length - 1))
        return root_window_error::invalid_range;

    const u64 bus_end = minimum + length - 1;
    if (bus_end > maximum)
        return root_window_error::invalid_range;

    u64 translated_base = 0;
    if ((translation_offset & (u64{1} << 63)) == 0)
    {
        if (minimum > ~u64{0} - translation_offset || bus_end > ~u64{0} - translation_offset)
            return root_window_error::invalid_translation;
        translated_base = minimum + translation_offset;
    }
    else
    {
        const u64 magnitude = ~translation_offset + 1;
        if (minimum < magnitude || bus_end < magnitude)
            return root_window_error::invalid_translation;
        translated_base = minimum - magnitude;
    }
    if (translated_base > ~u64{0} - length)
        return root_window_error::invalid_translation;

    result = {translated_base, length};
    return root_window_error::none;
}

/// Convert a validated ACPI memory descriptor. I/O descriptors are rejected
/// because this range is intended for PCI BAR/MMIO policy.
inline root_window_error decode_root_memory_window(u32 resource_type, u32 descriptor_revision, u32 general_flags,
                                                   u64 granularity, u64 minimum, u64 maximum, u64 translation_offset,
                                                   u64 length, memory_window &result) noexcept
{
    if (resource_type != 0)
    {
        result = {};
        return root_window_error::unsupported_resource_type;
    }
    return decode_root_address_window(resource_type, descriptor_revision, general_flags, granularity, minimum, maximum,
                                      translation_offset, length, result);
}

/// Decode an already assigned memory BAR without probing by writing all ones.
/// `upper_register_available` must be true only after the adjacent BAR dword
/// has been read for a 64-bit memory BAR.
inline bar_decode_error decode_bar(u8 count, u8 index, u32 low, bool upper_register_available, u32 high,
                                   bar_descriptor &result) noexcept
{
    result = {};
    if (count == 0 || count > bar_register_count)
        return bar_decode_error::invalid_bar_count;
    if (index >= count)
        return bar_decode_error::invalid_index;

    constexpr u32 io_space_bit = 1u;
    constexpr u32 memory_type_mask = 3u << 1;
    constexpr u32 memory_type_below_1mb = 1u << 1;
    constexpr u32 memory_type_64 = 2u << 1;
    constexpr u32 memory_address_mask = 0xfffffff0u;
    constexpr u32 prefetchable_bit = 1u << 3;

    if ((low & io_space_bit) != 0)
        return bar_decode_error::io_space_unsupported;

    const u32 memory_type = low & memory_type_mask;
    if (memory_type == memory_type_below_1mb)
        return bar_decode_error::memory_below_1mb_unsupported;
    if (memory_type != 0 && memory_type != memory_type_64)
        return bar_decode_error::reserved_memory_type;

    const bool is_64_bit = memory_type == memory_type_64;
    if (is_64_bit && (index + 1 >= count || !upper_register_available))
        return bar_decode_error::missing_upper_register;

    const u64 base = (is_64_bit ? (static_cast<u64>(high) << 32) : 0) | (low & memory_address_mask);
    if (base == 0)
        return bar_decode_error::unassigned;

    result = {base, index, static_cast<u8>(is_64_bit ? 2 : 1), (low & prefetchable_bit) != 0};
    return bar_decode_error::none;
}

inline bool make_bar_page_cover(u64 base, u64 length, bar_page_cover &result) noexcept
{
    result = {};
    const u64 max_value = ~u64{0};
    if (length == 0 || base > max_value - length)
        return false;

    const u64 end = base + length;
    const u64 page_mask = bar_page_size - 1;
    const u64 remainder = end & page_mask;
    u64 page_end = end;
    if (remainder != 0)
    {
        const u64 padding = bar_page_size - remainder;
        if (end > max_value - padding)
            return false;
        page_end += padding;
    }

    const u64 page_base = base & ~page_mask;
    if (page_end <= page_base)
        return false;
    result = {page_base, page_end};
    return true;
}

/// Conventional PCI BAR apertures have power-of-two size and their assigned
/// base is aligned to that size. This is checked against `_CRS`; no size probe
/// writes are required or permitted.
inline bool valid_bar_aperture(u64 base, u64 length) noexcept
{
    return length != 0 && (length & (length - 1)) == 0 && (base & (length - 1)) == 0;
}

/// A BAR must fit logically in one firmware root window. Its complete page
/// envelope must also fit in that window's page envelope before direct mapping.
inline bool bar_range_within_window(u64 base, u64 length, u64 window_base, u64 window_length) noexcept
{
    const u64 max_value = ~u64{0};
    if (length == 0 || window_length == 0 || base > max_value - length || window_base > max_value - window_length)
        return false;

    const u64 end = base + length;
    const u64 window_end = window_base + window_length;
    if (base < window_base || end > window_end)
        return false;

    bar_page_cover bar_cover{};
    bar_page_cover window_cover{};
    if (!make_bar_page_cover(base, length, bar_cover) || !make_bar_page_cover(window_base, window_length, window_cover))
        return false;
    return bar_cover.base >= window_cover.base && bar_cover.end <= window_cover.end;
}
} // namespace pci
