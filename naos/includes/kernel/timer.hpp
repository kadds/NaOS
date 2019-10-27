#pragma once
#include "clock.hpp"
#include "common.hpp"
namespace timer
{

typedef void (*watcher_func)(time::microsecond_t expires, u64 user_data);
void init();

time::microsecond_t get_high_resolution_time();

void add_watcher(u64 expires_delta_time, watcher_func func, u64 user_data);

bool add_time_point_watcher(u64 expires_time_point, watcher_func func, u64 user_data);

void remove_watcher(watcher_func func);

} // namespace timer
