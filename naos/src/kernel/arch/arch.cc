#include "kernel/arch/arch.hpp"
#include "common.hpp"
#include "kernel/arch/acpi/acpi.hpp"
#include "kernel/arch/apic.hpp"
#include "kernel/arch/cpu.hpp"
#include "kernel/arch/cpu_info.hpp"
#include "kernel/arch/dev/serial/8042.hpp"
#include "kernel/arch/gdt.hpp"
#include "kernel/arch/idt.hpp"
#include "kernel/arch/io_apic.hpp"
#include "kernel/arch/local_apic.hpp"
#include "kernel/arch/paging.hpp"
#include "kernel/arch/tss.hpp"
#include "kernel/cmdline.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/mm/mm.hpp"
#include "kernel/terminal.hpp"
#include "kernel/trace.hpp"
namespace arch
{
ExportC Unpaged_Text_Section void temp_init(const kernel_start_args *args)
{
    gdt::init_before_paging(args != 0);
    paging::temp_init(args != 0);
}

void early_init(kernel_start_args *args)
{
    if (args != nullptr) /// bsp
    {
        // init serial device for logger when video device is preparing.
        if (args->fb_type != 1)
        {
            while (true)
                ;
            // trace::warning("fb type not support");
        }

        term::early_init(args);

        trace::early_init();

        trace::print<trace::PrintAttribute<trace::Color::Foreground::Yellow, trace::Color::Background::Black>>(
            " NaOS: Nano Operating System (arch X86_64)\n");
        trace::print<trace::PrintAttribute<trace::TextAttribute::Reset>>();

        trace::info((u32)args->fb_width, "x", (u32)args->fb_height);

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

        cpu::allocate_bsp_stack();

        trace::debug("GDT init");
        gdt::init_after_paging();

        trace::debug("TSS init");
        tss::init(0, phy_addr_t::from(0x0), memory::pa2va(phy_addr_t::from(0x80000)));

        cpu::init_data(0);
        trace::debug("IDT init");
        idt::init_after_paging();

        term::reset_early_paging();
        idt::enable();
        memory::kernel_vm_info->paging().load();

        memory::init2();

        if (cmdline::get_bool("acpi", false))
        {
            trace::debug("ACPI init");
            ACPI::init();
        }

        trace::debug("APIC init");
        APIC::local_init();
        APIC::io_init();

        trace::debug("Arch init done");
        return;
    }
    // ap
    auto cpuid = cpu::init();
    paging::init_ap();
    gdt::init_after_paging();
    tss::init(cpuid, phy_addr_t::from(0x0), memory::pa2va(phy_addr_t::from(0x10000)));
    cpu::init_data(cpuid);
    idt::init_after_paging();
    APIC::local_init();
}

void init2()
{
    if (cpu::current().is_bsp())
    {
        if (cmdline::get_bool("acpi", false))
        {
            ACPI::enable();
        }
    }
}
void post_init()
{
    if (cpu::current().is_bsp())
    {
    }
}

void init_drivers() { device::chip8042::init(); }

} // namespace arch
