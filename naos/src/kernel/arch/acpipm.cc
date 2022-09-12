#include "kernel/arch/acpipm.hpp"
#include "common.hpp"
#include "kernel/arch/acpi/acpi.hpp"
#include "kernel/arch/io.hpp"
#include "kernel/arch/io_apic.hpp"
#include "kernel/arch/mm.hpp"
#include "kernel/arch/paging.hpp"
#include "kernel/irq.hpp"
#include "kernel/kernel.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/mm/vm.hpp"
#include "kernel/trace.hpp"
#include "kernel/types.hpp"
#include "kernel/ucontext.hpp"
#include <limits>

namespace arch::device::ACPI
{

u32 read_register_io(u64 port)
{
    u32 val = io_in32(port);
    return val;
}
u32 read_register_mm(u64 address)
{
    u32 val = *reinterpret_cast<u32 *>(address);
    _mfence();
    return val;
}

irq::request_result _ctx_interrupt_ on_acpipm_event(u64 user_data)
{
    clock_event *ev = (clock_event *)user_data;
    if (unlikely(ev == nullptr))
        return irq::request_result::no_handled;
    if (!ev->is_suspend_.load())
    {
        ev->jiff_.fetch_add(1);
        irq::raise_soft_irq(irq::soft_vector::timer);
        return irq::request_result::ok;
    }
    else
    {
        return irq::request_result::no_handled;
    }
}

constexpr u64 freq = 3579545;

void clock_event::init(u64 hz)
{
    // load base
    auto info_opt = arch::ACPI::get_acpipm_base();
    auto info = info_opt.value();
    trace::debug("acpipm base ", trace::hex(info.block_base), " new base ", trace::hex(info.xblock_base));

    if (info.bit32mode)
    {
        bit32_mode_ = true;
    }
    periods_ = (1U << 23) - 1;

    read_register_ = &read_register_io;
    if (info.block_base == 0)
    {
        if (info.xblock_base != 0)
        {
            address_ = reinterpret_cast<u64>(info.xblock_base);
            if (!info.use_io)
            {
                u64 start = memory::alloc_io_mmap_address(memory::page_size, memory::page_size);
                void *p = reinterpret_cast<void *>(start);
                memory::kernel_vm_info->paging().map_to(
                    p, 1, phy_addr_t::from(address_),
                    paging::flags::writable | paging::flags::write_through | paging::flags::cache_disable, 0);
                memory::kernel_vm_info->paging().reload();

                read_register_ = read_register_mm;
                address_ = start;
            }
        }
        else
        {
            trace::panic("acpipm base is 0");
        }
    }
    else
    {
        address_ = info.block_base;
    }

    is_suspend_ = false;
    suspend();
}

void clock_event::destroy() {}

void clock_event::suspend()
{
    if (!is_suspend_)
    {
        is_suspend_ = true;
        uctx::UninterruptibleContext icu;
        arch::ACPI::unregister_timer();
    }
}

void clock_event::resume()
{
    if (is_suspend_)
    {
        is_suspend_ = false;
        jiff_ = 1;
        last_tick_ = 0;
        uctx::UninterruptibleContext icu;
        bool ok = arch::ACPI::register_timer(on_acpipm_event, (u64)this);
        kassert(ok, "enable acpipm fail");
        init_val_ = read_register_(address_) & 0xFFFFFF;
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

    u32 val = ev->read_register_(ev->address_) & 0xFFFFFF;
    u64 jiff = ev->jiff_;
    u32 init_val = ev->init_val_;
    u32 counter = ev->periods_;
    u64 tick = jiff * counter + val;

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

    return ((tick - init_val) * 1000'000UL) / freq;
}
u64 clock_source::jiff()
{
    clock_event *ev = (clock_event *)event;
    return ev->jiff_;
}

clock_source *make_clock()
{
    if (arch::ACPI::has_init() && arch::ACPI::get_acpipm_base().has_value())
    {
        auto acpipm_source = memory::New<clock_source>(memory::KernelCommonAllocatorV);
        auto acpipm_event = memory::New<clock_event>(memory::KernelCommonAllocatorV);
        acpipm_source->set_event(acpipm_event);
        acpipm_event->set_source(acpipm_source);
        acpipm_event->init(0);
        acpipm_source->init();
        return acpipm_source;
    }
    return nullptr;
}
} // namespace arch::device::ACPI
