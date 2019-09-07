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
    memory::BuddyAllocator alloc(memory::zone_t::prop::present);
    tss = memory::New<tss_t, 8>(&alloc);
    descriptor &tss_descriptor = gdt::get_tss_descriptor(0);

    tss_descriptor.set_offset(memory::kernel_virtaddr_to_phyaddr((u64)tss));
    tss_descriptor.set_limit(sizeof(tss_t) - 1);
    tss_descriptor.set_type(tss_type);
    tss_descriptor.set_valid(true);
    tss_descriptor.set_dpl(0);

    _load_tss_descriptor(gdt::get_offset_of_tss());

    tss->rsp0 = (u64)baseAddr;
    tss->rsp1 = tss->rsp0;
    tss->rsp2 = tss->rsp0;

    tss->ist1 = (u64)ist;
    tss->ist2 = tss->ist1;
    tss->ist3 = tss->ist1;
    tss->ist4 = tss->ist1;
    tss->ist5 = tss->ist1;
    tss->ist6 = tss->ist1;
    tss->ist7 = tss->ist1;
    tss->io_address = 0;
}
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Waddress-of-packed-member"
void set_ist(int index, void *ist)
{
    kassert(index >= 1 && index <= 7, "Index of IST must >= 1 and <= 7.");
    *((&tss->ist1) + index - 1) = (u64)ist;
}

void *get_ist(int index)
{
    kassert(index >= 1 && index <= 7, "Index of IST must >= 1 and <= 7.");
    return (void *)*((&tss->ist1) + index - 1);
}

void set_rsp(int index, void *rsp0)
{
    kassert(index >= 0 && index <= 2, "Index of RSP must >= 0 and <= 2.");
    *(&tss->rsp0 + index) = (u64)rsp0;
}

void *get_rsp(int index)
{
    kassert(index >= 0 && index <= 2, "Index of RSP must >= 0 and <= 2.");
    return (void *)*(&tss->rsp0 + index);
}
#pragma GCC diagnostic pop

} // namespace arch::tss
