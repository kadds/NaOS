#include "kernel/arch/tsc.hpp"

#include "kernel/arch/cpu_info.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/time/unsigned_math.hpp"

namespace arch::TSC
{
namespace
{
constexpr u64 calibration_duration_ns = 20'000'000;
}

bool clock::start_cpu() noexcept
{
    if (tsc_ticks_per_second_ == 0)
    {
        const auto info = cpu_info::get_tsc_cpuid15_info();
        if (!info.has_frequency())
            return false;
        tsc_ticks_per_second_ = info.frequency_hz();
    }
    begin_tsc_ = read_tsc_ordered();
    cross_cpu_monotonic_ = cpu_info::has_feature(cpu_info::feature::nostop_tsc);
    return tsc_ticks_per_second_ != 0;
}

timeclock::nanosecond_t clock::now_ns() noexcept
{
    if (tsc_ticks_per_second_ == 0)
        return 0;
    const u64 delta = read_tsc_ordered() - begin_tsc_;
    u64 result = 0;
    if (!timeclock::unsigned_math::try_mul_div_floor(delta, timeclock::nanoseconds_per_second, tsc_ticks_per_second_,
                                                     result))
        return static_cast<u64>(-1);
    return result;
}

bool clock::calibrate(timeclock::event_clock &reference) noexcept
{
    const u64 start_ns = reference.now_ns();
    const u64 start_tsc = read_tsc_ordered();
    u64 deadline = 0;
    if (!timeclock::try_add_nanoseconds(start_ns, calibration_duration_ns, deadline))
        return false;
    while (reference.now_ns() < deadline)
        cpu_pause();

    const u64 elapsed_ns = reference.now_ns() - start_ns;
    const u64 elapsed_tsc = read_tsc_ordered() - start_tsc;
    if (elapsed_ns == 0 || elapsed_tsc == 0)
        return false;
    u64 frequency = 0;
    if (!timeclock::unsigned_math::try_mul_div_floor(elapsed_tsc, timeclock::nanoseconds_per_second, elapsed_ns,
                                                     frequency) ||
        frequency == 0)
        return false;
    tsc_ticks_per_second_ = frequency;
    return true;
}

clock *make_event_clock(u64 known_frequency_hz) noexcept
{
    if (!cpu_info::has_feature(cpu_info::feature::constant_tsc))
        return nullptr;
    return memory::New<clock>(memory::KernelCommonAllocatorV, known_frequency_hz);
}

} // namespace arch::TSC
