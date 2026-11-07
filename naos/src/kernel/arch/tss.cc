#include "kernel/arch/tss.hpp"
#include "kernel/arch/cpu.hpp"
#include "kernel/arch/gdt.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/arch/tss_bitmap.hpp"
#include "kernel/log.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/mm/new.hpp"

KLOG_MODULE(arch);

namespace arch::tss
{
const u8 tss_type = 0b1001;
const u8 tss_type_using = 0b1011;

tss_storage *tss;

void init(int core_index, phy_addr_t base_addr, void *ist)
{
    if (core_index == 0) // bsp
    {
        // The bitmap makes the per-CPU TSS larger than kmalloc's slab size.
        // Use page-backed kernel memory so the allocation is contiguous and
        // remains addressable by the TSS descriptor.
        auto *storage = static_cast<tss_storage *>(
            memory::KernelBuddyAllocatorV->allocate(sizeof(tss_storage) * cpu::max_cpu_support, alignof(tss_storage)));
        tss = storage;
        for (u32 index = 0; index < cpu::max_cpu_support; index++)
            new (&tss[index]) tss_storage{};
    }

    descriptor &tss_descriptor = gdt::get_tss_descriptor(core_index);

    tss_descriptor.set_offset((u64)(&tss[core_index].tss));
    // Include the terminating 0xff byte in the descriptor limit.  The
    // no-lease base is immediately after it, so every CPL3 I/O instruction
    // faults even if a stale bitmap bit were ever observed.
    tss_descriptor.set_limit(detail::io_bitmap_descriptor_limit);
    tss_descriptor.set_type(tss_type);
    tss_descriptor.set_valid(true);
    tss_descriptor.set_dpl(0);

    u16 tss_index = (u16)gdt::get_offset_of_tss() + core_index * sizeof(descriptor);

    __asm__ __volatile__("ltr %0	\n\t" : : "r"(tss_index) : "memory");

    set_rsp(core_index, 0, base_addr());
    set_rsp(core_index, 1, base_addr());
    set_rsp(core_index, 2, base_addr());
    set_ist(core_index, 1, ist);
    set_ist(core_index, 2, ist);
    set_ist(core_index, 3, ist);
    set_ist(core_index, 4, ist);
    set_ist(core_index, 5, ist);
    set_ist(core_index, 6, ist);
    set_ist(core_index, 7, ist);

    detail::deny_all(tss[core_index]);
}

bool install_io_lease(int core_index, u16 first_port, u32 port_count)
{
    const io_range range{first_port, port_count};
    return install_io_leases(core_index, &range, 1);
}

bool install_io_leases(int core_index, const io_range *ranges, u32 range_count)
{
    if (core_index < 0 || static_cast<u32>(core_index) >= cpu::max_cpu_support || range_count > max_direct_io_ranges ||
        (range_count != 0 && ranges == nullptr))
        return false;
    for (u32 index = 0; index < range_count; index++)
    {
        if (ranges[index].port_count == 0 ||
            ranges[index].port_count > 0x1'0000u - static_cast<u32>(ranges[index].first_port))
            return false;
    }
    if (range_count == 0)
        detail::deny_all(tss[core_index]);
    else
        detail::allow_ranges(tss[core_index], ranges, range_count);
    return true;
}

void revoke_io_lease(int core_index)
{
    if (core_index < 0 || static_cast<u32>(core_index) >= cpu::max_cpu_support)
        return;
    detail::deny_all(tss[core_index]);
}

void add(int cpu, void *baseAddr, void *ist) {}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Waddress-of-packed-member"
void set_ist(int core_index, int index, void *ist)
{
    kassert(index >= 1 && index <= 7, "Index of IST must >= 1 and <= 7.");
    index--;

    u32 high = (u64)ist >> 32;
    u32 low = (u64)ist;

    *((&tss[core_index].tss.ist1_low) + index * 2) = low;
    *((&tss[core_index].tss.ist1_low) + index * 2 + 1) = high;
}

void *get_ist(int core_index, int index)
{
    kassert(index >= 1 && index <= 7, "Index of IST must >= 1 and <= 7.");
    index--;
    u32 low = *((&tss[core_index].tss.ist1_low) + index * 2);
    u32 high = *((&tss[core_index].tss.ist1_low) + index * 2 + 1);
    return (void *)((((u64)high) << 32) | low);
}

void set_rsp(int core_index, int index, void *rsp0)
{
    kassert(index >= 0 && index <= 2, "Index of RSP must >= 0 and <= 2.");
    u32 high = (u64)rsp0 >> 32;
    u32 low = (u64)rsp0;

    *((&tss[core_index].tss.rsp0_low) + index * 2) = low;
    *((&tss[core_index].tss.rsp0_low) + index * 2 + 1) = high;
}

void *get_rsp(int core_index, int index)
{
    kassert(index >= 0 && index <= 2, "Index of RSP must >= 0 and <= 2.");
    u32 low = *((&tss[core_index].tss.rsp0_low) + index * 2);
    u32 high = *((&tss[core_index].tss.rsp0_low) + index * 2 + 1);
    return (void *)((((u64)high) << 32) | low);
}
#pragma GCC diagnostic pop

} // namespace arch::tss
