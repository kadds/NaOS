#include "kernel/tss.hpp"
#include "kernel/gdt.hpp"
#include "kernel/mm/memory.hpp"
namespace tss
{
const u8 tss_type = 0b1001;
const u8 tss_type_using = 0b1011;
ExportC void _load_tss_descriptor(int tss_index);
tss *tss_ptr;
void init(void *baseAddr, void *ist)
{
    tss_ptr = memory::New<tss, 8>(memory::VirtBootAllocatorV);
    descriptor &tss_descriptor = gdt::get_tss_descriptor(0);

    tss_descriptor.set_offset(memory::kernel_virtaddr_to_phyaddr((u64)tss_ptr));
    tss_descriptor.set_limit(sizeof(tss) - 1);
    tss_descriptor.set_type(tss_type);
    tss_descriptor.set_valid(true);
    tss_descriptor.set_dpl(0);

    _load_tss_descriptor(gdt::get_offset_of_tss());
    tss *vtss = tss_ptr;
    vtss->rsp0 = (u64)baseAddr;
    vtss->rsp1 = vtss->rsp0;
    vtss->rsp2 = vtss->rsp0;

    vtss->ist1 = (u64)ist;
    vtss->ist2 = vtss->ist1;
    vtss->ist3 = vtss->ist1;
    vtss->ist4 = vtss->ist1;
    vtss->ist5 = vtss->ist1;
    vtss->ist6 = vtss->ist1;
    vtss->ist7 = vtss->ist1;
    vtss->io_address = 0;
}
} // namespace tss
