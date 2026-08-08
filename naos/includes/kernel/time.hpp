#pragma once
#include "kernel/common.hpp"
#include "types.hpp"
namespace timeclock
{

constexpr microsecond_t microseconds_per_second = 1'000'000;

struct time
{
    int64_t tv_sec;
    int64_t tv_nsec;
    time(int64_t sec)
        : tv_sec(sec)
        , tv_nsec(0) {

        };
    time(int64_t sec, int64_t ns)
        : tv_sec(sec)
        , tv_nsec(ns) {

        };
    static time make(int64_t ms) { return time(ms / 1000, ms % 1000 * 1000); }
};

// Convert an absolute monotonic timespec to the precision used by timer sources.
inline bool try_to_microseconds(const time &value, microsecond_t &result)
{
    if (value.tv_sec < 0 || value.tv_nsec < 0 || value.tv_nsec >= 1'000'000'000)
        return false;

    const auto seconds = static_cast<microsecond_t>(value.tv_sec);
    if (seconds > static_cast<microsecond_t>(-1) / microseconds_per_second)
        return false;

    const auto whole_seconds = seconds * microseconds_per_second;
    const auto microseconds = static_cast<microsecond_t>(value.tv_nsec / 1000);
    if (whole_seconds > static_cast<microsecond_t>(-1) - microseconds)
        return false;

    result = whole_seconds + microseconds;
    return true;
}

typedef void (*tick_callback_func)(microsecond_t time, u64 user_data);

} // namespace timeclock
