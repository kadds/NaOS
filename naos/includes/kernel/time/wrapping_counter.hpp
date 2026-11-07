#pragma once

#include "kernel/types.hpp"
#include <atomic>

namespace timeclock
{

/// Extends a power-of-two wrapping hardware counter into a monotonic tick
/// count. Samples must be taken at intervals shorter than half the hardware
/// period; older concurrent samples are clamped to the latest published value.
class wrapping_counter
{
  public:
    void reset(u32 initial_count, u32 counter_mask) noexcept
    {
        initial_count_ = initial_count & counter_mask;
        counter_mask_ = counter_mask;
        modulus_ = static_cast<u64>(counter_mask) + 1;
        half_range_ = modulus_ / 2;
        extended_ticks_.store(0, std::memory_order_release);
    }

    u64 sample(u32 counter_value) noexcept
    {
        const auto raw_ticks = static_cast<u64>((counter_value - initial_count_) & counter_mask_);
        auto observed = extended_ticks_.load(std::memory_order_acquire);
        for (;;)
        {
            const auto previous_raw = observed & counter_mask_;
            const auto epoch = observed & ~static_cast<u64>(counter_mask_);
            u64 candidate = observed;

            if (raw_ticks < previous_raw)
            {
                // A small backwards step is an out-of-order CPU sample. A
                // larger one is the hardware counter wrapping forward.
                if (previous_raw - raw_ticks <= half_range_)
                    return observed;
                const auto maximum = ~u64(0);
                candidate = epoch > maximum - modulus_ || epoch + modulus_ > maximum - raw_ticks
                                ? maximum
                                : epoch + modulus_ + raw_ticks;
            }
            else
            {
                // A large forwards step can only be a stale sample from the
                // previous hardware epoch.
                if (raw_ticks - previous_raw > half_range_)
                    return observed;
                candidate = epoch + raw_ticks;
            }

            if (candidate <= observed)
                return observed;
            if (extended_ticks_.compare_exchange_weak(observed, candidate, std::memory_order_acq_rel,
                                                      std::memory_order_acquire))
                return candidate;
        }
    }

  private:
    u32 initial_count_ = 0;
    u32 counter_mask_ = 0;
    u64 modulus_ = 1;
    u64 half_range_ = 0;
    std::atomic<u64> extended_ticks_{0};
};

} // namespace timeclock
