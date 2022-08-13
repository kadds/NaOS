#include "kernel/arch/rtc.hpp"
#include "freelibcxx/time.hpp"
#include "kernel/arch/io.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/trace.hpp"

namespace arch::device::RTC
{

const io_port index_port = 0x70;
const io_port data_port = 0x71;
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

u64 get_current_clock()
{
    u16 century;
    freelibcxx::tm_t startup;
    do
    {
        startup.seconds = get_data(0x0);

        startup.minutes = get_data(0x2);

        startup.hours = get_data(0x4);

        startup.mday = get_data(0x7);

        startup.month = get_data(0x8);

        startup.year = get_data(0x9);

        century = get_data(0x32);

    } while (!has_update());
    start_tsc = _rdtsc();

    u8 flag = get_data(0xB);
    if (!(flag & 0x4))
    {
        startup.seconds = bcd2bin(startup.seconds);
        startup.minutes = bcd2bin(startup.minutes);
        startup.hours = ((startup.hours & 0x0F) + (((startup.hours & 0x70) / 16) * 10)) | (startup.hours & 0x80);
        startup.mday = bcd2bin(startup.mday);
        startup.month = bcd2bin(startup.month);
        startup.year = bcd2bin(startup.year);
        century = bcd2bin(century) * 100;
    }
    startup.year += century;
    startup.month--;

    kassert(startup.year >= 1970, "Current time is incorrect.");

    return startup.to_posix_microseconds();
}
void update_clock(u64 time)
{
    // TODO:: write to RTC register
}

} // namespace arch::device::RTC
