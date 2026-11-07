#pragma once

#include "kernel/types.hpp"

namespace pci
{
struct msi_route_function_key
{
    u16 segment = 0;
    u8 bus = 0;
    u8 device = 0;
    u8 function = 0;
};

enum class msi_route_registry_error : u8
{
    none,
    duplicate,
    full,
    not_found,
    already_quarantined,
};

/// Fixed-capacity, allocation-free uniqueness policy for one MSI route per
/// physical PCI function. The adapter serializes calls with its kernel lock.
template <u32 Capacity> class msi_route_registry
{
  public:
    msi_route_registry_error reserve(msi_route_function_key key) noexcept
    {
        u32 free_slot = Capacity;
        for (u32 index = 0; index < Capacity; ++index)
        {
            if (!entries_[index].used)
            {
                if (free_slot == Capacity)
                    free_slot = index;
                continue;
            }
            if (same_key(entries_[index].key, key))
                return entries_[index].quarantined ? msi_route_registry_error::already_quarantined
                                                   : msi_route_registry_error::duplicate;
        }
        if (free_slot == Capacity)
            return msi_route_registry_error::full;
        entries_[free_slot] = {key, true, false};
        return msi_route_registry_error::none;
    }

    msi_route_registry_error release(msi_route_function_key key) noexcept
    {
        auto *entry = find(key);
        if (entry == nullptr)
            return msi_route_registry_error::not_found;
        if (entry->quarantined)
            return msi_route_registry_error::already_quarantined;
        *entry = {};
        return msi_route_registry_error::none;
    }

    msi_route_registry_error quarantine(msi_route_function_key key) noexcept
    {
        auto *entry = find(key);
        if (entry == nullptr)
            return msi_route_registry_error::not_found;
        entry->quarantined = true;
        return msi_route_registry_error::none;
    }

  private:
    struct entry
    {
        msi_route_function_key key{};
        bool used = false;
        bool quarantined = false;
    };

    static bool same_key(msi_route_function_key left, msi_route_function_key right) noexcept
    {
        return left.segment == right.segment && left.bus == right.bus && left.device == right.device &&
               left.function == right.function;
    }

    entry *find(msi_route_function_key key) noexcept
    {
        for (auto &candidate : entries_)
        {
            if (candidate.used && same_key(candidate.key, key))
                return &candidate;
        }
        return nullptr;
    }

    entry entries_[Capacity]{};
};
} // namespace pci
