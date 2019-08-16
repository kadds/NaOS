#include "kernel/arch/interrupt.hpp"
#include "kernel/arch/8259A.hpp"
#include "kernel/arch/gdt.hpp"
#include "kernel/arch/idt.hpp"
#include "kernel/arch/io.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/trace.hpp"
ExportC char _interrupt_wrapper_32[];
ExportC char _interrupt_wrapper_33[];

namespace interrupt
{
void build(int index, void *func) { idt::set_interrupt_system_gate(index, func); }
void init()
{
    int len = (char *)_interrupt_wrapper_33 - (char *)_interrupt_wrapper_32;
    for (int i = 0; i < 16; i++)
    {
        build(i + 32, (char *)_interrupt_wrapper_32 + len * i);
    }
    device::chip8259A::init();
}

ExportC void do_irq(regs_t *regs)
{
    // trace::debug("interrupt vector: ", regs->vector);
    io_out8(0x20, 0x20);
}

} // namespace interrupt
