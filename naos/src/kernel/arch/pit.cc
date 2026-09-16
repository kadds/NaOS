#include "kernel/arch/pit.hpp"

#include "kernel/arch/io.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/time/unsigned_math.hpp"
#include "kernel/ucontext.hpp"

namespace arch::device::PIT
{
namespace
{
constexpr io_port channel_0_port = 0x40;
constexpr io_port command_port = 0x43;
constexpr u64 input_frequency_hz = 1'193'182;
constexpr u64 sampling_hz = 1'000;

u16 read_counter() noexcept
{
    io_out8(command_port, 0);
    const u16 low = io_in8(channel_0_port);
    const u16 high = io_in8(channel_0_port);
    return static_cast<u16>(low | (high << 8));
}
} // namespace

bool clock::start_cpu() noexcept
{
    if (started_)
        return true;
    divisor_ = input_frequency_hz / sampling_hz;
    if (divisor_ == 0 || divisor_ > 0xFFFF)
        return false;
    {
        uctx::UninterruptibleContext context;
        io_out8(command_port, 0b0011'0100);
        io_out8(channel_0_port, static_cast<u8>(divisor_));
        io_out8(channel_0_port, static_cast<u8>(divisor_ >> 8));
        last_count_ = read_counter();
    }
    initial_count_ = divisor_ - last_count_;
    last_tick_ = 0;
    started_ = true;
    return true;
}

void clock::stop_cpu() noexcept
{
    if (!started_)
        return;
    disable_all();
    started_ = false;
}

timeclock::nanosecond_t clock::now_ns() noexcept
{
    if (!started_)
        return 0;
    u16 current = 0;
    {
        uctx::UninterruptibleContext context;
        current = read_counter();
    }

    // Mode 2 counts down and wraps at the programmed divisor.  Reading the
    // counter is enough; the event_source is independent of this clock.
    const u64 elapsed_in_period = divisor_ - current;
    u64 ticks = elapsed_in_period;
    if (current > last_count_)
        last_tick_ += divisor_;
    ticks += last_tick_;
    last_count_ = current;
    if (ticks < initial_count_)
        return 0;
    ticks -= initial_count_;
    u64 ns = 0;
    if (!timeclock::unsigned_math::try_mul_div_floor(ticks, timeclock::nanoseconds_per_second, input_frequency_hz, ns))
        return static_cast<u64>(-1);
    return ns;
}

clock *make_event_clock() noexcept { return memory::New<clock>(memory::KernelCommonAllocatorV); }

void disable_all() noexcept
{
    uctx::UninterruptibleContext context;
    io_out8(command_port, 0b0011'0100);
    io_out8(0x40, 0xFF);
    io_out8(0x40, 0xFF);
}

} // namespace arch::device::PIT
