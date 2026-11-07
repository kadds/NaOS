#pragma once

#include "kernel/types.hpp"

namespace acpi
{
struct sci_status_masks
{
    u64 pm1_event = 0;
    u64 pm1_event_b = 0;
    u64 gpe0 = 0;
    u64 gpe1 = 0;

    bool operator==(const sci_status_masks &) const = default;
};

enum class sci_status_clear_error : u8
{
    none,
    no_drain,
    stale_sequence,
    empty,
    unobserved_bits,
    already_cleared,
};

/// Tracks exactly which status bits were observed by one SCI drain and which
/// have since been cleared. The fixed-hardware adapter serializes this state
/// with the matching hardware writes and SCI sequence.
class sci_status_ledger
{
  public:
    bool record(u64 sequence, sci_status_masks observed) noexcept
    {
        if (sequence == 0)
            return false;
        if (valid_ && sequence_ == sequence)
        {
            observed_.pm1_event |= observed.pm1_event;
            observed_.pm1_event_b |= observed.pm1_event_b;
            observed_.gpe0 |= observed.gpe0;
            observed_.gpe1 |= observed.gpe1;
            remaining_.pm1_event |= observed.pm1_event;
            remaining_.pm1_event_b |= observed.pm1_event_b;
            remaining_.gpe0 |= observed.gpe0;
            remaining_.gpe1 |= observed.gpe1;
            return true;
        }
        sequence_ = sequence;
        observed_ = observed;
        remaining_ = observed;
        valid_ = true;
        return true;
    }

    sci_status_clear_error validate_clear(u64 sequence, sci_status_masks requested) const noexcept
    {
        if (!valid_)
            return sci_status_clear_error::no_drain;
        if (sequence != sequence_)
            return sci_status_clear_error::stale_sequence;
        if (empty(requested))
            return sci_status_clear_error::empty;
        if (has_unobserved_bits(requested, observed_))
            return sci_status_clear_error::unobserved_bits;
        if (has_unobserved_bits(requested, remaining_))
            return sci_status_clear_error::already_cleared;
        return sci_status_clear_error::none;
    }

    bool commit_clear(u64 sequence, sci_status_masks cleared) noexcept
    {
        if (validate_clear(sequence, cleared) != sci_status_clear_error::none)
            return false;
        remaining_.pm1_event &= ~cleared.pm1_event;
        remaining_.pm1_event_b &= ~cleared.pm1_event_b;
        remaining_.gpe0 &= ~cleared.gpe0;
        remaining_.gpe1 &= ~cleared.gpe1;
        return true;
    }

    sci_status_masks remaining(u64 sequence) const noexcept
    {
        return valid_ && sequence == sequence_ ? remaining_ : sci_status_masks{};
    }

    void invalidate() noexcept
    {
        valid_ = false;
        sequence_ = 0;
        observed_ = {};
        remaining_ = {};
    }

  private:
    static constexpr bool empty(sci_status_masks masks) noexcept
    {
        return masks.pm1_event == 0 && masks.pm1_event_b == 0 && masks.gpe0 == 0 && masks.gpe1 == 0;
    }

    static constexpr bool has_unobserved_bits(sci_status_masks requested, sci_status_masks allowed) noexcept
    {
        return (requested.pm1_event & ~allowed.pm1_event) != 0 || (requested.pm1_event_b & ~allowed.pm1_event_b) != 0 ||
               (requested.gpe0 & ~allowed.gpe0) != 0 || (requested.gpe1 & ~allowed.gpe1) != 0;
    }

    u64 sequence_ = 0;
    sci_status_masks observed_{};
    sci_status_masks remaining_{};
    bool valid_ = false;
};
} // namespace acpi
