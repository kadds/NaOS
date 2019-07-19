.section .text
call_code:
    pushq %rax
	movq %es, %rax
	pushq %rax
	movq %ds, %rax
	pushq %rax

	pushq %rbp
    pushq %rdi
	pushq %rsi
	pushq %rdx
	pushq %rcx
	pushq %rbx
	pushq %r8
	pushq %r9
	pushq %r10
	pushq %r11
	pushq %r12
	pushq %r13
	pushq %r14
	pushq %r15	
    movq $0x10, %rax
    movq %rax, %ds
    movq %rax, %es

    movq %rsp, %rdi


    callq *%rdx

return_code:
    popq %r15	
	popq %r14	
	popq %r13	
	popq %r12	
	popq %r11	
	popq %r10	
	popq %r9	
	popq %r8	
	popq %rbx	
	popq %rcx
	popq %rdx
	popq %rsi
    popq %rdi
	popq %rbp
	popq %rax
	movq %rax, %ds	
	popq %rax
	movq %rax, %es
	popq %rax
	popq %rax
	iretq

.globl _divide_error_warpper
_divide_error_warpper:
    pushq %rdx
    leaq entry_divide_error(%rip), %rdx
    jmp call_code

.globl _debug_warpper
_debug_warpper:
    pushq %rdx
    leaq entry_debug(%rip), %rdx
    jmp call_code

.globl _nmi_warpper
_nmi_warpper:
    pushq %rdx
    leaq entry_nmi(%rip), %rdx
    jmp call_code

.globl _int3_warpper
_int3_warpper:
	pushq %rdx
    leaq entry_int3(%rip), %rdx
    jmp call_code

.globl _overflow_warpper
_overflow_warpper:
	pushq %rdx
    leaq entry_overflow(%rip), %rdx
    jmp call_code

.globl _bounds_warpper
_bounds_warpper:
	pushq %rdx
    leaq entry_bounds(%rip), %rdx
    jmp call_code

.globl _undefined_opcode_warpper
_undefined_opcode_warpper:
	pushq %rdx
    leaq entry_undefined_opcode(%rip), %rdx
    jmp call_code

.globl _dev_not_available_warpper
_dev_not_available_warpper:
	pushq %rdx
    leaq entry_dev_not_available(%rip), %rdx
    jmp call_code

.globl _double_fault_warpper
_double_fault_warpper:
	pushq %rdx
    leaq entry_double_fault(%rip), %rdx
    jmp call_code

.globl _coprocessor_segment_overrun_warpper
_coprocessor_segment_overrun_warpper:
	pushq %rdx
    leaq entry_coprocessor_segment_overrun(%rip), %rdx
    jmp call_code

.globl _invalid_TSS_warpper
_invalid_TSS_warpper:
	 pushq %rdx
    leaq entry_invalid_TSS(%rip), %rdx
    jmp call_code

.globl _segment_not_present_warpper
_segment_not_present_warpper:
	pushq %rdx
    leaq entry_segment_not_present(%rip), %rdx
    jmp call_code

.globl _stack_segment_fault_warpper
_stack_segment_fault_warpper:
	pushq %rdx
    leaq entry_stack_segment_fault(%rip), %rdx
    jmp call_code

.globl _general_protection_warpper
_general_protection_warpper:
	pushq %rdx
    leaq entry_general_protection(%rip), %rdx
    jmp call_code

.globl _page_fault_warpper
_page_fault_warpper:
	pushq %rdx
    leaq entry_page_fault(%rip), %rdx
    jmp call_code

.globl _x87_FPU_error_warpper
_x87_FPU_error_warpper:
	pushq %rdx
    leaq entry_x87_FPU_error(%rip), %rdx
    jmp call_code

.globl _alignment_check_warpper
_alignment_check_warpper:
	pushq %rdx
    leaq entry_alignment_check(%rip), %rdx
    jmp call_code

.globl _machine_check_warpper
_machine_check_warpper:
	pushq %rdx
    leaq entry_machine_check(%rip), %rdx
    jmp call_code

.globl _SIMD_exception_warpper
_SIMD_exception_warpper:
	pushq %rdx
    leaq entry_SIMD_exception(%rip), %rdx
    jmp call_code
    
	
.globl _virtualization_exception_warpper
_virtualization_exception_warpper:
	pushq %rdx
    leaq entry_virtualization_exception(%rip), %rdx
    jmp call_code