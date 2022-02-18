#include "kernel/arch/pit.hpp"
#include "kernel/arch/interrupt.hpp"
#include "kernel/arch/io.hpp"
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
const u32 rate = 1193182;

irq::request_result _ctx_interrupt_ on_event(const void *regs, u64 extra_data, u64 user_data)
{
    clock_event *ev = (clock_event *)user_data;
    if (unlikely(ev == nullptr))
        return irq::request_result::no_handled;
    if (!ev->is_suspend)
        ev->tick_jiff = ev->tick_jiff + 1;
    irq::raise_soft_irq(irq::soft_vector::timer);
    return irq::request_result::ok;
}

void clock_event::wait_next_tick()
{
    u64 old_jiff = tick_jiff;

    // wait a new tick
    while (tick_jiff == old_jiff)
        ;
    kassert(tick_jiff > old_jiff, "Unexpected value");
}

void clock_event::init(u64 hz)
{
    this->hz = hz;
    tick_jiff = 0;
    is_suspend = false;
    suspend();
    pc = (rate + hz / 2) / hz;
    trace::debug("PIT timer set count ", pc);
}

void clock_event::destroy() { pc = 0; }

void clock_event::suspend()
{
    if (!is_suspend)
    {
        uctx::UninterruptibleContext icu;
        irq::unregister_request_func(irq::hard_vector::pit_timer, on_event, (u64)this);
        io_out8(command_port, 0b00110110);
        u32 p = 0xFFFFFFFF;
        io_out8(channel_0_port, p & 0xff);
        io_out8(channel_0_port, (p >> 8) & 0xff);
        is_suspend = true;
    }
}

void clock_event::resume()
{

    if (is_suspend)
    {
        {
            uctx::UninterruptibleContext icu;

            io_out8(command_port, 0b00110110);
            io_out8(channel_0_port, pc & 0xff);
            io_out8(channel_0_port, (pc >> 8) & 0xff);

            is_suspend = false;
            irq::register_request_func(irq::hard_vector::pit_timer, on_event, (u64)this);
        }
    }
}

void clock_source::init() {}

void clock_source::destroy() {}

void clock_source::calibrate(::timeclock::clock_source *cs)
{
    // nothing to do
}

u64 clock_source::current()
{
    static u64 old_jiff = 0xFFFFFFFFFF;
    static u64 old_count = 0xFFFFFFFFFF;
    clock_event *ev = (clock_event *)event;
    u16 count;
    u64 jiff;
    {
        uctx::UninterruptibleContext uic;

        jiff = ev->tick_jiff;

        io_out8(command_port, 0);
        count = io_in8(channel_0_port);
        count |= ((u16)io_in8(channel_0_port)) << 8;

        if (count > ev->pc)
            count = ev->pc - 1;

        if (count > old_count && old_jiff == jiff)
            count = old_count;

        old_count = count;
        old_jiff = jiff;
    }
    count = ev->pc - 1 - count;

    return (jiff * ev->pc + count) * 1000000ul / (rate + ev->hz / 2);
    // return jiff * 1000000ul / ev->hz;
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
        global_pit_event->init(1000);
        global_pit_source->init();
    }
    return global_pit_source;
}

} // namespace arch::device::PIT