#include "kernel/arch/tss.hpp"
#include "kernel/arch/cpu.hpp"
#include "kernel/arch/gdt.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/mm/buddy.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/trace.hpp"
namespace arch::tss
{
const u8 tss_type = 0b1001;
const u8 tss_type_using = 0b1011;

tss_t *tss;

void init(int core_index, phy_addr_t base_addr, void *ist)
{
    if (core_index == 0) // bsp
    {
        tss = memory::NewArray<tss_t, memory::IAllocator *, 8>(memory::KernelCommonAllocatorV, cpu::max_cpu_support);
    }

    descriptor &tss_descriptor = gdt::get_tss_descriptor(core_index);

    tss_descriptor.set_offset((u64)(tss + core_index));
    tss_descriptor.set_limit(sizeof(tss_t) - 1);
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

    tss[core_index].io_address = 0;
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

    *((&tss[core_index].ist1_low) + index * 2) = low;
    *((&tss[core_index].ist1_low) + index * 2 + 1) = high;
}

void *get_ist(int core_index, int index)
{
    kassert(index >= 1 && index <= 7, "Index of IST must >= 1 and <= 7.");
    index--;
    u32 low = *((&tss[core_index].ist1_low) + index * 2);
    u32 high = *((&tss[core_index].ist1_low) + index * 2 + 1);
    return (void *)((((u64)high) << 32) | low);
}

void set_rsp(int core_index, int index, void *rsp0)
{
    kassert(index >= 0 && index <= 2, "Index of RSP must >= 0 and <= 2.");
    u32 high = (u64)rsp0 >> 32;
    u32 low = (u64)rsp0;

    *((&tss[core_index].rsp0_low) + index * 2) = low;
    *((&tss[core_index].rsp0_low) + index * 2 + 1) = high;
}

void *get_rsp(int core_index, int index)
{
    kassert(index >= 0 && index <= 2, "Index of RSP must >= 0 and <= 2.");
    u32 low = *((&tss[core_index].rsp0_low) + index * 2);
    u32 high = *((&tss[core_index].rsp0_low) + index * 2 + 1);
    return (void *)((((u64)high) << 32) | low);
}
#pragma GCC diagnostic pop

} // namespace arch::tss
