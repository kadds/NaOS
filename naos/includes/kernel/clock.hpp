#pragma once
#include "common.hpp"
#include "time.hpp"
namespace clock
{

// start 1970

void init();
void start_tick();

time::microsecond_t get_current_clock();
time::microsecond_t get_startup_clock();

void time2time_t(time::microsecond_t ms, time::time_t *t);
time::microsecond_t time_t2time(const time::time_t *t);

} // namespace clock
