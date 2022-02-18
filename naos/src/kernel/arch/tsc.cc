#include "kernel/arch/tsc.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/clock/clock_event.hpp"
#include "kernel/irq.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/trace.hpp"
namespace arch::TSC
{
void clock_source::init() { fill_tsc = _rdtsc(); }

void clock_source::destroy() {}

u64 clock_source::calibrate_tsc(::timeclock::clock_source *cs)
{
    // 5ms
    u64 test_time = 5000;
    auto ev = cs->get_event();
    ev->resume();
    ev->wait_next_tick();
    u64 t = test_time + cs->current();
    u64 start_tsc = _rdtsc();

    while (cs->current() <= t)
    {
        __asm__ __volatile__("pause\n\t" : : : "memory");
    }
    u64 end_tsc = _rdtsc();
    t = cs->current() - t + test_time;
    ev->suspend();

    return (end_tsc - start_tsc) * 1000000 / t;
}

void clock_source::calibrate(::timeclock::clock_source *cs)
{
    i64 tsc_hz[5];
    i64 sum = 0;
    for (auto &i : tsc_hz)
    {
        i = calibrate_tsc(cs);
        sum += i;
    }
    sum /= (sizeof(tsc_hz) / (sizeof(tsc_hz[0])));
    tsc_tick_per_microsecond = sum / 1000000;
    u64 abs_delta = 0;
    for (auto i : tsc_hz)
    {
        i64 d = (i - sum) / 1000;
        abs_delta += d * d;
    }
    abs_delta /= (sizeof(tsc_hz) / (sizeof(tsc_hz[0])));
    abs_delta /= 1000000;

    trace::debug("TSC freqency ", sum, "HZ. ", tsc_tick_per_microsecond, "MHZ. Delta ", abs_delta);
}

u64 clock_source::current() { return (_rdtsc() - fill_tsc) / tsc_tick_per_microsecond; }

clock_source *global_tsc_cs = nullptr;
clock_event *global_tsc_ev = nullptr;

clock_source *make_clock()
{
    if (global_tsc_cs == nullptr)
    {
        global_tsc_cs = memory::New<clock_source>(memory::KernelCommonAllocatorV);
        global_tsc_ev = memory::New<clock_event>(memory::KernelCommonAllocatorV);
        global_tsc_ev->set_source(global_tsc_cs);
        global_tsc_cs->set_event(global_tsc_ev);
        global_tsc_ev->init(1000);
        global_tsc_cs->init();
    }
    return global_tsc_cs;
}

} // namespace arch::TSC
