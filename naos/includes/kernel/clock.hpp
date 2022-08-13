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

} // namespace clock
