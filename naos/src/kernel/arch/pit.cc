#include "kernel/arch/pit.hpp"
#include "kernel/arch/interrupt.hpp"
#include "kernel/arch/io.hpp"
#include "kernel/arch/io_apic.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/irq.hpp"
#include "kernel/kernel.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/trace.hpp"
#include "kernel/ucontext.hpp"

namespace arch::device::PIT
{
const io_port channel_0_port = 0x40;
const io_port channel_2_port = 0x42;
const io_port command_port = 0x43;
// rate HZ
const u32 freq = 1193182;

u16 read()
{
    io_out8(command_port, 0b0000'0000);
    u16 count = io_in8(channel_0_port);
    count |= ((u16)io_in8(channel_0_port)) << 8;
    return count;
}

u16 read_counter()
{
    uctx::UninterruptibleContext icu;

    u16 val = read();
    u16 new_val = val;
    return new_val;
}

irq::request_result _ctx_interrupt_ on_event(const irq::interrupt_info *inter, u64 extra_data, u64 user_data)
{
    clock_event *ev = (clock_event *)user_data;
    if (unlikely(ev == nullptr))
        return irq::request_result::no_handled;
    if (!ev->is_suspend_.load())
    {
        ev->jiff_.fetch_add(1);
        ev->update_tsc_ = _rdtsc();
    }
    irq::raise_soft_irq(irq::soft_vector::timer);
    return irq::request_result::ok;
}

void clock_event::init(u64 hz)
{
    hz_ = hz;
    is_suspend_ = false;
    suspend();
    divisor_ = freq / hz * 2;

    trace::debug("PIT timer divisor ", divisor_, " ", hz, "hz");
}

void clock_event::destroy() { divisor_ = 0; }

void clock_event::suspend()
{
    if (!is_suspend_)
    {
        is_suspend_ = true;
        APIC::io_disable(APIC::query_gsi(APIC::gsi_vector::pit));
        {
            uctx::UninterruptibleContext icu;

            io_out8(command_port, 0b00110110);
            u32 p = 0xFFFFFFFF;
            io_out8(channel_0_port, p & 0xFF);
            io_out8(channel_0_port, (p >> 8) & 0xFF);
        }
        irq::unregister_request_func(APIC::query_gsi(APIC::gsi_vector::pit) + 0x20, on_event, (u64)this);
    }
}

void clock_event::resume()
{
    if (is_suspend_)
    {
        is_suspend_ = false;
        jiff_ = 1;
        last_tick_ = 0;

        irq::register_request_func(APIC::query_gsi(APIC::gsi_vector::pit) + 0x20, on_event, (u64)this);
        {
            uctx::UninterruptibleContext icu;
            io_out8(command_port, 0b00110110);
            io_out8(channel_0_port, divisor_ & 0xFF);
            io_out8(channel_0_port, (divisor_ >> 8) & 0xFF);

            init_val_ = divisor_ - read_counter();
            update_tsc_ = 0;
        }
        APIC::io_enable(APIC::query_gsi(APIC::gsi_vector::pit));
    }
}

void clock_source::init() {}

void clock_source::destroy() { clock_event *ev = (clock_event *)event; }

void clock_source::calibrate(::timeclock::clock_source *cs)
{
    // nothing to do
}

u64 clock_source::current()
{
    clock_event *ev = (clock_event *)event;

    u64 val = read_counter();
    u64 jiff = ev->jiff_.load();
    u64 init_val = ev->init_val_;

    u64 div = ev->divisor_;
    u64 tick = jiff * div + div - val;

    u64 last = ev->last_tick_;
    if (tick < last)
    {
        while (tick < last)
        {
            tick += div;
        }
    }
    ev->last_tick_ = tick;

    return ((tick - init_val) * 1000'000UL) / freq / 2;
}
u64 clock_source::jiff()
{
    clock_event *ev = (clock_event *)event;
    return ev->jiff_;
}

clock_source *global_pit_source = nullptr;
clock_event *global_pit_event = nullptr;

clock_source *make_clock()
{
    if (global_pit_source == nullptr)
    {
        global_pit_source = memory::New<clock_source>(memory::KernelCommonAllocatorV);
        global_pit_event = memory::New<clock_event>(memory::KernelCommonAllocatorV);
        global_pit_source->set_event(global_pit_event);
        global_pit_event->set_source(global_pit_source);
        global_pit_event->init(50);
        global_pit_source->init();
    }
    return global_pit_source;
}
void disable_all()
{
    uctx::UninterruptibleContext icu;
    io_out8(command_port, 0b00110110);
    u32 p = 0xFFFFFFFF;
    io_out8(channel_0_port, p & 0xff);
    io_out8(channel_0_port, (p >> 8) & 0xff);
}

} // namespace arch::device::PIT