#pragma once

#include "kernel/errno.hpp"
#include "kernel/types.hpp"

namespace acpi
{

inline constexpr u32 facs_global_lock_pending = 1u << 0;
inline constexpr u32 facs_global_lock_owned = 1u << 1;
inline constexpr u32 pm1_global_lock_status = 1u << 5;
inline constexpr u32 pm1_global_lock_enable = 1u << 5;
inline constexpr u32 pm1_global_lock_release = 1u << 2;

constexpr u64 next_pm1_global_lock_release(u64 current) noexcept { return current | pm1_global_lock_release; }

template <typename Guard, typename Lock, typename Operation>
decltype(auto) with_pm1_control_update_lock(Lock &lock, Operation &&operation)
{
    Guard guard(lock);
    return operation();
}

enum class global_lock_owner_claim_decision : u8
{
    claim,
    already_owned,
    contended,
    invalid_state,
};

/// The AML interpreter owns recursive _GL_ depth and calls the host only for
/// its outermost acquisition. The kernel therefore tracks one lease and
/// rejects a duplicate request from the current owner.
constexpr global_lock_owner_claim_decision classify_global_lock_owner_claim(u64 owner_pid, u64 owner_tid,
                                                                            bool acquire_pending, bool hardware_owned,
                                                                            u64 requester_pid,
                                                                            u64 requester_tid) noexcept
{
    if (requester_pid == 0 || requester_tid == 0)
        return global_lock_owner_claim_decision::invalid_state;

    if (owner_pid == 0)
    {
        return owner_tid == 0 && !acquire_pending && !hardware_owned ? global_lock_owner_claim_decision::claim
                                                                     : global_lock_owner_claim_decision::invalid_state;
    }

    if (owner_tid == 0 || (acquire_pending && hardware_owned))
        return global_lock_owner_claim_decision::invalid_state;

    return owner_pid == requester_pid && owner_tid == requester_tid ? global_lock_owner_claim_decision::already_owned
                                                                    : global_lock_owner_claim_decision::contended;
}

enum class global_lock_arbitration_path : u8
{
    software,
    facs_event,
    facs_poll,
    fail_closed,
};

/// Keep FACS arbitration whenever the shared firmware lock exists. If its SCI
/// notification is unavailable, callers must poll the FACS word instead of
/// silently replacing OS/firmware mutual exclusion with a local mutex.
constexpr global_lock_arbitration_path select_global_lock_arbitration_path(bool has_facs, bool hardware_reduced,
                                                                           bool event_available,
                                                                           bool event_lost) noexcept
{
    // Software Global Lock serialization does not depend on the SCI. When
    // firmware omits the shared FACS lock, a failed SCI path must not disable
    // the local PID/TID owner lock and its wait queue.
    if (!has_facs || hardware_reduced)
        return global_lock_arbitration_path::software;
    if (event_lost)
        return global_lock_arbitration_path::fail_closed;
    return event_available ? global_lock_arbitration_path::facs_event : global_lock_arbitration_path::facs_poll;
}

/// A notification path is lost only if it was previously confirmed available.
constexpr bool global_lock_event_lost_after_failure(bool was_available, bool already_lost) noexcept
{
    return was_available || already_lost;
}

enum class global_lock_operation_result : u8
{
    success,
    access_denied,
    resource_revoked,
    stale_generation,
    unsupported,
    timed_out,
    not_acquired,
    io_error,
};

constexpr int global_lock_operation_errno(global_lock_operation_result result) noexcept
{
    switch (result)
    {
        case global_lock_operation_result::success:
            return 0;
        case global_lock_operation_result::access_denied:
            return EACCES;
        case global_lock_operation_result::resource_revoked:
            return EIDRM;
        case global_lock_operation_result::stale_generation:
            return ESTALE;
        case global_lock_operation_result::unsupported:
            return ENOTSUP;
        case global_lock_operation_result::timed_out:
            return ETIMEDOUT;
        case global_lock_operation_result::not_acquired:
            return EPERM;
        case global_lock_operation_result::io_error:
            return EIO;
    }
    return EIO;
}

struct global_lock_acquire_update
{
    u32 value = 0;
    bool acquired = false;
};

/// Compute the ACPI FACS Global Lock compare-exchange value.
///
/// The current owner is preserved while setting Pending. An unowned lock is
/// claimed atomically after clearing a stale Pending bit. Reserved bits are
/// preserved in both cases.
constexpr global_lock_acquire_update next_global_lock_acquire(u32 current) noexcept
{
    u32 next = current & ~facs_global_lock_pending;
    next |= facs_global_lock_owned;
    if ((current & facs_global_lock_owned) != 0)
        next |= facs_global_lock_pending;
    return {next, (next & (facs_global_lock_pending | facs_global_lock_owned)) == facs_global_lock_owned};
}

struct global_lock_release_update
{
    u32 value = 0;
    bool owned = false;
    bool notify_firmware = false;
};

/// Compute the ACPI FACS Global Lock release compare-exchange value.
constexpr global_lock_release_update next_global_lock_release(u32 current) noexcept
{
    const bool owned = (current & facs_global_lock_owned) != 0;
    return {owned ? current & ~(facs_global_lock_pending | facs_global_lock_owned) : current, owned,
            owned && (current & facs_global_lock_pending) != 0};
}

} // namespace acpi
