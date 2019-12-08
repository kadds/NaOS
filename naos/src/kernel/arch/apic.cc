#include "kernel/arch/apic.hpp"
#include "kernel/arch/cpu.hpp"
#include "kernel/arch/idt.hpp"
#include "kernel/arch/io.hpp"
#include "kernel/arch/io_apic.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/arch/local_apic.hpp"
#include "kernel/mm/memory.hpp"
namespace arch::APIC
{
void init()
{
    local_init();
    if (cpu::current().is_bsp())
    {
        io_init();
    }
}

void EOI(u8 index)
{
    // if (index >= 0x0)

    local_EOI(index);
}
} // namespace arch::APIC