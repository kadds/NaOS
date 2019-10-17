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

bool time_tick(time::microsecond_t period_microsecond, u64 user_data)
{
    current_time_microsecond += period_microsecond;
    return true;
}

bool print_tick(time::microsecond_t period_microsecond, u64 user_data)
{
    time::time_t t;
    time2time_t(current_time_microsecond, &t);
    trace::debug("current time: ", t.year, ".", t.month, ".", t.day, " ", t.hour, ":", t.minute, ":", t.second, ":",
                 t.millisecond);
    return true;
}

void init()
{
    location_offset = 0;
    arch::device::RTC::init();
    start_time_microsecond = arch::device::RTC::get_current_time_microsecond();
    current_time_microsecond = start_time_microsecond;

    time::time_t t;
    time2time_t(current_time_microsecond, &t);
    trace::debug("current time in RTC  ", t.year, ".", t.month, ".", t.day, " ", t.hour, ":", t.minute, ":", t.second,
                 ":", t.millisecond);
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

void time2time_t(time::microsecond_t ms, time::time_t *t)
{
    t->microsecond = ms % 1000;
    ms /= 1000;
    t->millisecond = ms % 1000;
    u64 s = ms / 1000;
    u64 m = s / 60;
    u64 h = m / 60;

    u64 d = h / 24;
    ms -= d * 24 * 60 * 60 * 1000;
    // 2000 is leap year so d subtact 1
    // y = (d - y / 4 - 1) / 365
    u64 y = 4 * d / (4 * 365 + 1);
    t->year = 2000 + y;

    u64 ad = d - y / 4 - y * 365 - 1;
    ad++;

    kassert(ad <= 365, "day must  <= a year day 356");

    for (int i = 0; i < 13; i++)
    {
        if (ad <= (u64)cv_monthy_table[i])
        {
            t->month = i - 1;
            t->day = ad - cv_monthy_table[i - 1] - 1;
            break;
        }
    }
    ms /= 1000;
    t->second = ms % 60;
    ms /= 60;
    t->minute = ms % 60;
    ms /= 60;
    t->hour = ms % 24;
}

time::microsecond_t time_t2time(const time::time_t *t)
{
    if (t->year < 2000)
        return 0;

    time::microsecond_t ms;
    ms = ((t->year - 2000) / 4 + (t->year - 2000) * 365 + monthy_table[t->month] + t->day) * 24 * 60 * 60 + 1000;
    ms += t->hour * 60 * 60 * 1000 + t->minute * 60 * 1000 + t->second * 1000 + t->millisecond;
    return (ms * 1000) + t->microsecond;
}

} // namespace clock
