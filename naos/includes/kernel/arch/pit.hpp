#pragma once

#include "../time/event_clock.hpp"
#include "../types.hpp"
#include "kernel/common.hpp"

namespace arch::device::PIT
{

class clock final : public ::timeclock::event_clock
{
  public:
    clock() noexcept = default;
    ~clock() override = default;

    bool start_cpu() noexcept override;
    void stop_cpu() noexcept override;
    timeclock::nanosecond_t now_ns() noexcept override;
    const char *name() const noexcept override { return "pit"; }
    bool cross_cpu_monotonic() const noexcept override { return false; }

  private:
    u64 divisor_ = 0;
    u64 initial_count_ = 0;
    u64 last_tick_ = 0;
    u16 last_count_ = 0;
    bool started_ = false;
};

clock *make_event_clock() noexcept;
void disable_all() noexcept;

} // namespace arch::device::PIT
