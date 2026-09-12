#pragma once
#include "../clock/clock_event.hpp"
#include "../clock/clock_source.hpp"
#include "kernel/common.hpp"

namespace arch::TSC
{

class clock_source : public ::timeclock::clock_source
{
  private:
    u64 tsc_tick_second_ = 0;
    u64 begin_tsc_ = 0;
    bool builtin_freq_ = false;

  public:
    clock_source()
        : ::timeclock::clock_source("tsc")
    {
    }
    void init() override;
    void destroy() override;
    void calibrate(::timeclock::clock_source *cs) override;
    u64 calibrate_tsc(::timeclock::clock_source *cs);
    u64 current() override;
};

clock_source *make_clock();

} // namespace arch::TSC
