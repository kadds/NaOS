#include "kernel/exception.hpp"
#include "kernel/ScreenPrinter.hpp"
#include "kernel/idt.hpp"
namespace exception
{
ExportC u64 _get_cr2();

ExportC void _divide_error_warpper();
ExportC void _debug_warpper();
ExportC void _nmi_warpper();
ExportC void _int3_warpper();

ExportC void _overflow_warpper();
ExportC void _bounds_warpper();
ExportC void _undefined_opcode_warpper();
ExportC void _dev_not_available_warpper();
ExportC void _double_fault_warpper();
ExportC void _coprocessor_segment_overrun_warpper();
ExportC void _invalid_TSS_warpper();
ExportC void _segment_not_present_warpper();
ExportC void _stack_segment_fault_warpper();
ExportC void _general_protection_warpper();
ExportC void _page_fault_warpper();
ExportC void _x87_FPU_error_warpper();
ExportC void _alignment_check_warpper();
ExportC void _machine_check_warpper();
ExportC void _SIMD_exception_warpper();
ExportC void _virtualization_exception_warpper();
void print_dst(idt::regs *regs)
{
    gPrinter->printf("exception at: 0x%x%x, error code: %u!\n", (u32)(regs->rip >> 32), (u32)(regs->rip),
                     regs->error_code);
}
ExportC void entry_divide_error(idt::regs *regs)
{
    gPrinter->printf("divide error. ");
    print_dst(regs);
    while (1)
        ;
}
ExportC void entry_debug(idt::regs *regs)
{
    gPrinter->printf("debug trap. ");
    print_dst(regs);
    while (1)
        ;
}
ExportC void entry_nmi(idt::regs *regs)
{
    gPrinter->printf("nmi error! ");
    print_dst(regs);
    while (1)
        ;
}
ExportC void entry_int3(idt::regs *regs)
{
    gPrinter->printf("int3 trap. ");
    print_dst(regs);
    while (1)
        ;
}
ExportC void entry_overflow(idt::regs *regs)
{
    gPrinter->printf("overflow trap. ");
    print_dst(regs);
    while (1)
        ;
}
ExportC void entry_bounds(idt::regs *regs)
{
    gPrinter->printf("out of bounds error. ");
    print_dst(regs);
    while (1)
        ;
}
ExportC void entry_undefined_opcode(idt::regs *regs)
{
    gPrinter->printf("undefined opcode error. ");
    print_dst(regs);
    while (1)
        ;
}
ExportC void entry_dev_not_available(idt::regs *regs)
{
    gPrinter->printf("dev not available error. ");
    print_dst(regs);
    while (1)
        ;
}
ExportC void entry_double_fault(idt::regs *regs)
{
    gPrinter->printf("double abort. ");
    print_dst(regs);
    while (1)
        ;
}
ExportC void entry_coprocessor_segment_overrun(idt::regs *regs)
{
    gPrinter->printf("coprocessor segment overrun error. ");
    print_dst(regs);
    while (1)
        ;
}
ExportC void entry_invalid_TSS(idt::regs *regs)
{
    gPrinter->printf("invalid tss error. ");
    print_dst(regs);
    while (1)
        ;
}
ExportC void entry_segment_not_present(idt::regs *regs)
{
    gPrinter->printf("segment not present error. ");
    print_dst(regs);
    while (1)
        ;
}
ExportC void entry_stack_segment_fault(idt::regs *regs)
{
    gPrinter->printf("stack segment fault. ");
    print_dst(regs);
    while (1)
        ;
}
ExportC void entry_general_protection(idt::regs *regs)
{
    gPrinter->printf("general protection error. ");
    print_dst(regs);
    while (1)
        ;
}
ExportC void entry_page_fault(idt::regs *regs)
{
    gPrinter->printf("page fault. ");
    print_dst(regs);
    u64 cr2 = _get_cr2();

    while (1)
        ;
}
ExportC void entry_x87_FPU_error(idt::regs *regs)
{
    gPrinter->printf("X87 fpu error. ");
    print_dst(regs);
    while (1)
        ;
}
ExportC void entry_alignment_check(idt::regs *regs)
{
    gPrinter->printf("alignment error. ");
    print_dst(regs);
    while (1)
        ;
}
ExportC void entry_machine_check(idt::regs *regs)
{
    gPrinter->printf("machine error. ");
    print_dst(regs);
    while (1)
        ;
}
ExportC void entry_SIMD_exception(idt::regs *regs)
{
    gPrinter->printf("SIMD error. ");
    print_dst(regs);
    while (1)
        ;
}
ExportC void entry_virtualization_exception(idt::regs *regs)
{
    gPrinter->printf("virtualization error. ");
    print_dst(regs);
    while (1)
        ;
}

void set_trap_system_gate(int index, void *func) { idt::set_exception_entry(index, func, 0x8, 0, 1); }
void set_trap_gate(int index, void *func) { idt::set_exception_entry(index, func, 0x18, 3, 1); }
void set_interrupt_system_gate(int index, void *func) { idt::set_interrupt_entry(index, func, 0x8, 0, 1); }
void set_interrupt_gate(int index, void *func) { idt::set_interrupt_entry(index, func, 0x18, 3, 1); }
void init()
{
    set_trap_system_gate(0, (void *)_divide_error_warpper);
    set_trap_system_gate(1, (void *)_debug_warpper);
    set_interrupt_system_gate(2, (void *)_nmi_warpper);
    set_trap_gate(3, (void *)_int3_warpper);
    set_trap_gate(4, (void *)_overflow_warpper);
    set_trap_gate(5, (void *)_bounds_warpper);
    set_trap_system_gate(6, (void *)_undefined_opcode_warpper);
    set_trap_system_gate(7, (void *)_dev_not_available_warpper);
    set_trap_system_gate(8, (void *)_double_fault_warpper);
    set_trap_system_gate(9, (void *)_coprocessor_segment_overrun_warpper);
    set_trap_system_gate(10, (void *)_invalid_TSS_warpper);
    set_trap_system_gate(11, (void *)_segment_not_present_warpper);
    set_trap_system_gate(12, (void *)_stack_segment_fault_warpper);
    set_trap_system_gate(13, (void *)_general_protection_warpper);
    set_trap_system_gate(14, (void *)_page_fault_warpper);
    // none 15
    set_trap_system_gate(16, (void *)_x87_FPU_error_warpper);
    set_trap_system_gate(17, (void *)_alignment_check_warpper);
    set_trap_system_gate(18, (void *)_machine_check_warpper);
    set_trap_system_gate(19, (void *)_SIMD_exception_warpper);
    set_trap_system_gate(20, (void *)_virtualization_exception_warpper);
}
} // namespace exception
