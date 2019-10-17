#pragma once
#include "clock.hpp"
#include "common.hpp"
namespace timer
{

typedef bool (*watcher_func)(u64 pass, u64 user_data);
void init();

time::microsecond_t get_high_resolution_time();

void add_watcher(u64 time, watcher_func func, u64 user_data);
void remove_watcher(watcher_func func);

} // namespace timer
