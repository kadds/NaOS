#include "kernel/arch/interrupt.hpp"
#include "kernel/arch/8259A.hpp"
#include "kernel/arch/gdt.hpp"
#include "kernel/arch/idt.hpp"
#include "kernel/arch/io.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/mm/list_node_cache.hpp"
#include "kernel/trace.hpp"
#include "kernel/util/linked_list.hpp"
ExportC char _interrupt_wrapper_32[];
ExportC char _interrupt_wrapper_33[];

namespace arch::interrupt
{
using idt::regs_t;
arch::idt::call_func global_call_func;

void build(int index, void *func, u8 ist) { idt::set_interrupt_system_gate(index, func, ist); }

void set_callback(arch::idt::call_func func) { global_call_func = func; }

void init()
{
    // Each interrupt entry code has the same size
    int len = (char *)_interrupt_wrapper_33 - (char *)_interrupt_wrapper_32;
    for (int i = 0; i < 16; i++)
    {
        build(i + 32, (char *)_interrupt_wrapper_32 + len * i, 0);
    }
    device::chip8259A::init();
}

ExportC void do_irq(const regs_t *regs)
{
    global_call_func(regs, 0);
    device::chip8259A::send_EOI(regs->vector - 32);
}

} // namespace arch::interrupt
