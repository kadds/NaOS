.section .text
.globl call_code_interrupt
call_code_interrupt:
	movabs $__do_irq, %rax
	pushq %rax
	testb $3, 0x28(%rsp)
	je not_need_swaps
    swapgs
 not_need_swaps:
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

return_code_interrupt:
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
	testb $3, 0x28(%rsp)
	je not_need_swaps2
    swapgs
 not_need_swaps2:
 	addq $0x8, %rsp # func addr
	popq %rax # rax
	addq $0x10, %rsp # error code & vector
	iretq
.align 8
.globl interrupt_code_start
interrupt_code_start:
	push $0 # error code
	pushq $200 # vector
    pushq %rax # rax
	movabs $call_code_interrupt, %rax
    jmp *%rax
.globl interrupt_code_end
interrupt_code_end:
	nop
	nop
	pause