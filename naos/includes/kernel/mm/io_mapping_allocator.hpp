#pragma once

#include "kernel/types.hpp"

#include <atomic>

namespace memory::detail
{
/// Atomically reserve a non-overlapping aligned range from a virtual-address
/// cursor. The expected value passed to compare_exchange must remain the
/// observed cursor, not the aligned allocation base.
inline u64 reserve_io_mapping_address(std::atomic_uint64_t &cursor, u64 bytes, u64 alignment) noexcept
{
    if (bytes == 0 || alignment == 0 || (alignment & (alignment - 1)) != 0)
        return 0;

    constexpr u64 max_address = ~u64(0);
    const u64 alignment_mask = alignment - 1;
    u64 current = cursor.load();
    for (;;)
    {
        if (current > max_address - alignment_mask)
            return 0;
        const u64 base = (current + alignment_mask) & ~alignment_mask;
        if (base == 0 || bytes > max_address - base)
            return 0;

        const u64 next = base + bytes;
        if (cursor.compare_exchange_weak(current, next))
            return base;
    }
}
} // namespace memory::detail
