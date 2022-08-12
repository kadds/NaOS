#include "kernel/clock.hpp"
#include "freelibcxx/formatter.hpp"
#include "freelibcxx/string.hpp"
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

microsecond_t start_time_microsecond, current_time_microsecond;
const microsecond_t time_1970_to_start = (30ul * 365 + (2000 - 1970) / 4) * 24 * 60 * 60 * 1000 * 1000;

void time_tick(microsecond_t expires, u64 user_data)
{
    current_time_microsecond = expires + start_time_microsecond;
    timer::add_watcher(100000, time_tick, 0);
}

void init()
{
    location_offset = 0;
    arch::device::RTC::init();
    start_time_microsecond = arch::device::RTC::get_current_time_microsecond();
    current_time_microsecond = start_time_microsecond;

    time_t t;
    time2time_t(current_time_microsecond, &t);
    trace::debug("Current time: ", 2000 + t.year, ".", t.month + 1, ".", t.mday + 1, " ", t.hour, ":", t.minute, ":",
                 t.second, ".", t.millisecond);
}

void start_tick()
{
    // add 100 ms
    timer::add_watcher(100000, time_tick, 0);
}

microsecond_t get_current_clock() { return current_time_microsecond; }

microsecond_t get_startup_clock() { return start_time_microsecond; }

time to_time(microsecond_t t)
{
    return time{
        static_cast<int64_t>(t / 1000),
        static_cast<int64_t>(t % 1000 * 1000),
    };
}

int monthy_table[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
int cv_monthy_table[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365};
int cv_leap_monthy_table[] = {0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335, 366};

void time2time_t(microsecond_t ms, time_t *t)
{
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

microsecond_t time_t2time(const time_t *t)
{
    if (t->year < 2000)
        return 0;

    microsecond_t ms;
    ms = ((t->year - 2000) / 4 + (t->year - 2000) * 365 + monthy_table[t->month] + t->mday) * 24 * 60 * 60 + 1000;
    ms += t->hour * 60 * 60 * 1000 + t->minute * 60 * 1000 + t->second * 1000 + t->millisecond;
    return (ms * 1000) + t->microsecond;
}

void format_fixed(freelibcxx::string &str, int fix, uint64_t val)
{
    str.from_uint64(val);
    if (str.size() < fix)
    {
    }
}

void time_t::format(freelibcxx::span<char> buf, const char *format)
{
    char *b = buf.get();
    int len = buf.size();
    freelibcxx::string str(memory::KernelCommonAllocatorV);

    while (*format != 0 && len > 0)
    {
        auto c = *format;
        switch (c)
        {
            case 'Y':
                format_fixed(str, 4, year + 2000);
                break;
            case 'm':
                format_fixed(str, 2, month + 1);
                break;
            case 'd':
                format_fixed(str, 2, mday);
                break;
            case 'H':
                format_fixed(str, 2, hour);
                break;
            case 'M':
                format_fixed(str, 2, minute);
                break;
            case 'S':
                format_fixed(str, 2, second);
                break;
            default:
                str.append_buffer(&c, 1);
                break;
        }
        if (len < str.size())
            return;

        memcpy(b, str.data(), str.size());
        len -= str.size();
        b += str.size();

        format++;
        str.clear();
    }
}

freelibcxx::optional<time_t> time_t::from(freelibcxx::span<char> buf, const char *format) {}

} // namespace timeclock
