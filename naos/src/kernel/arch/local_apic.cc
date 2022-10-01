#include "kernel/arch/local_apic.hpp"
#include "kernel/arch/acpi/acpi.hpp"
#include "kernel/arch/cpu.hpp"
#include "kernel/arch/cpu_info.hpp"
#include "kernel/arch/interrupt.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/arch/mm.hpp"
#include "kernel/arch/paging.hpp"
#include "kernel/arch/pit.hpp"
#include "kernel/cmdline.hpp"
#include "kernel/irq.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/mm/vm.hpp"
#include "kernel/trace.hpp"
#include "kernel/types.hpp"
#include "kernel/ucontext.hpp"
#include <atomic>
#include <cstddef>

const u16 id_register = 2;
const u16 version_register = 3;
const u16 task_priority_register = 8;
const u16 arbitration_priority_register = 9;
const u16 processor_priority_register = 10;
const u16 eoi_register = 11;
const u16 remote_read_register = 12;
const u16 logical_destination_register = 13;
const u16 destination_format_register = 14;
const u16 spurious_interrupt_vector_register = 15;
const u16 isr_0 = 16;
const u16 isr_1 = 17;
const u16 isr_2 = 18;
const u16 isr_3 = 19;
const u16 isr_4 = 20;
const u16 isr_5 = 21;
const u16 isr_6 = 22;
const u16 isr_7 = 23;

const u16 tmr_0 = 24;
const u16 tmr_1 = 25;
const u16 tmr_2 = 26;
const u16 tmr_3 = 27;
const u16 tmr_4 = 28;
const u16 tmr_5 = 29;
const u16 tmr_6 = 30;
const u16 tmr_7 = 31;

const u16 irr_0 = 32;
const u16 irr_1 = 33;
const u16 irr_2 = 34;
const u16 irr_3 = 35;
const u16 irr_4 = 36;
const u16 irr_5 = 37;
const u16 irr_6 = 38;
const u16 irr_7 = 39;
const u16 esr = 40;

const u16 lvt_cmci = 47; // 0x82F
const u16 icr_0 = 48;
const u16 icr_1 = 49;
const u16 lvt_timer = 50;
const u16 lvt_thermal_sensor_register = 51;
const u16 lvt_performance_register = 52;
const u16 lvt_lint_0 = 53;
const u16 lvt_lint_1 = 54;
const u16 lvt_error = 55;
const u16 timer_initial_count_register = 56;
const u16 timer_current_count_register = 57;

const u16 timer_divide_register = 62; // 0x83E

const u16 self_pip = 63;

const u16 lvt_index_array[] = {
    lvt_cmci, lvt_timer, lvt_thermal_sensor_register, lvt_performance_register, lvt_lint_0, lvt_lint_1, lvt_error, 0};

