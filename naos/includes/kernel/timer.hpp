#pragma once
#include "clock.hpp"
#include "common.hpp"
#include "kernel/types.hpp"
namespace timer
{

typedef void (*watcher_func)(timeclock::microsecond_t expires, u64 user_data);
void init();

///
/// \brief get current time
///
/// \note the time of each CPU is not synchronized
timeclock::microsecond_t get_high_resolution_time();

void busywait(timeclock::microsecond_t duration);

void add_watcher(u64 expires_delta_time, watcher_func func, u64 user_data);

bool add_time_point_watcher(u64 expires_time_point, watcher_func func, u64 user_data);

void remove_watcher(watcher_func func);

} // namespace timer
