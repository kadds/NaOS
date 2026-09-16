#pragma once

#include "kernel/time.hpp"
#include "kernel/time/unsigned_math.hpp"

namespace timeclock
{

class event_clock;

/// Result of submitting a deadline to an event source.
enum class arm_result : u8
{
    armed,
    invalid,
    unavailable,
};

/// A deadline is a request in the event_clock time domain.  The timer core
/// supplies the observation used to calculate the hardware delta; an event
/// source must not read or retain the clock itself.
struct deadline_request
{
    nanosecond_t now_ns = 0;
    nanosecond_t deadline_ns = 0;
    u64 generation = 0;

    constexpr bool is_valid() const noexcept { return generation != 0 && deadline_ns >= now_ns; }
    constexpr nanosecond_t delta_ns() const noexcept { return deadline_ns >= now_ns ? deadline_ns - now_ns : 0; }
};

/// A per-CPU producer of timer interrupts.  The source is deliberately
/// independent from event_clock: timer core owns the time-domain read and
/// re-evaluates the deadline after every interrupt.
class event_source
{
  public:
    struct deadline_ticks
    {
        u32 ticks = 1;
        bool saturated = false;
    };

    virtual ~event_source() = default;

    /// Calibrate hardware against a selected clock before start_cpu().  A
    /// source with a fixed frequency can keep the default implementation.
    virtual bool calibrate(event_clock &reference) noexcept
    {
        (void)reference;
        return true;
    }
    virtual bool start_cpu() noexcept = 0;
    virtual void stop_cpu() noexcept = 0;
    virtual arm_result arm(const deadline_request &request) noexcept = 0;
    virtual void cancel(u64 generation) noexcept = 0;
    virtual bool is_armed() const noexcept = 0;
    virtual u64 armed_generation() const noexcept = 0;
    virtual const char *name() const noexcept = 0;

    /// Convert a relative nanosecond deadline to a 32-bit down-counter value.
    /// The conversion rounds up so a timer never fires before its deadline.
    static constexpr deadline_ticks convert_deadline(nanosecond_t delta_ns, u64 frequency_hz,
                                                     u32 maximum_ticks) noexcept
    {
        if (frequency_hz == 0 || maximum_ticks == 0)
            return {1, true};

        constexpr u64 ns_per_second = 1'000'000'000;
        const u64 whole_seconds = delta_ns / ns_per_second;
        const u64 remainder_ns = delta_ns % ns_per_second;

        u64 whole_ticks = 0;
        u64 rounded_remainder = 0;
        if (!unsigned_math::try_mul_div_floor(whole_seconds, frequency_hz, 1, whole_ticks) ||
            !unsigned_math::try_mul_div_ceil(remainder_ns, frequency_hz, ns_per_second, rounded_remainder) ||
            rounded_remainder > static_cast<u64>(maximum_ticks) ||
            whole_ticks > static_cast<u64>(maximum_ticks) - rounded_remainder)
            return {maximum_ticks, true};

        const u64 ticks_total = whole_ticks + rounded_remainder;
        u64 ticks = ticks_total;
        if (ticks == 0)
            ticks = 1;
        return {static_cast<u32>(ticks), false};
    }
};

} // namespace timeclock
