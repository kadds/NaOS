#include "kernel/arch/hpet.hpp"
#include "kernel/arch/acpi/acpi.hpp"
#include "kernel/arch/io_apic.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/arch/paging.hpp"
#include "kernel/irq.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/mm/vm.hpp"
#include "kernel/trace.hpp"
#include "kernel/ucontext.hpp"
namespace arch::device::HPET
{
irq::request_result _ctx_interrupt_ on_hpet_event(const irq::interrupt_info *inter, u64 extra_data, u64 user_data)
{
    clock_event *ev = (clock_event *)user_data;
    if (unlikely(ev == nullptr))
        return irq::request_result::no_handled;
    if (!ev->is_suspend.load())
    {
        if (!ev->mode_periodic_)
        {
            ev->target_counter_ = ev->target_counter_ + ev->counter_;
            ev->base_[32 + 1] = ev->target_counter_;
        }
        ev->jiff_.fetch_add(1);
    }

    irq::raise_soft_irq(irq::soft_vector::timer);
    return irq::request_result::ok;
}

void clock_event::init(u64 hz)
{
    auto base = arch::ACPI::get_hpet_base();
    auto addr = base.value();
    u64 map_start = memory::alloc_io_mmap_address(paging::frame_size::size_4kb, paging::frame_size::size_4kb);
    auto &paging = memory::kernel_vm_info->paging();
    paging.map_to(reinterpret_cast<void *>(map_start), 1, addr,
                  paging::flags::cache_disable | paging::flags::writable | paging::flags::write_through, 0);
    paging.reload();

    u64 *reg = reinterpret_cast<u64 *>(map_start);
    base_ = reg;

    u64 capabilities = base_[0];
    u32 period = capabilities >> 32;
    u16 id = (capabilities >> 16) & 0xFFFF;
    bool bit64mode = capabilities & 0x2000;  // bit 13
    bool leg_rt_cap = capabilities & 0x8000; // bit 15
    if (!leg_rt_cap)
    {
        trace::panic("LegacyReplacement Route Capable: false");
    }
    if (!bit64mode)
    {
        trace::panic("Bit 64 mode: false");
    }

    u8 rev_id = capabilities & 0xF;
    u8 timers = ((capabilities >> 8) & 0x7) + 1;

    u64 freq = 1'000'000'000'000'000UL / period;

    // disable all timer
    for (u8 i = 0; i < timers; i++)
    {
        u64 n_config_reg = base_[32 + 4 * i];
        if (n_config_reg & 0b10000 && i == 0)
        {
            // mode_periodic_ = true;
        }

        // clean field
        n_config_reg &= ~0xC10EUL;

        base_[32 + 4 * i] = n_config_reg;
    }

    // enable main counter
    u64 config_reg = (base_[2] & ~0b11UL) | 0b11;
    base_[2] = config_reg;

    // set counter
    counter_ = freq / hz;

    trace::debug("HPET timers ", timers, " rev id ", rev_id, " id ", id, " bit64 ", bit64mode, " freq ",
                 freq / 1000'000UL, "MHZ ", "periodic ", mode_periodic_, " set counter ", counter_);

    this->freq_ = freq;
    hz_ = hz;
    mode64_ = bit64mode;

    APIC::io_entry entry;
    APIC::io_irq_setup(APIC::query_gsi(APIC::gsi_vector::hpet), &entry);

    is_suspend = false;
    suspend();
}

void clock_event::destroy() { freq_ = 0; }

void clock_event::suspend()
{
    if (!is_suspend)
    {
        is_suspend = true;
        APIC::io_disable(APIC::query_gsi(APIC::gsi_vector::hpet));
        {
            uctx::UninterruptibleContext icu;
            u64 n_config_reg = base_[32];
            base_[32] = n_config_reg & ~0b100UL;
        }

        irq::unregister_request_func(APIC::query_gsi(APIC::gsi_vector::hpet) + 0x20, on_hpet_event, (u64)this);
    }
}

void clock_event::resume()
{
    if (is_suspend)
    {
        is_suspend = false;
        jiff_ = 1;
        last_tick_ = 0;
        irq::register_request_func(APIC::query_gsi(APIC::gsi_vector::hpet) + 0x20, on_hpet_event, (u64)this);
        {
            uctx::UninterruptibleContext icu;

            init_val_ = base_[30];
            target_counter_ = init_val_ + counter_;
            base_[32 + 1] = target_counter_;
            base_[32] = base_[32] | 0b100UL;
        }
        APIC::io_enable(APIC::query_gsi(APIC::gsi_vector::hpet));
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
    clock_event *ev = (clock_event *)event;
    u64 val = ev->base_[30];
    _mfence();
    u64 jiff = ev->jiff_;
    u64 counter = ev->counter_;
    u64 target = ev->target_counter_;
    u64 init_val = ev->init_val_;

    u64 tick = jiff * counter + val + counter - target;
    u64 last = ev->last_tick_;
    if (tick < last)
    {
        while (tick < last)
        {
            tick += counter;
        }
    }
    else
    {
    }
    ev->last_tick_ = tick;

    return ((tick - init_val) * 1000'000UL) / ev->freq_;
}

u64 clock_source::jiff()
{
    clock_event *ev = (clock_event *)event;
    return ev->jiff_;
}

clock_source *global_hpet_cs = nullptr;
clock_event *global_hpet_ev = nullptr;

clock_source *make_clock()
{
    if (global_hpet_cs == nullptr)
    {
        auto base = arch::ACPI::get_hpet_base();
        if (!base.has_value())
        {
            return nullptr;
        }
        global_hpet_cs = memory::New<clock_source>(memory::KernelCommonAllocatorV);
        global_hpet_ev = memory::New<clock_event>(memory::KernelCommonAllocatorV);
        global_hpet_ev->set_source(global_hpet_cs);
        global_hpet_cs->set_event(global_hpet_ev);
        global_hpet_ev->init(50);
        global_hpet_cs->init();
    }
    return global_hpet_cs;
}

} // namespace arch::device::HPET