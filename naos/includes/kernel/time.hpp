#pragma once
#include "common.hpp"
#include "types.hpp"
namespace timeclock
{

struct time
{
    int64_t tv_sec;
    int64_t tv_nsec;
    time(int64_t sec)
        : tv_sec(sec)
        , tv_nsec(0){

          };
    time(int64_t sec, int64_t ns)
        : tv_sec(sec)
        , tv_nsec(ns){

          };
    static time make(int64_t ms) { return time(ms / 1000, ms % 1000 * 1000); }
};

typedef void (*tick_callback_func)(microsecond_t time, u64 user_data);

} // namespace time
