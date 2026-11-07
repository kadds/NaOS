#pragma once

#include "kernel/arch/tss.hpp"

namespace arch::tss::detail
{

inline constexpr u32 io_bitmap_descriptor_limit = sizeof(tss_t) + io_bitmap_bytes;
inline constexpr u32 denied_io_map_base = io_bitmap_descriptor_limit + io_bitmap_guard_bytes;

inline void deny_all(tss_storage &storage) noexcept
{
    // A map base beyond the TSS descriptor limit denies every port without
    // rewriting 8 KiB on each switch to a thread with no direct-I/O lease.
    storage.io_bitmap[io_bitmap_bytes] = 0xff;
    storage.tss.io_address = denied_io_map_base;
    storage.cached_io_range_count = 0;
}

inline bool cached_ranges_match(const tss_storage &storage, const io_range *ranges, u32 range_count) noexcept
{
    if (storage.tss.io_address != sizeof(tss_t) || storage.cached_io_range_count != range_count ||
        (range_count != 0 && ranges == nullptr))
        return false;
    for (u32 index = 0; index < range_count; index++)
    {
        if (storage.cached_io_ranges[index].first_port != ranges[index].first_port ||
            storage.cached_io_ranges[index].port_count != ranges[index].port_count)
            return false;
    }
    return true;
}

inline void allow_ranges(tss_storage &storage, const io_range *ranges, u32 range_count) noexcept
{
    if (range_count > max_direct_io_ranges || (range_count != 0 && ranges == nullptr) ||
        cached_ranges_match(storage, ranges, range_count))
        return;
    __builtin_memset(storage.io_bitmap, 0xff, sizeof(storage.io_bitmap));
    for (u32 range_index = 0; range_index < range_count; range_index++)
    {
        for (u32 offset = 0; offset < ranges[range_index].port_count; offset++)
        {
            const u32 port = static_cast<u32>(ranges[range_index].first_port) + offset;
            storage.io_bitmap[port / 8] &= static_cast<u8>(~(1u << (port % 8)));
        }
    }
    storage.tss.io_address = sizeof(tss_t);
    storage.cached_io_range_count = range_count;
    for (u32 index = 0; index < range_count; index++)
        storage.cached_io_ranges[index] = ranges[index];
}

} // namespace arch::tss::detail