namespace arch::APIC
{

void *apic_base_addr;
u64 ms_tsc;

u32 read_register_MSR(u16 reg)
{
    reg += 0x800;
    _mfence();
    return _rdmsr(reg);
}

u64 read_register_MSR_64(u16 reg)
{
    reg += 0x800;
    _mfence();
    return _rdmsr(reg);
}

void write_register_MSR(u16 reg, u32 v)
{
    reg += 0x800;
    _wrmsr(reg, v);
    _mfence();
}

void write_register_MSR_64(u16 reg, u64 v)
{
    reg += 0x800;
    _wrmsr(reg, v);
    _mfence();
}

u32 read_register_mm(u16 reg)
{
    ptrdiff_t offset = reg;
    offset <<= 4;
    u32 v = *(u32 *)((byte *)apic_base_addr + offset);
    _mfence();
    return v;
}

u64 read_register_mm_64(u16 reg)
{
    ptrdiff_t offset = reg;
    offset <<= 4;

    u64 v = *(u32 *)((byte *)apic_base_addr + offset);
    _mfence();
    v = v << 32;
    v |= *(u32 *)((byte *)apic_base_addr + offset + 0x4);
    _mfence();
    return v;
}

void write_register_mm(u16 reg, u32 v)
{
    ptrdiff_t offset = reg;
    offset <<= 4;
    *(u32 *)((byte *)apic_base_addr + offset) = v;
    _mfence();
}

void write_register_mm_64(u16 reg, u64 v)
{
    ptrdiff_t offset = reg;
    offset <<= 4;
    *(u32 *)((byte *)apic_base_addr + offset + 0x4) = v >> 32;
    *(u32 *)((byte *)apic_base_addr + offset) = v;
    _mfence();
}

typedef u32 (*read_register_func)(u16);
typedef void (*write_register_func)(u16, u32);

typedef u64 (*read_register_func_64)(u16);
typedef void (*write_register_func_64)(u16, u64);

typedef u64 (*io_read_register_func)(u16);
typedef void (*io_write_register_func)(u16, u64);

read_register_func read_register;
write_register_func write_register;
read_register_func_64 read_register64;
write_register_func_64 write_register64;

u64 current_apic_id() { return _rdmsr(0x802); }

void disable_all_lvt()
{
    u16 i = 1;

    while (lvt_index_array[i] != 0)
    {
        write_register(lvt_index_array[i], 1 << 16);
        i++;
    }
} // namespace arch::APIC

bool builtin_local_apic = false;

void local_init()
{
    u32 id;
    u64 v = (1 << 11);
    if (arch::cpu::current().is_bsp())
    {
        bool acpi = cmdline::get_bool("acpi", false);
        if (cpu_info::has_feature(cpu_info::feature::x2apic))
        {
            trace::debug("x2APIC is supported");
            // read_register = read_register_MSR;
            // write_register = write_register_MSR;
            // read_register64 = read_register_MSR_64;
            // write_register64 = write_register_MSR_64;
        }
        // else
        // {
        read_register = read_register_mm;
        write_register = write_register_mm;
        read_register64 = read_register_mm_64;
        write_register64 = write_register_mm_64;
        // }

        phy_addr_t local_apic_base_addr = phy_addr_t::from(_rdmsr(0x1B) & ~((1 << 13) - 1));
        if (acpi)
        {
            phy_addr_t base = arch::ACPI::get_local_apic_base();
            if (base != local_apic_base_addr)
            {
                trace::warning("Local APIC base from msr ", trace::hex(local_apic_base_addr()), " from ACPI ",
                               trace::hex(base()));
            }
            local_apic_base_addr = base;
        }

        trace::debug("Local APIC base ", trace::hex(local_apic_base_addr()));
        auto &paging = memory::kernel_vm_info->paging();
        u64 map_base = memory::alloc_io_mmap_address(paging::frame_size::size_2mb, paging::frame_size::size_2mb);

        paging.big_page_map_to(reinterpret_cast<void *>(map_base), paging::big_pages, local_apic_base_addr,
                               paging::flags::cache_disable | paging::flags::writable | paging::flags::write_through,
                               0);
        paging.reload();

        apic_base_addr = reinterpret_cast<void *>(map_base);
    }

    if (cpu_info::has_feature(cpu_info::feature::x2apic))
    {
        // v |= (1 << 10);
    }

    _wrmsr(0x1B, _rdmsr(0x1B) | v);
    kassert((_rdmsr(0x1B) & v) == v, "Can't enable (IA32_APIC_BASE) APIC value ", (void *)_rdmsr(0x1B));

    u64 version_value = read_register(version_register);
    // timer mask
    v = (1 << 8);
    // u16 lvtCount = ((version_value & 0xFF0000) >> 16) + 1;
    u8 version = version_value & 0xFF;

    id = read_register(id_register) >> 24;

    cpu::current().set_apic_id(id);
    if (arch::cpu::current().is_bsp())
    {
        if (version > 0xf)
        {
            trace::debug("Use Intergrated APIC");
            builtin_local_apic = true;
        }
        else
        {
            trace::warning("Use 82489DX");
        }
    }

    if ((version_value & (1 << 24)) != 0)
    {
        v |= 1 << 12; // disable broadcast EOI
    }

    disable_all_lvt();
    // enable software Local APIC
    write_register(spurious_interrupt_vector_register, read_register(spurious_interrupt_vector_register) | v);
    kassert((read_register(spurious_interrupt_vector_register) & v) == v, "Can't software enable local-APIC value ",
            (void *)(u64)read_register(spurious_interrupt_vector_register));
}

u64 local_ID() { return cpu::current().get_apic_id(); }

void local_software_enable()
{
    u32 v = 1 << 8;
    write_register(spurious_interrupt_vector_register, read_register(spurious_interrupt_vector_register) | v);
}

void local_software_disable()
{
    u32 v = 1 << 8;
    write_register(spurious_interrupt_vector_register, read_register(spurious_interrupt_vector_register) & ~v);
}

void local_disable(u8 index)
{
    write_register(lvt_index_array[index], read_register(lvt_index_array[index]) | (1 << 16));
}

void local_irq_setup(u8 index, u8 vector, u8 flags)
{
    write_register(lvt_index_array[index], vector | ((u16)flags << 8));
}

void local_enable(u8 index)
{
    write_register(lvt_index_array[index], read_register(lvt_index_array[index]) & ~(1 << 16));
}

void local_post_init_IPI() { write_register64(icr_0, 0xc2500); }

void local_post_start_up(u64 addr)
{
    addr &= 0x100000 - 1;
    addr >>= 12;
    write_register64(icr_0, 0xc2600 | addr);
}

void local_post_IPI_all(u64 intr)
{
    intr &= 0xFF;
    write_register64(icr_0, intr | (0b01000000u) << 8 | 0b1000u << 16);
}

void local_post_IPI_all_notself(u64 intr)
{
    intr &= 0xFF;
    write_register64(icr_0, intr | (0b01000000u) << 8 | 0b1100u << 16);
}

void local_post_IPI_self(u64 intr)
{
    intr &= 0xFF;
    write_register64(icr_0, intr | (0b01000000u) << 8 | 0b0100u << 16);
}

void local_post_IPI_mask(u64 intr, u64 mask0)
{
    intr &= 0xFF;
    write_register64(icr_0, intr | (0b01000000u) << 8 | mask0 << 56);
}

void local_EOI(u8 index) { write_register(eoi_register, 0); }

irq::request_result _ctx_interrupt_ on_apic_event(const irq::interrupt_info *inter, u64 extra_data, u64 user_data)
{
    auto event = (clock_event *)user_data;

    if (likely(event && !event->is_suspend_.load() && event->id_ == cpu::current().get_apic_id()))
    {
        event->jiff_.fetch_add(1);
        irq::raise_soft_irq(irq::soft_vector::timer);
        return irq::request_result::ok;
    }
    return irq::request_result::no_handled;
}

/*
divide configuration register
000: Divide by 2
001: Divide by 4
010: Divide by 8
011: Divide by 16
100: Divide by 32
101: Divide by 64
110: Divide by 128
111: Divide by 1
*/
constexpr u8 dv_table[] = {2, 4, 8, 16, 32, 64, 128, 1};
inline constexpr u8 divide_value(u8 d) { return dv_table[d]; }

void clock_event::init(u64 HZ)
{
    hz_ = HZ;
    id_ = cpu::current().get_apic_id();
    counter_ = 5'000'000;
    const u64 base_hz = 100'000'000UL;

    // auto freq = cpu_info::get_feature(cpu_info::feature::crystal_frequency);
    if (builtin_local_apic)
    {
        // core freq
        if (cpu_info::max_basic_cpuid() >= 0x16)
        {
            auto freq = cpu_info::get_feature(cpu_info::feature::bus_frequency);
            bus_frequency_ = freq * 1000'000UL;
        }
    }
    else
    {
        // bus freq
        auto scale = (_rdmsr(0xCE) & 0xFF00) >> 8; // MSR PlatformInfo
        if (scale < 100)
        {
            bus_frequency_ = scale * base_hz;
        }
    }

    if (bus_frequency_ == 0)
    {
        bus_frequency_ = base_hz;
    }
    else
    {
        builtin_frequency_ = true;
        trace::debug("load builtin bus frequency ", bus_frequency_ / 1000'000UL, "MHZ");
    }

    is_suspend_ = false;
    suspend();
    local_irq_setup(lvt_index::timer, irq::hard_vector::local_apic_timer, 0);
    write_register(lvt_index_array[lvt_index::timer], read_register(lvt_index_array[lvt_index::timer]) | (0b01 << 17));
}

void clock_event::destroy() {}

void clock_event::suspend()
{
    if (!is_suspend_)
    {
        is_suspend_ = true;
        local_disable(lvt_index::timer);
        {
            uctx::UninterruptibleContext icu;
            write_register(timer_initial_count_register, 0xFFFF'FFFF);
            write_register(timer_divide_register, 0);
        }
        irq::unregister_request_func(irq::hard_vector::local_apic_timer, on_apic_event, (u64)this);
    }
}

void clock_event::resume()
{
    if (is_suspend_)
    {
        is_suspend_ = false;
        jiff_ = 0;
        last_tick_ = 0;

        irq::register_request_func(irq::hard_vector::local_apic_timer, on_apic_event, (u64)this);

        uctx::UninterruptibleContext icu;

        write_register(timer_initial_count_register, counter_);
        write_register(timer_divide_register, divide_);
        write_register(timer_current_count_register, counter_);
        local_enable(lvt_index::timer);
    }
}

void clock_source::init() {}

void clock_source::destroy() {}

u64 clock_source::current()
{
    clock_event *ev = (clock_event *)event;
    u64 tick = count();
    return (tick * 1000'000UL) / ev->bus_frequency_;
}

u64 clock_source::count()
{
    clock_event *ev = (clock_event *)event;
    u32 val;
    u64 jiff;
    {
        uctx::UninterruptibleContext icu;
        val = read_register(timer_current_count_register);
        jiff = ev->jiff_;
    }
    u32 counter = ev->counter_;

    u64 tick = jiff * counter + counter - val;
    u64 last = ev->last_tick_;
    if (tick < last)
    {
        while (tick < last)
        {
            tick += counter;
        }
    }
    ev->last_tick_ = tick;
    return tick * 2;
}

u64 clock_source::jiff()
{
    clock_event *ev = (clock_event *)event;
    return ev->jiff_;
}

// 20ms
constexpr u64 test_duration_us = 200'000;
constexpr size_t test_times = 5;

u64 clock_source::calibrate_apic(::timeclock::clock_source *cs)
{
    auto from_ev = cs->get_event();
    from_ev->resume();
    clock_event *ev = (clock_event *)event;
    ev->resume();

    u64 t = test_duration_us + cs->current();
    u64 start_count = count();

    while (cs->current() < t)
    {
        cpu_pause();
    }

    u64 end_count = count();
    u64 cost = cs->current() - t + test_duration_us;

    from_ev->suspend();
    ev->suspend();

    return (end_count - start_count) * 1000'000UL / ev->counter_ / cost;
}

u64 clock_source::calibrate_counter(::timeclock::clock_source *cs)
{
    auto from_ev = cs->get_event();
    from_ev->resume();
    clock_event *ev = (clock_event *)event;
    ev->resume();

    u64 t = test_duration_us + cs->current();
    u64 start_count = count();
    // u64 tsc = _rdtsc();

    while (cs->current() < t)
    {
        cpu_pause();
    }

    u64 end_count = count();
    u64 cost = cs->current() - t + test_duration_us;

    from_ev->suspend();
    ev->suspend();
    // trace::info("tsc ", (_rdtsc() - tsc) / 100'000UL, " ", end_count - start_count, " ", cost);

    return (end_count - start_count) * 1'000'000UL / cost;
}

std::atomic_uint64_t lapic_freq = 0;

void clock_source::calibrate(::timeclock::clock_source *cs)
{
    clock_event *ev = (clock_event *)event;

    if (lapic_freq == 0)
    {
        if (!ev->builtin_frequency_)
        {
            u64 apic_freq[test_times];
            u64 total_freq = 0;
            u64 max_freq = 0;
            u64 min_freq = std::numeric_limits<u64>::max();
            for (auto &freq : apic_freq)
            {
                freq = calibrate_counter(cs);
                total_freq += freq;
                min_freq = freelibcxx::min(min_freq, freq);
                max_freq = freelibcxx::max(max_freq, freq);
            }
            freelibcxx::sort(apic_freq, apic_freq + test_times);

            const u64 avg_freq = total_freq / test_times;

            const u64 freq = apic_freq[test_times / 2];

            u64 delta = 0;
            for (auto freq : apic_freq)
            {
                i64 d = ((i64)freq - (i64)avg_freq) / 1000'000;
                delta += d * d;
            }
            delta /= test_times;
            trace::info("Local APIC bus test frequency ", freq / 1000'000UL, "MHZ. delta ", delta, " min ",
                        min_freq / 1000'000UL, "MHZ. max ", max_freq / 1000'000UL, "MHZ. ");

            lapic_freq = freq;
            ev->bus_frequency_ = freq;
        }
        else
        {
            lapic_freq = ev->bus_frequency_;
        }
    }
    if (!ev->builtin_frequency_)
    {
        ev->bus_frequency_ = lapic_freq;
    }
    // hz = bus_freq / divide * counter
    // counter = bus_freq / divide / hz
    u32 lapic_counter = ev->bus_frequency_ / (divide_value(ev->divide_)) / ev->hz_;
    if (cpu::current().is_bsp())
    {
        trace::debug("Local APIC set counter ", lapic_counter);
    }

    ev->counter_ = lapic_counter;

    u64 current_freq = calibrate_apic(cs);
    trace::debug("Local APIC Timer ", current_freq, "HZ");

    ev->hz_ = current_freq;
}

clock_source *make_clock()
{
    clock_source *lt_cs = nullptr;
    clock_event *lt_ev = nullptr;

    lt_cs = memory::New<clock_source>(memory::KernelCommonAllocatorV);
    lt_ev = memory::New<clock_event>(memory::KernelCommonAllocatorV);
    lt_ev->set_source(lt_cs);
    lt_cs->set_event(lt_ev);
    lt_ev->init(100);
    lt_cs->init();

    return lt_cs;
}

} // namespace arch::APIC
