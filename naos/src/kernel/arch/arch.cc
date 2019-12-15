#include "kernel/arch/arch.hpp"
#include "kernel/arch/apic.hpp"
#include "kernel/arch/cpu.hpp"
#include "kernel/arch/cpu_info.hpp"
#include "kernel/arch/gdt.hpp"
#include "kernel/arch/idt.hpp"
#include "kernel/arch/paging.hpp"
#include "kernel/arch/tss.hpp"
#include "kernel/arch/video/vga/vga.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/mm/mm.hpp"
#include "kernel/trace.hpp"
namespace arch
{
ExportC Unpaged_Text_Section void temp_init(const kernel_start_args *args)
{
    gdt::init_before_paging(args != 0);
    paging::temp_init(args != 0);
}

void init(const kernel_start_args *args)
{
    if (args != nullptr) /// bsp
    {
        trace::early_init();
        device::vga::init();
        trace::print<trace::PrintAttribute<trace::Color::Foreground::Yellow, trace::Color::Background::Black>>(
            " NaOS: Nano Operating System (arch X86_64)\n");
        trace::print<trace::PrintAttribute<trace::TextAttribute::Reset>>();

        trace::debug("Boot from ", (const char *)args->boot_loader_name);
        if (sizeof(kernel_start_args) != args->size_of_struct)
        {
            trace::panic("Kernel args is invalid.");
        }
        trace::debug("Arch init start...");
        cpu_info::init();
        if (!cpu_info::has_future(cpu_info::future::system_call_ret))
        {
            trace::panic("Cpu doesn 't support syscall/sysret instructions.");
        }
        if (!cpu_info::has_future(cpu_info::future::apic))
        {
            trace::panic("Cpu doesn 't support APIC.");
        }
        auto cpuid = cpu::init();

        trace::debug("Memory init...");
        memory::init(args, 0x0);
        trace::init();
        device::vga::tag_memory();

        trace::debug("Paging init...");
        device::vga::flush();
        paging::init();
        void *video_start = device::vga::get_vram();
        void *video_start_2mb = (void *)(((u64)video_start) & ~(paging::frame_size::size_2mb - 1));

        paging::map(paging::current(), (void *)memory::kernel_vga_bottom_address, video_start_2mb,
                    paging::frame_size::size_2mb,
                    (memory::kernel_vga_top_address - memory::kernel_vga_bottom_address) / paging::frame_size::size_2mb,
                    paging::flags::writable | paging::flags::write_through | paging::flags::cache_disable);
        paging::reload();

        device::vga::set_vram(
            (byte *)(memory::kernel_vga_bottom_address + (u64)((byte *)video_start - (byte *)video_start_2mb)));

        device::vga::flush();

        trace::debug("GDT init...");
        gdt::init_after_paging();

        trace::debug("TSS init...");
        tss::init(cpuid, (void *)0x0, memory::kernel_phyaddr_to_virtaddr((void *)0x10000));
        cpu::init_data(cpuid);
        trace::debug("IDT init...");
        idt::init_after_paging();
        trace::debug("APIC init...");
        APIC::init();

        trace::debug("Arch init end");
        return;
    }
    auto cpuid = cpu::init();
    paging::init();
    gdt::init_after_paging();
    tss::init(cpuid, (void *)0x0, memory::kernel_phyaddr_to_virtaddr((void *)0x10000));
    cpu::init_data(cpuid);
    idt::init_after_paging();
    APIC::init();
}

void last_init() { device::vga::auto_flush(); }
} // namespace arch
