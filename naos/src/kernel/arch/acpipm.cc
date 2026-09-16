#include "kernel/arch/acpipm.hpp"

#include "kernel/arch/acpi/acpi.hpp"
#include "kernel/arch/io.hpp"
#include "kernel/arch/paging.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/mm/vm.hpp"
#include "kernel/time/unsigned_math.hpp"
#include "kernel/ucontext.hpp"

namespace arch::device::ACPI
{
namespace
{
constexpr u64 input_frequency_hz = 3'579'545;
constexpr u32 counter_mask_24 = (1u << 24) - 1;

u32 read_register_io(u64 port) noexcept { return io_in32(port); }

u32 read_register_mm(u64 address) noexcept
{
    const u32 value = *reinterpret_cast<volatile u32 *>(address);
    _mfence();
    return value;
}
} // namespace

bool clock::start_cpu() noexcept
{
    if (started_)
        return true;
    if (!arch::ACPI::has_init())
        return false;
    const auto info = arch::ACPI::get_acpipm_base();
    if (!info.has_value())
        return false;

    bit32_mode_ = info.value().bit32mode;
    read_register_ = &read_register_io;
    if (info.value().block_base != 0)
    {
        address_ = info.value().block_base;
    }
    else if (info.value().xblock_base != nullptr && info.value().use_io)
    {
        address_ = reinterpret_cast<u64>(info.value().xblock_base);
    }
    else if (info.value().xblock_base != nullptr)
    {
        const u64 map = memory::alloc_io_mmap_address(memory::page_size, memory::page_size);
        memory::kernel_vm_info->paging().map_to(
            reinterpret_cast<void *>(map), 1, phy_addr_t::from(info.value().xblock_base),
            paging::flags::writable | paging::flags::write_through | paging::flags::cache_disable, 0);
        memory::kernel_vm_info->paging().reload();
        address_ = map;
        read_register_ = &read_register_mm;
    }
    else
    {
        return false;
    }
    initial_count_ = read_register_(address_) & counter_mask_24;
    started_ = true;
    return true;
}

timeclock::nanosecond_t clock::now_ns() noexcept
{
    if (!started_ || read_register_ == nullptr)
        return 0;
    const u32 current = read_register_(address_) & counter_mask_24;
    const u32 mask = bit32_mode_ ? 0xFFFF'FFFFu : counter_mask_24;
    const u32 delta = (current - initial_count_) & mask;
    u64 ns = 0;
    if (!timeclock::unsigned_math::try_mul_div_floor(delta, timeclock::nanoseconds_per_second, input_frequency_hz, ns))
        return static_cast<u64>(-1);
    return ns;
}

clock *make_event_clock() noexcept { return memory::New<clock>(memory::KernelCommonAllocatorV); }

} // namespace arch::device::ACPI
