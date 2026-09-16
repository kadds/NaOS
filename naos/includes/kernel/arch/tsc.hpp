#pragma once

#include "../time/event_clock.hpp"
#include "kernel/common.hpp"

namespace arch::TSC
{

class clock final : public ::timeclock::event_clock
{
  public:
    explicit clock(u64 known_frequency_hz = 0) noexcept
        : tsc_ticks_per_second_(known_frequency_hz)
    {
    }
    ~clock() override = default;

    bool start_cpu() noexcept override;
    void stop_cpu() noexcept override {}
    timeclock::nanosecond_t now_ns() noexcept override;
    const char *name() const noexcept override { return "tsc"; }
    bool cross_cpu_monotonic() const noexcept override { return cross_cpu_monotonic_; }

    bool calibrate(timeclock::event_clock &reference) noexcept;
    u64 frequency_hz() const noexcept { return tsc_ticks_per_second_; }

  private:
    u64 tsc_ticks_per_second_ = 0;
    u64 begin_tsc_ = 0;
    bool cross_cpu_monotonic_ = false;
};

clock *make_event_clock(u64 known_frequency_hz = 0) noexcept;

} // namespace arch::TSC
