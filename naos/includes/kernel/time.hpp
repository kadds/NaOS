#pragma once
#include "common.hpp"
#include "types.hpp"
namespace time
{
typedef void (*tick_callback_func)(microsecond_t time, u64 user_data);

} // namespace time
