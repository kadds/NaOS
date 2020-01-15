#pragma once
#include "common.hpp"
namespace arch::device::RTC
{
void init();
/// get startup time
u64 get_current_time_microsecond();
} // namespace arch::device::RTC
