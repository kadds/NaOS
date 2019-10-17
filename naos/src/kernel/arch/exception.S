.section .text
call_code:
    pushq %rax # func addr
	pushf
	pop %rax
	cli
	testb $3, 0x28(%rsp)
	je not_need_swaps
    swapgs
 not_need_swaps:
	test $0x200, %eax
	je not_need_enable
	sti
 not_need_enable:
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
	testb $3, 0xa8(%rsp)
	je not_need_schedule
    movabs $schedule, %rdx
	callq *%rdx
 not_need_schedule:
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
	pushf
	pop %rax
	cli
	testb $3, 0x28(%rsp)
	je not_need_swaps2
    swapgs
 not_need_swaps2:
	test $0x200, %eax
	je not_need_enable2
	sti
not_need_enable2:
 	addq $0x8, %rsp # func addr
	popq %rax # rax
	addq $0x10, %rsp # error code & vector
	iretq

#define exception_entry_error_code(n, name)  \
.globl _##name##_wrapper ;\
_##name##_wrapper: \
	pushq $n ;\
    pushq %rax ;\
    leaq __do_exception(%rip), %rax ;\
    jmp call_code ;

#define exception_entry(n, name)  \
.globl _##name##_wrapper ;\
_##name##_wrapper: \
	pushq $0 ;\
	pushq $n ;\
    pushq %rax ;\
    leaq __do_exception(%rip), %rax ;\
    jmp call_code ;

.globl _nmi_wrapper
_nmi_wrapper:
	pushq $0
	pushq $2
    pushq %rax 
    leaq entry_nmi(%rip), %rax
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
	movq %cs, %rax
    add $0x8, %rax
    movq %rax, %ds
    movq %rax, %es

    movq %rsp, %rdi
	movq 0x80(%rsp), %rdx
    callq *%rdx
	movabs $schedule, %rdx
	callq *%rdx

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

exception_entry(0, divide_error)

exception_entry(1, debug)

exception_entry(3, int3)

exception_entry(4, overflow)

exception_entry(5, bounds)

exception_entry(6, undefined_opcode)

exception_entry(7, dev_not_available)

exception_entry_error_code(8, double_fault)

exception_entry(9, coprocessor_segment_overrun)

exception_entry_error_code(10, invalid_TSS)

exception_entry_error_code(11, segment_not_present)

exception_entry_error_code(12, stack_segment_fault)

exception_entry_error_code(13, general_protection)

exception_entry_error_code(14, page_fault)

exception_entry(16, x87_FPU_error)

exception_entry_error_code(17, alignment_check)

exception_entry(18, machine_check)

exception_entry(19, SIMD_exception)

exception_entry(20, virtualization_exception)

