#include "kernel/arch/exception.hpp"
#include "kernel/arch/idt.hpp"
#include "kernel/trace.hpp"
namespace exception
{
ExportC u64 _get_cr2();

ExportC void _divide_error_wrapper();
ExportC void _debug_wrapper();
ExportC void _nmi_wrapper();
ExportC void _int3_wrapper();

ExportC void _overflow_wrapper();
ExportC void _bounds_wrapper();
ExportC void _undefined_opcode_wrapper();
ExportC void _dev_not_available_wrapper();
ExportC void _double_fault_wrapper();
ExportC void _coprocessor_segment_overrun_wrapper();
ExportC void _invalid_TSS_wrapper();
ExportC void _segment_not_present_wrapper();
ExportC void _stack_segment_fault_wrapper();
ExportC void _general_protection_wrapper();
ExportC void _page_fault_wrapper();
ExportC void _x87_FPU_error_wrapper();
ExportC void _alignment_check_wrapper();
ExportC void _machine_check_wrapper();
ExportC void _SIMD_exception_wrapper();
ExportC void _virtualization_exception_wrapper();
void print_dst(regs_t *regs) { trace::debug("exception at: ", (void *)regs->rip, ", error code: ", regs->error_code); }
ExportC void entry_divide_error(regs_t *regs)
{
    trace::debug("divide error. ");
    print_dst(regs);
    while (1)
        ;
}
ExportC void entry_debug(regs_t *regs)
{
    trace::debug("debug trap. ");
    print_dst(regs);
    while (1)
        ;
}
ExportC void entry_nmi(regs_t *regs)
{
    trace::debug("nmi error! ");
    print_dst(regs);
    while (1)
        ;
}
ExportC void entry_int3(regs_t *regs)
{
    trace::debug("int3 trap. ");
    print_dst(regs);
    while (1)
        ;
}
ExportC void entry_overflow(regs_t *regs)
{
    trace::debug("overflow trap. ");
    print_dst(regs);
    while (1)
        ;
}
ExportC void entry_bounds(regs_t *regs)
{
    trace::debug("out of bounds error. ");
    print_dst(regs);
    while (1)
        ;
}
ExportC void entry_undefined_opcode(regs_t *regs)
{
    trace::debug("undefined opcode error. ");
    print_dst(regs);
    while (1)
        ;
}
ExportC void entry_dev_not_available(regs_t *regs)
{
    trace::debug("dev not available error. ");
    print_dst(regs);
    while (1)
        ;
}
ExportC void entry_double_fault(regs_t *regs)
{
    trace::debug("double abort. ");
    print_dst(regs);
    while (1)
        ;
}
ExportC void entry_coprocessor_segment_overrun(regs_t *regs)
{
    trace::debug("coprocessor segment overrun error. ");
    print_dst(regs);
    while (1)
        ;
}
ExportC void entry_invalid_TSS(regs_t *regs)
{
    trace::debug("invalid tss error. ");
    print_dst(regs);
    while (1)
        ;
}
ExportC void entry_segment_not_present(regs_t *regs)
{
    trace::debug("segment not present error. ");
    print_dst(regs);
    while (1)
        ;
}
ExportC void entry_stack_segment_fault(regs_t *regs)
{
    trace::debug("stack segment fault. ");
    print_dst(regs);
    while (1)
        ;
}
ExportC void entry_general_protection(regs_t *regs)
{
    trace::debug("general protection error. ");
    print_dst(regs);
    while (1)
        ;
}
ExportC void entry_page_fault(regs_t *regs)
{
    trace::debug("page fault. ");
    u64 cr2 = _get_cr2();
    trace::debug("page at: 0x%x%x, ", cr2 >> 32, cr2);
    print_dst(regs);
    while (1)
        ;
}
ExportC void entry_x87_FPU_error(regs_t *regs)
{
    trace::debug("X87 fpu error. ");
    print_dst(regs);
    while (1)
        ;
}
ExportC void entry_alignment_check(regs_t *regs)
{
    trace::debug("alignment error. ");
    print_dst(regs);
    while (1)
        ;
}
ExportC void entry_machine_check(regs_t *regs)
{
    trace::debug("machine error. ");
    print_dst(regs);
    while (1)
        ;
}
ExportC void entry_SIMD_exception(regs_t *regs)
{
    trace::debug("SIMD error. ");
    print_dst(regs);
    while (1)
        ;
}
ExportC void entry_virtualization_exception(regs_t *regs)
{
    trace::debug("virtualization error. ");
    print_dst(regs);
    while (1)
        ;
}

void init()
{
    using namespace idt;
    set_trap_system_gate(0, (void *)_divide_error_wrapper);
    set_trap_system_gate(1, (void *)_debug_wrapper);
    set_interrupt_system_gate(2, (void *)_nmi_wrapper);
    set_trap_gate(3, (void *)_int3_wrapper);
    set_trap_gate(4, (void *)_overflow_wrapper);
    set_trap_gate(5, (void *)_bounds_wrapper);
    set_trap_system_gate(6, (void *)_undefined_opcode_wrapper);
    set_trap_system_gate(7, (void *)_dev_not_available_wrapper);
    set_trap_system_gate(8, (void *)_double_fault_wrapper);
    set_trap_system_gate(9, (void *)_coprocessor_segment_overrun_wrapper);
    set_trap_system_gate(10, (void *)_invalid_TSS_wrapper);
    set_trap_system_gate(11, (void *)_segment_not_present_wrapper);
    set_trap_system_gate(12, (void *)_stack_segment_fault_wrapper);
    set_trap_system_gate(13, (void *)_general_protection_wrapper);
    set_trap_system_gate(14, (void *)_page_fault_wrapper);
    // none 15
    set_trap_system_gate(16, (void *)_x87_FPU_error_wrapper);
    set_trap_system_gate(17, (void *)_alignment_check_wrapper);
    set_trap_system_gate(18, (void *)_machine_check_wrapper);
    set_trap_system_gate(19, (void *)_SIMD_exception_wrapper);
    set_trap_system_gate(20, (void *)_virtualization_exception_wrapper);
}
} // namespace exception
