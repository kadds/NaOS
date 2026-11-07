#pragma once

#include "kernel/errno.hpp"
#include "kernel/types.hpp"

namespace acpi
{
enum class sci_interrupt_action : u8
{
    suppressed,
    masked_and_signaled,
    coalesced_and_signaled,
    degraded,
};

enum class sci_ack_result : u8
{
    rearmed,
    resignaled,
    stale_sequence,
    conflict,
    degraded,
};

enum class sci_drain_result : u8
{
    ready,
    would_block,
    stale_sequence,
    resource_revoked,
};

enum class sci_operation_result : u8
{
    success,
    access_denied,
    resource_revoked,
    stale_generation,
    invalid_argument,
    not_supported,
    not_ready,
    conflict,
    would_block,
    resource_exhausted,
    io_error,
};

constexpr int sci_operation_errno(sci_operation_result result) noexcept
{
    switch (result)
    {
        case sci_operation_result::success:
            return 0;
        case sci_operation_result::access_denied:
            return EACCES;
        case sci_operation_result::resource_revoked:
            return EIDRM;
        case sci_operation_result::stale_generation:
            return ESTALE;
        case sci_operation_result::invalid_argument:
            return EINVAL;
        case sci_operation_result::not_supported:
            return ENOTSUP;
        case sci_operation_result::not_ready:
            return ENXIO;
        case sci_operation_result::conflict:
            return EBUSY;
        case sci_operation_result::would_block:
            return EAGAIN;
        case sci_operation_result::resource_exhausted:
            return ENOMEM;
        case sci_operation_result::io_error:
            return EIO;
    }
    return EIO;
}

enum class sci_event_state : u8
{
    masked,
    unmasked,
    degraded,
};

inline constexpr bool take_sci_sequence(u64 &next_sequence, u64 &sequence) noexcept
{
    if (next_sequence == 0)
        return false;
    sequence = next_sequence;
    next_sequence = next_sequence == ~u64{0} ? 0 : next_sequence + 1;
    return true;
}

/// Caller-serialized SCI event policy; it contains no synchronization.
/// Hardware access and IRQ masking remain in the fixed-hardware adapter.
class sci_event_lifecycle
{
  public:
    sci_event_lifecycle() = default;
    explicit sci_event_lifecycle(u64 next_sequence)
        : next_sequence_(next_sequence)
    {
    }

    bool claim_source()
    {
        if (claimed_ || state_ == sci_event_state::degraded)
            return false;
        claimed_ = true;
        state_ = sci_event_state::unmasked;
        sequence_ = 0;
        event_ready_ = false;
        pending_ = false;
        lost_ = false;
        return true;
    }

    sci_interrupt_action interrupt()
    {
        if (!claimed_ || state_ == sci_event_state::degraded)
            return sci_interrupt_action::suppressed;
        if (state_ == sci_event_state::masked)
        {
            lost_ = lost_ || pending_;
            event_ready_ = true;
            pending_ = true;
            return sci_interrupt_action::coalesced_and_signaled;
        }

        state_ = sci_event_state::masked;
        if (!take_sci_sequence(next_sequence_, sequence_))
        {
            degrade();
            return sci_interrupt_action::degraded;
        }
        event_ready_ = true;
        lost_ = false;
        return sci_interrupt_action::masked_and_signaled;
    }

    sci_drain_result begin_drain(u64 sequence) const
    {
        if (!claimed_ || state_ == sci_event_state::degraded)
            return sci_drain_result::resource_revoked;
        if (sequence_ != sequence)
            return sci_drain_result::stale_sequence;
        return state_ == sci_event_state::masked ? sci_drain_result::ready : sci_drain_result::would_block;
    }

    sci_drain_result finish_drain(u64 sequence, u32 &flags)
    {
        const auto result = begin_drain(sequence);
        if (result != sci_drain_result::ready)
            return result;
        flags = (pending_ ? 1u : 0u) | (lost_ ? 2u : 0u);
        pending_ = false;
        event_ready_ = false;
        return sci_drain_result::ready;
    }

    sci_ack_result acknowledge(u64 sequence, bool pending_after_drain, u32 &state, u32 &flags)
    {
        if (!claimed_ || state_ == sci_event_state::degraded)
            return sci_ack_result::degraded;
        if (sequence_ != sequence)
        {
            degrade();
            state = state_value();
            flags = 1u;
            return sci_ack_result::stale_sequence;
        }
        if (state_ != sci_event_state::masked)
        {
            degrade();
            state = state_value();
            flags = 1u;
            return sci_ack_result::conflict;
        }

        if (pending_after_drain)
        {
            u64 next_sequence = 0;
            if (!take_sci_sequence(next_sequence_, next_sequence))
            {
                degrade();
                state = state_value();
                flags = 1u;
                return sci_ack_result::degraded;
            }
            sequence_ = next_sequence;
            event_ready_ = true;
            pending_ = false;
            state = 1;
            flags = (lost_ ? 1u : 0u) | 2u;
            return sci_ack_result::resignaled;
        }

        state_ = sci_event_state::unmasked;
        event_ready_ = false;
        pending_ = false;
        flags = lost_ ? 1u : 0u;
        lost_ = false;
        state = 0;
        return sci_ack_result::rearmed;
    }

    void degrade()
    {
        state_ = sci_event_state::degraded;
        event_ready_ = false;
        pending_ = false;
        lost_ = true;
    }

    void degrade_drain() { state_ = sci_event_state::degraded; }
    void rearm_failed() { degrade(); }
    void mask_for_transition()
    {
        if (claimed_ && state_ != sci_event_state::degraded)
            state_ = sci_event_state::masked;
    }

    bool event_ready() const { return event_ready_; }
    bool pending() const { return pending_; }
    bool readable() const { return state_ == sci_event_state::degraded || event_ready_; }
    bool claimed() const { return claimed_; }
    u64 sequence() const { return sequence_; }
    sci_event_state state() const { return state_; }
    bool fixed_transition_ready() const
    {
        if (state_ == sci_event_state::degraded)
            return true; // commit must successfully mask the hardware source before writing registers.
        if (!claimed_)
            return state_ == sci_event_state::masked;
        return state_ == sci_event_state::unmasked && !pending_;
    }

    u32 state_value() const
    {
        switch (state_)
        {
            case sci_event_state::unmasked:
                return 0;
            case sci_event_state::masked:
                return 1;
            case sci_event_state::degraded:
                return 2;
        }
        return 2;
    }

  private:
    sci_event_state state_ = sci_event_state::masked;
    bool claimed_ = false;
    bool event_ready_ = false;
    bool pending_ = false;
    bool lost_ = false;
    u64 sequence_ = 0;
    u64 next_sequence_ = 1;
};
} // namespace acpi
