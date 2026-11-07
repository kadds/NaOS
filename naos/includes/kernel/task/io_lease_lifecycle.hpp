#pragma once

#include "kernel/types.hpp"

namespace task::detail
{

constexpr bool io_lease_generation_is_current(u64 current, u64 requested) noexcept
{
    return current != 0 && current == requested;
}

/// Clear the thread's direct-I/O set, revoke its active CPU bitmap, and mint a
/// new generation so queued requests from the old image cannot reinstall it.
template <typename Ranges, typename RevokeBitmap>
u64 revoke_direct_io_lease_state(Ranges &ranges, u32 &range_count, u64 generation, RevokeBitmap revoke_bitmap) noexcept
{
    for (auto &range : ranges)
        range = {};
    range_count = 0;
    revoke_bitmap();
    return generation == 0 || generation == ~u64(0) ? 0 : generation + 1;
}

} // namespace task::detail
