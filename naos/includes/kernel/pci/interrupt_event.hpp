#pragma once

#include "kernel/types.hpp"

namespace pci
{
enum class interrupt_event_ack : u8
{
    acknowledged,
    not_pending,
    stale_sequence,
};

/// Lock-free-free event state; the owning InterruptSource serializes access
/// with its state lock. Repeated MSI arrivals are coalesced into one readable
/// event while advancing the sequence so a stale acknowledgement cannot clear
/// a newer arrival.
class interrupt_event_state
{
  public:
    bool interrupt() noexcept
    {
        sequence_++;
        if (sequence_ == 0)
            sequence_ = 1;
        const bool became_readable = !pending_;
        pending_ = true;
        return became_readable;
    }

    interrupt_event_ack acknowledge(u64 sequence) noexcept
    {
        if (!pending_)
            return interrupt_event_ack::not_pending;
        if (sequence != sequence_)
            return interrupt_event_ack::stale_sequence;
        pending_ = false;
        return interrupt_event_ack::acknowledged;
    }

    bool readable() const noexcept { return pending_; }
    u64 sequence() const noexcept { return sequence_; }

  private:
    u64 sequence_ = 0;
    bool pending_ = false;
};
} // namespace pci
