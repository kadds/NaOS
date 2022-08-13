#include "kernel/clock.hpp"
#include "freelibcxx/formatter.hpp"
#include "freelibcxx/string.hpp"
#include "freelibcxx/time.hpp"
#include "kernel/arch/rtc.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/timer.hpp"
#include "kernel/trace.hpp"
#include "kernel/ucontext.hpp"

namespace timeclock
{

// offset UTC seconds
i64 location_offset;

microsecond_t start_time_microseconds, current_time_microseconds;
const microsecond_t time_1970_to_start = (30ul * 365 + (2000 - 1970) / 4) * 24 * 60 * 60 * 1000 * 1000;

void time_tick(microsecond_t expires, u64 user_data)
{
    current_time_microseconds = expires + start_time_microseconds;
    timer::add_watcher(100000, time_tick, 0);
}

void init()
{
    location_offset = 0;
    arch::device::RTC::init();
    start_time_microseconds = arch::device::RTC::get_current_clock();
    current_time_microseconds = start_time_microseconds;

    freelibcxx::string str(memory::KernelCommonAllocatorV);
    str.resize(20);

    if (auto val = freelibcxx::tm_t::from_posix_seconds(current_time_microseconds / 1000 / 1000); val.has_value())
    {
        val.value().format(str.span());
    }
    else
    {
        trace::panic("rtc get clock fail ", start_time_microseconds);
    }

    trace::debug("Clock: ", str.data());
}

void start_tick()
{
    // add 100 ms
    timer::add_watcher(100000, time_tick, 0);
}

microsecond_t get_current_clock() { return current_time_microseconds; }

microsecond_t get_startup_clock() { return start_time_microseconds; }

time to_time(microsecond_t t)
{
    return time{
        static_cast<int64_t>(t / 1000),
        static_cast<int64_t>(t % 1000 * 1000),
    };
}

} // namespace timeclock
