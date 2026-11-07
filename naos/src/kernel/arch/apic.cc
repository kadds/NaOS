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

void EOI(u8 index)
{
    local_EOI(index);
    // IOAPIC directed EOI is needed only for a vector actually routed by an
    // IOAPIC; MSI and software IPIs have no corresponding IOAPIC pin. The
    // vector-to-pin lookup is exact, not derived from the vector number.
    io_EOI(index);
}
} // namespace arch::APIC
