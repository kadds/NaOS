#pragma once
#include "common.hpp"
namespace arch::device::RTC
{
void init();
/// get startup time
u64 get_current_clock();
u64 update_clock();
} // namespace arch::device::RTC
