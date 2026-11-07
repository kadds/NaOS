#pragma once

#include "kernel/types.hpp"

namespace iommu::amd_vi
{
enum class dma_access : u8
{
    read = 1,
    write = 2,
    read_write = 3,
};

inline constexpr u64 page_size = 4096;
inline constexpr u64 page_address_mask = 0x000ffffffffff000ull;
inline constexpr u8 default_page_table_levels = 4;

/// Encode an AMD I/O page-translation entry for a 4 KiB leaf. The entry uses
/// the AMD I/O page-table PR/IR/IW/NextLevel fields from 48882 §2.2.3, not the
/// superficially similar Intel VT-d second-level entry format.
constexpr bool encode_page_translation_entry(u64 physical_address, dma_access access, u64 &entry) noexcept
{
    if ((physical_address & (page_size - 1)) != 0 || (physical_address & ~page_address_mask) != 0)
        return false;
    const u8 permissions = static_cast<u8>(access);
    if (permissions == 0 || (permissions & ~u8{3}) != 0)
        return false;

    constexpr u64 present = 1ull << 0;
    constexpr u64 read = 1ull << 61;
    constexpr u64 write = 1ull << 62;
    const u64 candidate =
        physical_address | present | ((permissions & 1) != 0 ? read : 0) | ((permissions & 2) != 0 ? write : 0);
    entry = candidate;
    return true;
}

/// Encode a four-level page-directory link. AMD's NextLevel field names the
/// child lookup level; a zero/7 value denotes a translation leaf, so directory
/// links use only levels 1 through 3 for a Mode=4 hierarchy.
constexpr bool encode_page_directory_entry(u64 child_table_physical, u8 next_level, u64 &entry) noexcept
{
    if ((child_table_physical & (page_size - 1)) != 0 || (child_table_physical & ~page_address_mask) != 0 ||
        next_level == 0 || next_level >= default_page_table_levels)
        return false;

    constexpr u64 present = 1ull << 0;
    constexpr u64 read = 1ull << 61;
    constexpr u64 write = 1ull << 62;
    constexpr u64 next_level_shift = 9;
    const u64 candidate =
        child_table_physical | present | read | write | (static_cast<u64>(next_level) << next_level_shift);
    entry = candidate;
    return true;
}
} // namespace iommu::amd_vi
