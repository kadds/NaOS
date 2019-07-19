#include "kernel/tss.hpp"
#include "kernel/gdt.hpp"
#include "kernel/memory.hpp"
namespace tss
{
const u8 tss_type = 0b1001;
const u8 tss_type_using = 0b1011;
ExportC void _load_tss_descriptor(int tss_index);
tss *tss_ptr;
void init(void *baseAddr, void *ist)
{
    tss_ptr = (tss *)memory::alloc(13 * 8, 8);

    descriptor &tss_descriptor = gdt::get_tss_descriptor(0);

    tss_descriptor.set_offset((u64)tss_ptr);
    tss_descriptor.set_limit(sizeof(tss) - 1);
    tss_descriptor.set_type(tss_type);
    tss_descriptor.set_valid(true);
    tss_descriptor.set_dpl(0);

    _load_tss_descriptor(gdt::get_offset_of_tss());

    tss_ptr->rsp0 = (u64)baseAddr;
    tss_ptr->rsp1 = tss_ptr->rsp0;
    tss_ptr->rsp2 = tss_ptr->rsp0;

    tss_ptr->ist1 = (u64)ist;
    tss_ptr->ist2 = tss_ptr->ist1;
    tss_ptr->ist3 = tss_ptr->ist1;
    tss_ptr->ist4 = tss_ptr->ist1;
    tss_ptr->ist5 = tss_ptr->ist1;
    tss_ptr->ist6 = tss_ptr->ist1;
    tss_ptr->ist7 = tss_ptr->ist1;
    tss_ptr->io_address = 0;
}
} // namespace tss
