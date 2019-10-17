#pragma once
#include "common.hpp"
namespace arch::device::RTC
{
void init();
u64 get_current_time_microsecond();
} // namespace arch::device::RTC
