#pragma once
#include "../clock/clock_event.hpp"
#include "../clock/clock_source.hpp"
#include "common.hpp"

namespace arch::TSC
{

class clock_source : public ::timeclock::clock_source
{
  private:
    u64 tsc_tick_second_;
    u64 begin_tsc_;
    bool builtin_freq_;

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
