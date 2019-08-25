#include "kernel/arch/arch.hpp"
#include "kernel/arch/cpu.hpp"
#include "kernel/arch/gdt.hpp"
#include "kernel/arch/idt.hpp"
#include "kernel/arch/paging.hpp"
#include "kernel/arch/tss.hpp"
#include "kernel/arch/vbe.hpp"
#include "kernel/mm/memory.hpp"
namespace arch
{
ExportC Unpaged_Text_Section void temp_init(const kernel_start_args *args)
{
    gdt::init_before_paging();
    paging::temp_init();
}
void init(const kernel_start_args *args)
{
    cpu::init();
    memory::init(args, 0x0);
    device::vbe::init();
    cpu::trace_debug_info();
    paging::init();

    gdt::init_after_paging();
    tss::init((void *)0x9fff, (void *)0x6000);
    idt::init_after_paging();
}

} // namespace arch
