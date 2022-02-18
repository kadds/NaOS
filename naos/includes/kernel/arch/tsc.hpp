#pragma once
#include "../clock/clock_event.hpp"
#include "../clock/clock_source.hpp"
#include "common.hpp"

namespace arch::TSC
{

class clock_event : public ::timeclock::clock_event
{
  private:
  public:
    clock_event()
        : ::timeclock::clock_event(0){};

    void init(u64 HZ) override{};
    void destroy() override{};

    void suspend() override {}
    void resume() override {}

    void wait_next_tick() override{};
    bool is_valid() override { return true; }
};

class clock_source : public ::timeclock::clock_source
{

  private:
    u64 tsc_tick_per_microsecond;
    u64 fill_tsc;

  public:
    void init() override;
    void destroy() override;
    void calibrate(::timeclock::clock_source *cs) override;
    u64 calibrate_tsc(::timeclock::clock_source *cs);
    u64 current() override;
};

clock_source *make_clock();

} // namespace arch::TSC
