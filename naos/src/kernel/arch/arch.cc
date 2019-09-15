#include "kernel/arch/arch.hpp"
#include "kernel/arch/cpu.hpp"
#include "kernel/arch/cpu_info.hpp"
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
    cpu_info::init();
    if (!cpu_info::has_future(cpu_info::future::system_call_ret))
    {
        trace::panic("Cpu doesn 't support syscall/sysret instructions.");
    }
    cpu::init(0);

    trace::debug("Memory init...");
    memory::init(args, 0x0);
    device::vga::tag_memory();

    trace::debug("Paging init...");
    device::vga::flush();
    paging::init();
    void *video_start = device::vga::get_video_addr();
    void *video_start_2mb = (void *)(((u64)video_start) & ~(paging::frame_size::size_2mb - 1));

    paging::map(paging::base_kernel_page_addr, (void *)0xFFFFE00000000000, video_start_2mb,
                paging::frame_size::size_2mb, (0xFFFFE000FFFFFFFF - 0xFFFFE00000000000) / paging::frame_size::size_2mb,
                paging::flags::writable | paging::flags::write_through | paging::flags::cache_disable);
    paging::load(paging::base_kernel_page_addr);

    device::vga::set_video_addr((void *)(0xFFFFE00000000000 + (u64)((char *)video_start - (char *)video_start_2mb)));

    device::vga::flush();

    trace::debug("GDT init...");
    gdt::init_after_paging();

    trace::debug("TSS init...");
    tss::init((void *)0x0, (void *)0xFFFF800000003000);

    trace::debug("IDT init...");
    idt::init_after_paging();
}

} // namespace arch
