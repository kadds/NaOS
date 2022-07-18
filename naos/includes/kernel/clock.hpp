#pragma once
#include "common.hpp"
#include "kernel/types.hpp"
#include "time.hpp"
namespace timeclock
{

// start 1970

void init();
void start_tick();

microsecond_t get_current_clock();
microsecond_t get_startup_clock();
time to_time(microsecond_t t);

void time2time_t(microsecond_t ms, time_t *t);
microsecond_t time_t2time(const time_t *t);

} // namespace clock
