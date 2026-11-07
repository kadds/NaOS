#pragma once

#include "kernel/mm/data_plane_dma.hpp"
#include "kernel/types.hpp"
#include <type_traits>
#include <utility>

namespace pci
{
enum class dma_mapping_state : u8
{
    free,
    pending,
    mapped,
    uncertain,
};

enum class dma_mapping_error : u8
{
    none,
    invalid_address,
    address_width_exceeded,
    already_present,
    capacity_exhausted,
    invalid_slot,
    invalid_transition,
    invalid_resource,
    not_found,
    bus_master_enabled,
    state_uncertain,
};

/// Fixed-capacity mapping ownership table. The caller serializes transactions
/// and keeps each Resource alive until release_after_unmap succeeds. Uncertain
/// entries deliberately retain their Resource so an IOTLB timeout cannot free
/// backing pages that hardware might still address.
template <typename Resource, u32 Capacity> class dma_mapping_ledger
{
  public:
    static_assert(Capacity > 0);

    struct record
    {
        u64 iova = 0;
        u64 physical_address = 0;
        naos::data_plane::dma_direction direction = naos::data_plane::dma_direction::device_reads;
        dma_mapping_state state = dma_mapping_state::free;
        Resource resource{};
    };

    dma_mapping_error reserve(u64 iova, u64 page_size, u8 address_width, u32 &slot) noexcept
    {
        slot = Capacity;
        if (page_size == 0 || (page_size & (page_size - 1)) != 0 || (iova & (page_size - 1)) != 0 || address_width == 0)
            return dma_mapping_error::invalid_address;
        if (iova > ~u64(0) - (page_size - 1))
            return dma_mapping_error::address_width_exceeded;

        if (address_width < 64)
        {
            const u64 maximum_address = (u64(1) << address_width) - 1;
            if (page_size - 1 > maximum_address || iova > maximum_address - (page_size - 1))
                return dma_mapping_error::address_width_exceeded;
        }

        u32 free_slot = Capacity;
        for (u32 index = 0; index < Capacity; ++index)
        {
            if (records_[index].state == dma_mapping_state::free)
            {
                if (free_slot == Capacity)
                    free_slot = index;
                continue;
            }
            if (records_[index].iova == iova)
                return dma_mapping_error::already_present;
        }
        if (free_slot == Capacity)
            return dma_mapping_error::capacity_exhausted;

        auto &entry = records_[free_slot];
        entry.iova = iova;
        entry.state = dma_mapping_state::pending;
        slot = free_slot;
        return dma_mapping_error::none;
    }

    template <typename R>
        requires std::is_same_v<std::remove_cvref_t<R>, Resource>
    dma_mapping_error commit(u32 slot, u64 physical_address, naos::data_plane::dma_direction direction,
                             R &&resource) noexcept
    {
        if (slot >= Capacity)
            return dma_mapping_error::invalid_slot;
        auto &entry = records_[slot];
        if (entry.state != dma_mapping_state::pending)
            return dma_mapping_error::invalid_transition;
        if (!static_cast<bool>(resource))
            return dma_mapping_error::invalid_resource;
        if (direction != naos::data_plane::dma_direction::device_reads &&
            direction != naos::data_plane::dma_direction::device_writes &&
            direction != naos::data_plane::dma_direction::device_reads_and_writes)
            return dma_mapping_error::invalid_transition;

        entry.physical_address = physical_address;
        entry.direction = direction;
        entry.resource = std::forward<R>(resource);
        entry.state = dma_mapping_state::mapped;
        return dma_mapping_error::none;
    }

    dma_mapping_error cancel(u32 slot) noexcept
    {
        if (slot >= Capacity)
            return dma_mapping_error::invalid_slot;
        auto &entry = records_[slot];
        if (entry.state != dma_mapping_state::pending)
            return dma_mapping_error::invalid_transition;
        clear(entry);
        return dma_mapping_error::none;
    }

    dma_mapping_error mark_uncertain(u64 iova) noexcept
    {
        auto *entry = find(iova);
        if (entry == nullptr)
            return dma_mapping_error::not_found;
        if (entry->state != dma_mapping_state::pending && entry->state != dma_mapping_state::mapped)
            return dma_mapping_error::invalid_transition;
        entry->state = dma_mapping_state::uncertain;
        return dma_mapping_error::none;
    }

    dma_mapping_error release_after_unmap(u64 iova, bool bus_master_enabled, Resource &released) noexcept
    {
        if (bus_master_enabled)
            return dma_mapping_error::bus_master_enabled;
        auto *entry = find(iova);
        if (entry == nullptr)
            return dma_mapping_error::not_found;
        if (entry->state == dma_mapping_state::uncertain)
            return dma_mapping_error::state_uncertain;
        if (entry->state != dma_mapping_state::mapped)
            return dma_mapping_error::invalid_transition;

        released = std::move(entry->resource);
        clear(*entry);
        return dma_mapping_error::none;
    }

    /// Drop a committed record only when the backend guarantees that a failed
    /// map did not install a translation. Uncertain backend outcomes must use
    /// mark_uncertain instead and retain the Resource.
    dma_mapping_error rollback_failed_map(u64 iova, Resource &released) noexcept
    {
        auto *entry = find(iova);
        if (entry == nullptr)
            return dma_mapping_error::not_found;
        if (entry->state != dma_mapping_state::mapped)
            return entry->state == dma_mapping_state::uncertain ? dma_mapping_error::state_uncertain
                                                                : dma_mapping_error::invalid_transition;
        released = std::move(entry->resource);
        clear(*entry);
        return dma_mapping_error::none;
    }

    record *find(u64 iova) noexcept
    {
        for (auto &entry : records_)
        {
            if (entry.state != dma_mapping_state::free && entry.iova == iova)
                return &entry;
        }
        return nullptr;
    }

    const record *find(u64 iova) const noexcept
    {
        for (const auto &entry : records_)
        {
            if (entry.state != dma_mapping_state::free && entry.iova == iova)
                return &entry;
        }
        return nullptr;
    }

    record *at(u32 slot) noexcept { return slot < Capacity ? &records_[slot] : nullptr; }
    const record *at(u32 slot) const noexcept { return slot < Capacity ? &records_[slot] : nullptr; }

    u32 size() const noexcept
    {
        u32 count = 0;
        for (const auto &entry : records_)
        {
            if (entry.state != dma_mapping_state::free)
                ++count;
        }
        return count;
    }

    static constexpr u32 capacity() noexcept { return Capacity; }

  private:
    static void clear(record &entry) noexcept
    {
        entry.resource = Resource{};
        entry.iova = 0;
        entry.physical_address = 0;
        entry.direction = naos::data_plane::dma_direction::device_reads;
        entry.state = dma_mapping_state::free;
    }

    record records_[Capacity]{};
};

// Keep the fixed KObject plus its mapping ledger below the kernel's 8 KiB
// allocation ceiling while still allowing nearly 1 MiB of DMA pages.
inline constexpr u32 max_dma_mappings_per_domain = 240;
} // namespace pci
