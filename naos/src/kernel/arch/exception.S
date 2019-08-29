.section .text
call_code:
    pushq %rax # func addr
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
	movq %cs, %rax
    add $0x8, %rax
    movq %rax, %ds
    movq %rax, %es

    movq %rsp, %rdi
	movq 0x80(%rsp), %rdx
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
	addq $0x8, %rsp # func addr
	popq %rax # rax
	addq $0x10, %rsp # error code & vector
	iretq

#define exception_entry(n, name)  \
.globl _##name##_wrapper ;\
_##name##_wrapper: \
	pushq $n ;\
    pushq %rax ;\
    leaq entry_##name(%rip), %rax ;\
    jmp call_code ;

#define exception_entry_ec(n, name)  \
.globl _##name##_wrapper ;\
_##name##_wrapper: \
	pushq $0 ;\
	pushq $n ;\
    pushq %rax ;\
    leaq entry_##name(%rip), %rax ;\
    jmp call_code ;

exception_entry_ec(0, divide_error)

exception_entry_ec(1, debug)

exception_entry_ec(2, nmi)

exception_entry_ec(3, int3)

exception_entry_ec(4, overflow)

exception_entry_ec(5, bounds)

exception_entry_ec(6, undefined_opcode)

exception_entry_ec(7, dev_not_available)

exception_entry(8, double_fault)

exception_entry_ec(9, coprocessor_segment_overrun)

exception_entry(10, invalid_TSS)

exception_entry(11, segment_not_present)

exception_entry(12, stack_segment_fault)

exception_entry(13, general_protection)

exception_entry(14, page_fault)

exception_entry_ec(16, x87_FPU_error)

exception_entry(17, alignment_check)

exception_entry_ec(18, machine_check)

exception_entry_ec(19, SIMD_exception)

exception_entry_ec(20, virtualization_exception)

