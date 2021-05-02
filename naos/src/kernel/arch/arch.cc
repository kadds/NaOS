#include "kernel/arch/arch.hpp"
#include "kernel/arch/apic.hpp"
#include "kernel/arch/cpu.hpp"
#include "kernel/arch/cpu_info.hpp"
#include "kernel/arch/dev/serial/8042.hpp"
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

constexpr u64 trace_early_bottom_address = 0x21000;
constexpr u64 trace_early_top_address = 0x30000;

void early_init(const kernel_start_args *args)
{
    if (args != nullptr) /// bsp
    {
        // init serial device for logger when video device is preparing.
        trace::early_init((void *)trace_early_bottom_address, (void *)trace_early_top_address);

        trace::print<trace::PrintAttribute<trace::Color::Foreground::Yellow, trace::Color::Background::Black>>(
            " NaOS: Nano Operating System (arch X86_64)\n");
        trace::print<trace::PrintAttribute<trace::TextAttribute::Reset>>();

        trace::debug("Boot loader is ", (const char *)args->boot_loader_name);
        if (sizeof(kernel_start_args) != args->size_of_struct)
        {
            trace::panic("Kernel args is invalid");
        }

        trace::debug("Arch init");

        cpu_info::init();

        trace::debug("Memory init");
        memory::init(args, 0x0);
    }
}

void init(const kernel_start_args *args)
{
    if (args != nullptr) /// bsp
    {
        cpu::init();

        trace::init();

        trace::debug("Paging init");
        paging::init();

        auto cpu_mesh = cpu_info::get_cpu_mesh_info();
        trace::info("detect cpu logic count ", cpu_mesh.logic_num, " core count ", cpu_mesh.core_num);
        cpu::allocate_stack(cpu_mesh.logic_num);

        phy_addr_t video_start =
            memory::align_down(phy_addr_t::from(device::vga::get_vram()), paging::frame_size::size_2mb);

        // paging::map(paging::current(), (void *)memory::kernel_vga_bottom_address, video_start,
        //             paging::frame_size::size_2mb,
        //             (memory::kernel_vga_top_address - memory::kernel_vga_bottom_address) /
        //             paging::frame_size::size_2mb, paging::flags::writable | paging::flags::write_through |
        //             paging::flags::cache_disable);

        paging::enable_new_paging();

        trace::debug("GDT init");
        gdt::init_after_paging();

        trace::debug("TSS init");
        tss::init(0, phy_addr_t::from(0x0), memory::pa2vax(0x80000));

        cpu::init_data(0);
        trace::debug("IDT init");
        idt::init_after_paging();

        trace::debug("APIC init");
        APIC::init();
        trace::debug("Arch init done");

        device::vga::init();
        // device::vga::set_vram(
        //     (byte *)(memory::kernel_vga_bottom_address + (u64)((byte *)video_start - (byte *)video_start_2mb)));
        return;
    }
    // ap
    auto cpuid = cpu::init();
    paging::init();
    gdt::init_after_paging();
    tss::init(cpuid, phy_addr_t::from(0x0), memory::pa2vax(0x10000));
    cpu::init_data(cpuid);
    idt::init_after_paging();
    APIC::init();
}

void post_init() { device::vga::auto_flush(); }

void init_drivers() { device::chip8042::init(); }

} // namespace arch
