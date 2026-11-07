#pragma once

#include "kernel/types.hpp"

namespace naos::data_plane
{
enum class dma_direction : u8
{
    device_reads,
    device_writes,
    device_reads_and_writes,
};

enum class dma_page_policy_error : u8
{
    none,
    invalid_range,
    partial_page,
    not_page_backed,
    borrowed_frames,
    external_storage,
    page_not_ready,
    read_only,
    invalid_direction,
};

struct dma_page_policy_input
{
    u64 size = 0;
    u64 offset = 0;
    bool page_backed = false;
    bool owns_frames = false;
    bool external_view = false;
    bool direct_physical = false;
    bool page_ready = false;
    bool read_only = false;
};

/// Admit exactly one complete, stable MemoryObject frame for device access.
/// The caller must retain the MemoryObject capability for the entire DMA map.
constexpr dma_page_policy_error validate_dma_page_policy(const dma_page_policy_input &input, u64 page_size,
                                                         dma_direction direction) noexcept
{
    if (page_size == 0 || (page_size & (page_size - 1)) != 0 || input.size == 0 ||
        (input.offset & (page_size - 1)) != 0 || input.offset >= input.size)
        return dma_page_policy_error::invalid_range;
    if (page_size > input.size - input.offset)
        return dma_page_policy_error::partial_page;
    if (!input.page_backed)
        return dma_page_policy_error::not_page_backed;
    if (input.external_view || input.direct_physical)
        return dma_page_policy_error::external_storage;
    if (!input.owns_frames)
        return dma_page_policy_error::borrowed_frames;
    if (!input.page_ready)
        return dma_page_policy_error::page_not_ready;
    if (direction != dma_direction::device_reads && direction != dma_direction::device_writes &&
        direction != dma_direction::device_reads_and_writes)
        return dma_page_policy_error::invalid_direction;
    if ((direction == dma_direction::device_writes || direction == dma_direction::device_reads_and_writes) &&
        input.read_only)
        return dma_page_policy_error::read_only;
    return dma_page_policy_error::none;
}
} // namespace naos::data_plane
