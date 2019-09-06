#include "kernel/arch/arch.hpp"
#include "kernel/arch/cpu.hpp"
#include "kernel/arch/gdt.hpp"
#include "kernel/arch/idt.hpp"
#include "kernel/arch/paging.hpp"
#include "kernel/arch/tss.hpp"
#include "kernel/arch/video/vga/vga.hpp"
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
    device::vga::init(args);
    if (sizeof(kernel_start_args) != args->size_of_struct)
    {
        trace::panic("kernel args is invalid.");
    }

    trace::debug("Kernel starting...");
    cpu::init();

    trace::debug("Memory init...");
    memory::init(args, 0x0);

    trace::debug("Paging init...");
    device::vga::flush();
    paging::init();
    void *video_start = device::vga::get_video_addr();

    paging::map(paging::base_kernel_page_addr, (void *)0xFFFFE00000000000, video_start, paging::frame_size::size_4kb,
                (0xFFFFE000FFFFFFFF - 0xFFFFE00000000000) / paging::frame_size::size_4kb,
                paging::flags::writable | paging::flags::write_through | paging::flags::cache_disable);
    paging::load(paging::base_kernel_page_addr);

    device::vga::set_video_addr((void *)0xFFFFE00000000000);
    device::vga::flush();

    trace::debug("GDT init...");
    gdt::init_after_paging();

    trace::debug("TSS init...");
    tss::init((void *)0x9fff, (void *)0x6000);

    trace::debug("IDT init...");
    idt::init_after_paging();
}

} // namespace arch
