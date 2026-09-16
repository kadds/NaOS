#include "kernel/arch/hpet.hpp"

#include "kernel/arch/acpi/acpi.hpp"
#include "kernel/arch/paging.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/mm/vm.hpp"
#include "kernel/time/unsigned_math.hpp"

namespace arch::device::HPET
{

bool clock::start_cpu() noexcept
{
    if (started_)
        return true;
    const auto physical_base = arch::ACPI::get_hpet_base();
    if (!physical_base.has_value())
        return false;

    const u64 map_start = memory::alloc_io_mmap_address(paging::frame_size::size_4kb, paging::frame_size::size_4kb);
    memory::kernel_vm_info->paging().map_to(
        reinterpret_cast<void *>(map_start), 1, physical_base.value(),
        paging::flags::cache_disable | paging::flags::writable | paging::flags::write_through, 0);
    memory::kernel_vm_info->paging().reload();
    base_ = reinterpret_cast<volatile u64 *>(map_start);

    const u64 capabilities = base_[0];
    const u32 period_femtoseconds = static_cast<u32>(capabilities >> 32);
    if (period_femtoseconds == 0 || (capabilities & 0x8000u) == 0 || (capabilities & 0x2000u) == 0)
        return false;
    frequency_hz_ = 1'000'000'000'000'000ULL / period_femtoseconds;
    if (frequency_hz_ == 0)
        return false;

    // Disable legacy timer comparators and start the main counter.  No HPET
    // interrupt is used for timekeeping; Local APIC is the event_source.
    const u8 timer_count = static_cast<u8>(((capabilities >> 8) & 0x7) + 1);
    for (u8 index = 0; index < timer_count; index++)
        base_[32 + 4 * index] &= ~0xC10EULL;
    base_[2] = (base_[2] & ~0x3ULL) | 0x1ULL;
    initial_counter_ = base_[30];
    started_ = true;
    return true;
}

timeclock::nanosecond_t clock::now_ns() noexcept
{
    if (!started_ || frequency_hz_ == 0)
        return 0;
    const u64 counter = base_[30] - initial_counter_;
    u64 ns = 0;
    if (!timeclock::unsigned_math::try_mul_div_floor(counter, timeclock::nanoseconds_per_second, frequency_hz_, ns))
        return static_cast<u64>(-1);
    return ns;
}

clock *make_event_clock() noexcept { return memory::New<clock>(memory::KernelCommonAllocatorV); }

} // namespace arch::device::HPET
