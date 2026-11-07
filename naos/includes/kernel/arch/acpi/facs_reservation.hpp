#pragma once

#include "kernel/physical_resource.hpp"

namespace arch::acpi_boot
{
inline constexpr size_t max_facs_state_ranges = 16;

struct facs_page_envelope
{
    u64 base = 0;
    u64 end = 0;
};

inline bool get_facs_page_envelope(u64 base, u64 length, u64 page_size, facs_page_envelope &result)
{
    constexpr u64 max_address = ~u64(0);
    if (base == 0 || length == 0 || page_size == 0 || (page_size & (page_size - 1)) != 0 || base > max_address - length)
        return false;

    const u64 mask = page_size - 1;
    const u64 page_base = base & ~mask;
    const u64 end = base + length;
    const u64 remainder = end & mask;
    u64 page_end = end;
    if (remainder != 0)
    {
        const u64 padding = page_size - remainder;
        if (end > max_address - padding)
            return false;
        page_end += padding;
    }
    if (page_end <= page_base)
        return false;

    result = {page_base, page_end};
    return true;
}

/// Reserve the complete page envelope of mutable FACS state in the kernel's
/// physical-resource ledger. FACS is never exposed as a writable mapping;
/// reserving the envelope prevents unrelated direct or mediated leases from
/// aliasing its page.
inline bool reserve_facs_state_range(const physical_resource::root &authority, u64 base, u64 length, u64 page_size,
                                     u64 generation, physical_resource::lease &result)
{
    if (authority.resources() == nullptr)
        return false;
    facs_page_envelope envelope{};
    if (!get_facs_page_envelope(base, length, page_size, envelope))
        return false;

    return authority.resources()->reserve(
        authority, base, length, envelope.base, envelope.end, physical_resource::kind::firmware_state,
        physical_resource::cache::uncacheable, physical_resource::access::read, generation, result);
}

/// Reserve every enumerated FACS state range before entering the continuation
/// that creates user-facing ACPI capabilities. A failure rolls back all FACS
/// leases admitted so far and never invokes that continuation.
template <typename EnumerateRanges, typename InitializeAfterReservation>
bool reserve_facs_state_then(const physical_resource::root &authority, u64 page_size, u64 generation,
                             EnumerateRanges enumerate_ranges, InitializeAfterReservation initialize_after_reservation)
{
    struct lease_transaction
    {
        const physical_resource::root *authority;
        u64 lease_ids[max_facs_state_ranges]{};
        size_t count = 0;
        bool committed = false;

        ~lease_transaction()
        {
            if (committed || authority->resources() == nullptr)
                return;
            while (count != 0)
            {
                (void)authority->resources()->revoke_kernel(lease_ids[--count]);
            }
        }
    } transaction{&authority};

    bool reservation_failed = false;
    const auto reserve_range = [&](u64 base, u64 length) {
        if (transaction.count == sizeof(transaction.lease_ids) / sizeof(transaction.lease_ids[0]))
        {
            reservation_failed = true;
            return false;
        }
        physical_resource::lease lease{};
        if (!reserve_facs_state_range(authority, base, length, page_size, generation, lease))
        {
            reservation_failed = true;
            return false;
        }
        transaction.lease_ids[transaction.count++] = lease.id;
        return true;
    };
    if (!enumerate_ranges(reserve_range) || reservation_failed)
        return false;
    if (!initialize_after_reservation())
        return false;
    transaction.committed = true;
    return true;
}

} // namespace arch::acpi_boot
