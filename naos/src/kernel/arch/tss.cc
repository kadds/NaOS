#include "kernel/arch/tss.hpp"
#include "kernel/arch/gdt.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/mm/buddy.hpp"
#include "kernel/mm/memory.hpp"
namespace arch::tss
{
const u8 tss_type = 0b1001;
const u8 tss_type_using = 0b1011;

tss_t *tss;

void init(void *baseAddr, void *ist)
{
    tss = memory::New<tss_t, 8>(memory::KernelBuddyAllocatorV);
    descriptor &tss_descriptor = gdt::get_tss_descriptor(0);

    tss_descriptor.set_offset((u64)tss);
    tss_descriptor.set_limit(sizeof(tss_t) - 1);
    tss_descriptor.set_type(tss_type);
    tss_descriptor.set_valid(true);
    tss_descriptor.set_dpl(0);

    __asm__ __volatile__("ltr %0	\n\t" : : "r"((u16)gdt::get_offset_of_tss()) : "memory");

    set_rsp(0, baseAddr);
    set_rsp(1, baseAddr);
    set_rsp(2, baseAddr);
    set_ist(1, ist);
    set_ist(2, ist);
    set_ist(3, ist);
    set_ist(4, ist);
    set_ist(5, ist);
    set_ist(6, ist);
    set_ist(7, ist);

    tss->io_address = 0;
}
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Waddress-of-packed-member"
void set_ist(int index, void *ist)
{
    kassert(index >= 1 && index <= 7, "Index of IST must >= 1 and <= 7.");
    index--;

    u32 high = (u64)ist >> 32;
    u32 low = (u64)ist;

    *((&tss->ist1_low) + index * 2) = low;
    *((&tss->ist1_low) + index * 2 + 1) = high;
}

void *get_ist(int index)
{
    kassert(index >= 1 && index <= 7, "Index of IST must >= 1 and <= 7.");
    index--;
    u32 low = *((&tss->ist1_low) + index * 2);
    u32 high = *((&tss->ist1_low) + index * 2 + 1);
    return (void *)((((u64)high) << 32) | low);
}

void set_rsp(int index, void *rsp0)
{
    kassert(index >= 0 && index <= 2, "Index of RSP must >= 0 and <= 2.");
    u32 high = (u64)rsp0 >> 32;
    u32 low = (u64)rsp0;

    *((&tss->rsp0_low) + index * 2) = low;
    *((&tss->rsp0_low) + index * 2 + 1) = high;
}

void *get_rsp(int index)
{
    kassert(index >= 0 && index <= 2, "Index of RSP must >= 0 and <= 2.");
    u32 low = *((&tss->rsp0_low) + index * 2);
    u32 high = *((&tss->rsp0_low) + index * 2 + 1);
    return (void *)((((u64)high) << 32) | low);
}
#pragma GCC diagnostic pop

} // namespace arch::tss
