#pragma once

#include "kernel/arch/klib.hpp"
#include "kernel/arch/regs.hpp"
#include "naos/abi.h"

namespace naos::usercopy
{

// These copies use a per-thread recovery continuation.  The page-fault
// dispatcher may redirect a kernel-mode fault back to the continuation after
// a lazy VM fault handler declines the address.  Callers therefore observe a
// status instead of a kernel panic, even when a user mapping disappears
// between valid_range() and the actual copy.
na_status_t copy_from(void *destination, u64 source, u64 size);
na_status_t copy_to(u64 destination, const void *source, u64 size);

bool recover_page_fault(regs_t *regs);

inline bool valid_range(u64 address, u64 size)
{
    return size == 0 ? address == 0 : is_user_space_range(reinterpret_cast<const void *>(address), size);
}

inline bool valid_struct_size(u32 actual, u64 expected) { return actual >= expected; }

// The IPC ABI uses a size-prefixed frame. Read the prefix before copying the
// complete known version so a short mapping is reported as an ABI error rather
// than being faulted while probing bytes that the caller did not provide.
template <typename T> na_status_t copy_versioned(T &destination, const T *source)
{
    if (source == nullptr || !is_user_space_range(reinterpret_cast<const void *>(source), sizeof(u32)))
        return NA_STATUS_FAULT;

    u32 declared_size = 0;
    auto status = copy_from(&declared_size, reinterpret_cast<u64>(source), sizeof(declared_size));
    if (status != NA_STATUS_OK)
        return status;
    if (!valid_struct_size(declared_size, sizeof(T)))
        return NA_STATUS_INVALID_ARGUMENT;

    destination = {};
    return copy_from(&destination, reinterpret_cast<u64>(source), sizeof(destination));
}

inline bool ranges_overlap(u64 first, u64 first_size, u64 second, u64 second_size)
{
    if (first_size == 0 || second_size == 0)
        return false;
    // Callers normally validate ranges before checking overlap. Treat an
    // invalid range as overlapping so a future caller cannot accidentally
    // turn an invalid pointer pair into an accepted alias.
    if (!valid_range(first, first_size) || !valid_range(second, second_size))
        return true;
    return first < second ? second - first < first_size : first - second < second_size;
}

} // namespace naos::usercopy
