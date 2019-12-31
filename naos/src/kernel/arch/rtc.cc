#include "kernel/arch/rtc.hpp"
#include "kernel/arch/io.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/trace.hpp"

namespace arch::device::RTC
{
// start 2001:01:01 00:00:00
struct date_time
{
    u8 second; // 0 -59
    u8 minute; // 0 - 59
    u8 hour;   // 0 - 24
    u8 day;    // 0 - 31
    u8 month;  // 0 - 12
    u8 year;   // 0 - 255
};

const io_port index_port = 0x70;
const io_port data_port = 0x71;
date_time dt;
u64 start_tsc;

u8 get_data(u8 port)
{
    io_out8(index_port, port);
    return io_in8(data_port);
}

bool has_update() { return get_data(0x0A) & 0x80; }
u8 bcd2bin(u8 v) { return (v & 0xF) + ((v >> 4) * 10); }

void init() {}

int is_leap_year(u16 year) { return year % 400 == 0 || (year % 4 == 0 && year % 100 != 0); }
int cv_monthy_table[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365};

u64 get_current_time_microsecond()
{
    u16 century;
    do
    {
        dt.second = get_data(0x0);

        dt.minute = get_data(0x2);

        dt.hour = get_data(0x4);

        dt.day = get_data(0x7);

        dt.month = get_data(0x8);

        dt.year = get_data(0x9);

        century = get_data(0x32);

    } while (!has_update());
    start_tsc = _rdtsc();

    u8 flag = get_data(0xB);
    if (!(flag & 0x4))
    {
        dt.second = bcd2bin(dt.second);
        dt.minute = bcd2bin(dt.minute);
        dt.hour = ((dt.hour & 0x0F) + (((dt.hour & 0x70) / 16) * 10)) | (dt.hour & 0x80);
        dt.day = bcd2bin(dt.day);
        dt.month = bcd2bin(dt.month);
        dt.year = bcd2bin(dt.year);
        century = bcd2bin(century) * 100;

        kassert(century == 2000, "Must be 21 century.");
    }

    kassert(dt.year >= 19, "Current time is incorrect.");

    u64 s = 0;
    s += dt.second;
    s += dt.minute * 60;
    s += dt.hour * 60 * 60;
    s += (dt.day + cv_monthy_table[dt.month - 1] + dt.year / 4) * 24 * 60 * 60;
    s += dt.year * 365 * 24 * 60 * 60;

    return s * 1000 * 1000;
}
void save_current_time_microsecond(u64 time)
{
    // TODO:: write to RTC register
}

} // namespace arch::device::RTC
