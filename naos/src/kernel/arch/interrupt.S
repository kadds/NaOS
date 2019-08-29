.section .text
call_code_interrupt:
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

return_code_interrupt:
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
#define interrupt_entry(n)  \
.globl _interrupt_wrapper_##n ;\
_interrupt_wrapper_##n: \
	push $0 ;\
	pushq $n ;\
    pushq %rax ;\
    leaq do_irq(%rip), %rax ;\
    jmp call_code_interrupt ;


interrupt_entry(32)
interrupt_entry(33)
interrupt_entry(34)
interrupt_entry(35)
interrupt_entry(36)
interrupt_entry(37)
interrupt_entry(38)
interrupt_entry(39)
interrupt_entry(40)
interrupt_entry(41)
interrupt_entry(42)
interrupt_entry(43)
interrupt_entry(44)
interrupt_entry(45)
interrupt_entry(46)
interrupt_entry(47)
interrupt_entry(48)
interrupt_entry(49)
interrupt_entry(50)
interrupt_entry(51)
interrupt_entry(52)
interrupt_entry(53)
interrupt_entry(54)
interrupt_entry(55)
interrupt_entry(56)
interrupt_entry(57)
interrupt_entry(58)
interrupt_entry(59)
interrupt_entry(60)
interrupt_entry(61)
interrupt_entry(62)
interrupt_entry(63)
interrupt_entry(64)
interrupt_entry(65)
interrupt_entry(66)
interrupt_entry(67)
interrupt_entry(68)
interrupt_entry(69)
interrupt_entry(70)
interrupt_entry(71)
interrupt_entry(72)
interrupt_entry(73)
interrupt_entry(74)
interrupt_entry(75)
interrupt_entry(76)
interrupt_entry(77)
interrupt_entry(78)
interrupt_entry(79)
interrupt_entry(80)
interrupt_entry(81)
interrupt_entry(82)
interrupt_entry(83)
interrupt_entry(84)
interrupt_entry(85)
interrupt_entry(86)
interrupt_entry(87)
interrupt_entry(88)
interrupt_entry(89)
interrupt_entry(90)
interrupt_entry(91)
interrupt_entry(92)
interrupt_entry(93)
interrupt_entry(94)
interrupt_entry(95)
interrupt_entry(96)
interrupt_entry(97)
interrupt_entry(98)
interrupt_entry(99)
interrupt_entry(100)
