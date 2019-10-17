#pragma once
#include "common.hpp"
namespace time
{
typedef u64 microsecond_t;
typedef u64 millisecond_t;
typedef u64 nanosecond_t;

typedef void (*tick_callback_func)(microsecond_t time, u64 user_data);

/// The time struct
struct time_t
{
    u16 microsecond; ///< 0 - 1000
    u16 millisecond; ///< 0 - 1000
    u8 second;       ///< 0 - 59
    u8 minute;       ///< 0 - 59
    u8 hour;         ///< 0 - 23
    u8 day;          ///< 0 - 30
    u8 month;        ///< 0 - 11
    u8 weak;         ///< 0 - 6
    u16 year;        ///< 2000 - ?
};

} // namespace time
