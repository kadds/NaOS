#pragma once

#include "../time/event_clock.hpp"
#include "../types.hpp"
#include "kernel/common.hpp"

namespace arch::device::HPET
{

class clock final : public ::timeclock::event_clock
{
  public:
    clock() noexcept = default;
    ~clock() override = default;

    bool start_cpu() noexcept override;
    void stop_cpu() noexcept override {}
    timeclock::nanosecond_t now_ns() noexcept override;
    const char *name() const noexcept override { return "hpet"; }
    bool cross_cpu_monotonic() const noexcept override { return true; }

  private:
    volatile u64 *base_ = nullptr;
    u64 frequency_hz_ = 0;
    u64 initial_counter_ = 0;
    bool started_ = false;
};

clock *make_event_clock() noexcept;

} // namespace arch::device::HPET
