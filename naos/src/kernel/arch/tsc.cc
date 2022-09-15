#include "kernel/arch/tsc.hpp"
#include "freelibcxx/utils.hpp"
#include "kernel/arch/cpu_info.hpp"
#include "kernel/arch/hpet.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/arch/pit.hpp"
#include "kernel/clock/clock_event.hpp"
#include "kernel/irq.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/trace.hpp"
#include <limits>
namespace arch::TSC
{
void clock_source::init() { begin_tsc_ = _rdtsc(); }

void clock_source::destroy() {}

// 20ms
constexpr u64 test_duration_us = 20000;
constexpr size_t test_times = 5;

u64 clock_source::calibrate_tsc(::timeclock::clock_source *cs)
{
    auto from_ev = cs->get_event();
    from_ev->resume();

    u64 t = test_duration_us + cs->current();
    u64 start_tsc = _rdtsc();

    while (cs->current() <= t)
    {
        cpu_pause();
    }
    u64 end_tsc = _rdtsc();
    u64 cost = cs->current() - t + test_duration_us;

    from_ev->suspend();

    return (end_tsc - start_tsc) * 1000'000UL / cost;
}

void clock_source::calibrate(::timeclock::clock_source *cs)
{
    if (cpu_info::max_basic_cpuid() >= 0x15)
    {
        auto f = cpu_info::get_feature(cpu_info::feature::tsc_frequency);
        if (f > 0)
        {
            const u64 base_hz = 10;
            trace::info("TSC builtin frequency ", f * base_hz, "MHZ");
            tsc_tick_second_ = f * base_hz;
            builtin_freq_ = true;
            return;
        }
    }
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
    freelibcxx::sort(tsc_freq, tsc_freq + test_times);

    const u64 avg_freq = total_freq / test_times;
    u64 delta = 0;
    for (auto freq : tsc_freq)
    {
        i64 d = ((i64)freq - (i64)avg_freq) / 1000'000;
        delta += d * d;
    }
    delta /= test_times;
    const u64 freq = tsc_freq[test_times / 2];

    trace::info("TSC frequency ", freq / 1000'000UL, "MHZ. delta ", delta, " min ", min_freq / 1000'000UL, "MHZ.",
                " max ", max_freq / 1000'000UL, "MHZ.");

    tsc_tick_second_ = freq;
}

u64 clock_source::current() { return (_rdtsc() - begin_tsc_) * 1000'000UL / tsc_tick_second_; }

clock_source *make_clock()
{
    if (arch::cpu_info::has_feature(arch::cpu_info::feature::constant_tsc))
    {
        auto tsc_cs = memory::New<clock_source>(memory::KernelCommonAllocatorV);
        tsc_cs->init();
        return tsc_cs;
    }
    else
    {
        trace::debug("no constant tsc");
    }
    return nullptr;
}

} // namespace arch::TSC
