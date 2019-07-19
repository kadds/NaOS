#include "kernel/exception.hpp"
#include "kernel/ScreenPrinter.hpp"
#include "kernel/idt.hpp"
namespace exception
{
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

ExportC void entry_divide_error()
{
    gPrinter->printf("divide error!\n");
    while (1)
        ;
}
ExportC void entry_debug()
{
    gPrinter->printf("debug trap/error!\n");
    while (1)
        ;
}
ExportC void entry_nmi()
{
    gPrinter->printf("nmi error!\n");
    while (1)
        ;
}
ExportC void entry_int3()
{
    gPrinter->printf("int3 trap!\n");
    while (1)
        ;
}
ExportC void entry_overflow()
{
    gPrinter->printf("overflow trap!\n");
    while (1)
        ;
}
ExportC void entry_bounds()
{
    gPrinter->printf("out of bounds error!\n");
    while (1)
        ;
}
ExportC void entry_undefined_opcode()
{
    gPrinter->printf("undefined opcode error!\n");
    while (1)
        ;
}
ExportC void entry_dev_not_available()
{
    gPrinter->printf("dev not available error!\n");
    while (1)
        ;
}
ExportC void entry_double_fault()
{
    gPrinter->printf("double abort!\n");
    while (1)
        ;
}
ExportC void entry_coprocessor_segment_overrun()
{
    gPrinter->printf("coprocessor segment overrun error!\n");
    while (1)
        ;
}
ExportC void entry_invalid_TSS()
{
    gPrinter->printf("invalid tss error!\n");
    while (1)
        ;
}
ExportC void entry_segment_not_present()
{
    gPrinter->printf("segment not present error!\n");
    while (1)
        ;
}
ExportC void entry_stack_segment_fault()
{
    gPrinter->printf("stack segment fault!\n");
    while (1)
        ;
}
ExportC void entry_general_protection()
{
    gPrinter->printf("general protection error!\n");
    while (1)
        ;
}
ExportC void entry_page_fault()
{
    gPrinter->printf("page fault!\n");
    while (1)
        ;
}
ExportC void entry_x87_FPU_error()
{
    gPrinter->printf("X87 fpu error!\n");
    while (1)
        ;
}
ExportC void entry_alignment_check()
{
    gPrinter->printf("alignment error!\n");
    while (1)
        ;
}
ExportC void entry_machine_check()
{
    gPrinter->printf("machine error!\n");
    while (1)
        ;
}
ExportC void entry_SIMD_exception()
{
    gPrinter->printf("SIMD error!\n");
    while (1)
        ;
}
ExportC void entry_virtualization_exception()
{
    gPrinter->printf("virtualization error!\n");
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
