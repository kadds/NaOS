#include "kernel/clock.hpp"
#include "kernel/arch/rtc.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/timer.hpp"
#include "kernel/trace.hpp"
#include "kernel/ucontext.hpp"
namespace clock
{

// offset UTC seconds
i64 location_offset;

time::microsecond_t start_time_microsecond, current_time_microsecond;
const time::microsecond_t time_1970_to_start = (30ul * 365 + (2000 - 1970) / 4) * 24 * 60 * 60 * 1000 * 1000;

void time_tick(time::microsecond_t expires, u64 user_data)
{
    current_time_microsecond = expires + start_time_microsecond;
    timer::add_watcher(100000, time_tick, 0);
}

void print_tick(time::microsecond_t expires, u64 user_data)
{
    time::time_t t;
    time2time_t(current_time_microsecond, &t);
    trace::debug("current time: ", 2000 + t.year, ".", t.month + 1, ".", t.mday + 1, " ", t.hour, ":", t.minute, ":",
                 t.second, ":", t.millisecond, ", weak ", t.wday);
    timer::add_watcher(10000000, print_tick, 0);
    return;
}

void init()
{
    location_offset = 0;
    arch::device::RTC::init();
    start_time_microsecond = arch::device::RTC::get_current_time_microsecond();
    current_time_microsecond = start_time_microsecond;

    time::time_t t;
    time2time_t(current_time_microsecond, &t);
    trace::debug("current time RTC: ", 2000 + t.year, ".", t.month + 1, ".", t.mday + 1, " ", t.hour, ":", t.minute,
                 ":", t.second, ":", t.second, ":", t.millisecond);
}

void start_tick()
{
    // add 100 ms
    timer::add_watcher(100000, time_tick, 0);
    timer::add_watcher(10000000, print_tick, 0);
}

time::microsecond_t get_current_clock() { return current_time_microsecond; }

time::microsecond_t get_start_clock() { return start_time_microsecond; }

int monthy_table[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
int cv_monthy_table[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365};
int cv_leap_monthy_table[] = {0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335, 366};

void time2time_t(time::microsecond_t ms, time::time_t *t)
{
    auto rms = ms;
    t->microsecond = ms % 1000;
    ms /= 1000;

    t->millisecond = ms % 1000;
    ms /= 1000;

    t->second = ms % 60;
    ms /= 60;

    t->minute = ms % 60;
    ms /= 60;

    t->hour = ms % 24;
    ms /= 24;

    u64 dt = 0;
    while (true)
    {
        t->year = (ms - dt) / 365;
        t->yday = (ms) - ((t->year + 3) / 4) - t->year * 365ul;
        if ((t->year) % 4 == 0)
        {
            if (t->yday < 366)
                break;
            dt += 1;
        }
        else
        {
            if (t->yday < 365)
                break;
            dt += 1;
        }
    }

    if (t->year > 0)
        t->wday = ((((t->year + 3) / 4) + t->year * 365ul + t->yday + 6) % 7);
    else
        t->wday = (t->yday + 6) % 7;

    auto ad = t->yday;
    bool is_leap = t->year % 4 == 0;
    if (is_leap)
    {
        for (int i = 1; i < 13; i++)
        {
            if (ad < (u64)cv_leap_monthy_table[i])
            {
                t->month = i - 1;
                t->mday = ad - cv_leap_monthy_table[i - 1];
                break;
            }
        }
    }
    else
    {
        for (int i = 1; i < 13; i++)
        {
            if (ad < (u64)cv_monthy_table[i])
            {
                t->month = i - 1;
                t->mday = ad - cv_monthy_table[i - 1];
                break;
            }
        }
    }
}

time::microsecond_t time_t2time(const time::time_t *t)
{
    if (t->year < 2000)
        return 0;

    time::microsecond_t ms;
    ms = ((t->year - 2000) / 4 + (t->year - 2000) * 365 + monthy_table[t->month] + t->mday) * 24 * 60 * 60 + 1000;
    ms += t->hour * 60 * 60 * 1000 + t->minute * 60 * 1000 + t->second * 1000 + t->millisecond;
    return (ms * 1000) + t->microsecond;
}

} // namespace clock
