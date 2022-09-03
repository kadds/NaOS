#include "kernel/arch/tsc.hpp"
#include "freelibcxx/utils.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/clock/clock_event.hpp"
#include "kernel/irq.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/trace.hpp"
#include <limits>
namespace arch::TSC
{
void clock_source::init() { begin_tsc = _rdtsc(); }

void clock_source::destroy() {}

// 20ms
constexpr u64 test_duration_us = 200000;
constexpr size_t test_times = 5;

u64 clock_source::calibrate_tsc(::timeclock::clock_source *cs)
{
    auto from_ev = cs->get_event();
    from_ev->resume();
    from_ev->wait_next_tick();

    u64 t = test_duration_us + cs->current();
    u64 start_tsc = _rdtsc();

    while (cs->current() <= t)
    {
        __asm__ __volatile__("pause\n\t" : : : "memory");
        _mfence();
    }
    u64 end_tsc = _rdtsc();
    u64 cost = cs->current() - t + test_duration_us;

    from_ev->suspend();

    return (end_tsc - start_tsc) * 1000'000UL / cost;
}

void clock_source::calibrate(::timeclock::clock_source *cs)
{
    trace::debug("TSC calibrate");
    u64 tsc_freq[test_times];
    u64 total_freq = 0;
    u64 max_freq = 0;
    u64 min_freq = std::numeric_limits<u64>::max();

    for (auto &freq : tsc_freq)
    {
        freq = calibrate_tsc(cs);
        total_freq += freq;
        min_freq = freelibcxx::min(min_freq, freq);
        max_freq = freelibcxx::max(max_freq, freq);
    }

    const u64 avg_freq = total_freq / test_times;
    u64 delta = 0;
    for (auto freq : tsc_freq)
    {
        i64 d = ((i64)freq - (i64)avg_freq) / 1000'000;
        delta += d * d;
    }
    delta /= test_times;

    trace::debug("TSC frequency ", avg_freq / 1000'000UL, "MHZ. delta ", delta, " min ", min_freq / 1000'000UL, "MHZ.",
                 " max ", max_freq / 1000'000UL, "MHZ.");

    tsc_tick_second = avg_freq;
}

u64 clock_source::current() { return (_rdtsc() - begin_tsc) * 1000'000UL / tsc_tick_second; }

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
