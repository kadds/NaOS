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
#include "kernel/log.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/mm/vm.hpp"
#include "kernel/time/unsigned_math.hpp"
#include "kernel/types.hpp"
#include "kernel/ucontext.hpp"
#include <atomic>
#include <cstddef>

KLOG_MODULE(arch);
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
            KLOG_DEBUG("x2APIC is supported");
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
                KLOG_WARN("Local APIC base from msr {} from ACPI {}", log::hex(local_apic_base_addr()),
                          log::hex(base()));
            }
            local_apic_base_addr = base;
        }

        KLOG_INFO("Local APIC base {}", log::hex(local_apic_base_addr()));
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
    kassert((_rdmsr(0x1B) & v) == v, "Can't enable (IA32_APIC_BASE) APIC value {}", (void *)_rdmsr(0x1B));

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
            KLOG_DEBUG("Use Intergrated APIC");
            builtin_local_apic = true;
        }
        else
        {
            KLOG_WARN("Use 82489DX");
        }
    }

    if ((version_value & (1 << 24)) != 0)
    {
        v |= 1 << 12; // disable broadcast EOI
    }

    disable_all_lvt();
    // enable software Local APIC
    write_register(spurious_interrupt_vector_register, read_register(spurious_interrupt_vector_register) | v);
    kassert((read_register(spurious_interrupt_vector_register) & v) == v, "Can't software enable local-APIC value {}",
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

namespace
{
constexpr u64 default_bus_frequency_hz = 100'000'000;
constexpr u64 platform_info_msr = 0xCE;
constexpr u64 calibration_duration_ns = 20'000'000;
} // namespace

irq::request_result event_source::on_interrupt(const irq::interrupt_info *, u64) noexcept
{
    if (!started_.load(std::memory_order_acquire) || cpu_id_ != cpu::current().get_id())
        return irq::request_result::no_handled;

    if (!armed_.exchange(false, std::memory_order_acq_rel))
        return irq::request_result::no_handled;

    armed_generation_.store(0, std::memory_order_release);
    local_disable(lvt_index::timer);
    program_counter(0);
    irq::raise_soft_irq(irq::soft_vector::timer);
    return irq::request_result::ok;
}

void event_source::refresh_frequency() noexcept
{
    bus_frequency_ = 0;
    if (builtin_local_apic && cpu_info::max_basic_cpuid() >= 0x16)
    {
        bus_frequency_ = cpu_info::get_feature(cpu_info::feature::bus_frequency) * 1'000'000ULL;
    }
    else
    {
        const u64 scale = (_rdmsr(platform_info_msr) >> 8) & 0xFF;
        if (scale != 0 && scale < 100)
            bus_frequency_ = scale * default_bus_frequency_hz;
    }
}

void event_source::program_counter(u32 ticks) noexcept
{
    uctx::UninterruptibleContext context;
    write_register(timer_divide_register, divide_);
    write_register(timer_initial_count_register, ticks);
}

bool event_source::calibrate_frequency(::timeclock::event_clock &clock) noexcept
{
    program_counter(maximum_counter);
    const u64 start_ns = clock.now_ns();
    const u32 start_count = read_register(timer_current_count_register);
    u64 deadline = 0;
    if (!timeclock::try_add_nanoseconds(start_ns, calibration_duration_ns, deadline))
    {
        program_counter(0);
        return false;
    }
    while (clock.now_ns() < deadline)
        cpu_pause();
    const u64 elapsed_ns = clock.now_ns() - start_ns;
    const u32 end_count = read_register(timer_current_count_register);
    const u64 elapsed_ticks = static_cast<u32>(start_count - end_count);
    program_counter(0);
    if (elapsed_ns == 0 || elapsed_ticks == 0)
        return false;

    u64 frequency = 0;
    if (!timeclock::unsigned_math::try_mul_div_floor(elapsed_ticks, timeclock::nanoseconds_per_second, elapsed_ns,
                                                     frequency) ||
        frequency == 0)
        return false;
    bus_frequency_ = frequency;
    return true;
}

bool event_source::calibrate(::timeclock::event_clock &reference) noexcept
{
    if (started_.load(std::memory_order_acquire))
        return false;
    if (bus_frequency_ != 0)
        return true;

    refresh_frequency();
    if (bus_frequency_ != 0)
        return true;

    local_irq_setup(lvt_index::timer, irq::hard_vector::local_apic_timer, 0); // one-shot mode
    local_disable(lvt_index::timer);
    return calibrate_frequency(reference);
}

bool event_source::start_cpu() noexcept
{
    if (started_.load(std::memory_order_acquire))
        return true;

    cpu_id_ = cpu::current().get_id();
    if (bus_frequency_ == 0)
        refresh_frequency();
    local_irq_setup(lvt_index::timer, irq::hard_vector::local_apic_timer, 0); // one-shot mode
    local_disable(lvt_index::timer);
    if (bus_frequency_ == 0)
        return false;

    program_counter(0);
    armed_.store(false, std::memory_order_release);
    armed_generation_.store(0, std::memory_order_release);
    irq_registration_ = irq::register_handler(irq::hard_vector::local_apic_timer,
                                              irq::hard_handler::bind<&event_source::on_interrupt>(*this));
    if (!irq_registration_)
        return false;
    started_.store(true, std::memory_order_release);
    return true;
}

void event_source::stop_cpu() noexcept
{
    armed_.store(false, std::memory_order_release);
    armed_generation_.store(0, std::memory_order_release);
    generation_.fetch_add(1, std::memory_order_relaxed);
    if (!started_.load(std::memory_order_acquire) && !irq_registration_)
        return;
    local_disable(lvt_index::timer);
    program_counter(0);
    irq_registration_.reset();
    started_.store(false, std::memory_order_release);
}

::timeclock::arm_result event_source::arm(const ::timeclock::deadline_request &request) noexcept
{
    if (!started_.load(std::memory_order_acquire) || bus_frequency_ == 0)
        return ::timeclock::arm_result::unavailable;
    if (!request.is_valid())
        return ::timeclock::arm_result::invalid;

    const u64 current_generation = generation_.load(std::memory_order_relaxed);
    if (request.generation <= current_generation)
        return ::timeclock::arm_result::invalid;

    const auto conversion =
        timeclock::event_source::convert_deadline(request.delta_ns(), bus_frequency_, maximum_counter);
    local_disable(lvt_index::timer);
    program_counter(conversion.ticks);
    generation_.store(request.generation, std::memory_order_relaxed);
    armed_generation_.store(request.generation, std::memory_order_release);
    armed_.store(true, std::memory_order_release);
    local_enable(lvt_index::timer);
    return ::timeclock::arm_result::armed;
}

void event_source::cancel(u64 generation) noexcept
{
    if (generation == 0 || generation < generation_.load(std::memory_order_relaxed))
        return;

    generation_.store(generation, std::memory_order_relaxed);
    armed_generation_.store(0, std::memory_order_release);
    armed_.store(false, std::memory_order_release);
    if (!started_.load(std::memory_order_acquire))
        return;

    local_disable(lvt_index::timer);
    program_counter(0);
}

event_source *make_event_source() noexcept { return memory::New<event_source>(memory::KernelCommonAllocatorV); }

} // namespace arch::APIC
