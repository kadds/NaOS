
.section .text

.globl _reload_segment
_reload_segment:
    movq reload_seg_ptr_x(%rip), %rax
    sub $16, %rsp
    movq %rdi, 8(%rsp)
    movq %rax, (%rsp)
    retfq
reload_seg_ptr_x:
    .quad reload_seg_x
reload_seg_x:
    movq %rsi, %ds
    movq %rsi, %es
    movq %rsi, %gs
    movq %rsi, %ss
    ret

.globl _cpu_id
_cpu_id:
    pushq %rbx
    movl %edi, %eax
    movq %rcx, %r9
    movq %rdi, %rcx
    shr  $32, %rcx
    clc
    
    movq %rdx, %rdi
    cpuid
    mov %eax, (%rsi)
    mov %ebx, (%rdi)
    mov %ecx, (%r9)
    mov %edx, (%r8)
    popq %rbx
    ret
.globl _switch_task
_switch_task:
    pushq %rbp
    pushq %rax
    movq %rsp, 0x8(%rdi) # save rsp
    movq 0x8(%rsi), %rsp # new rsp
    leaq end_task(%rip), %rax 
    movq %rax, 0x0(%rdi) # prev rip
    pushq 0x0(%rsi) # push new rip

    # movq 0x18(%rsi), %rax
    # movq %rax, %fs
    # movq 0x20(%rsi), %rax
    # movq %rax, %gs

    jmp switch_task
end_task:
    popq %rax
    popq %rbp
    retq

.globl _kernel_thread
_kernel_thread:
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
    addq $0x28, %rsp

    movq %rdx, %rdi
    sti
    callq *%rbx
    movq %rax, %rdi
    callq task_do_exit
    ret

// args -> rdi, rsi, r12, r13, r8, r9
// r10: rsp
// r11: return address
.globl _sys_call
_sys_call:
    cld
    pushq %rax
    swapgs
    movq %gs:0x8, %rax
    movq %rax, %rsp
    movq -0x8(%r10), %rax
    pushq %rax

	movq %es,	%rax
	pushq %rax
	movq %ds,	%rax
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

	movq 0x80(%rsp), %rax
    cmp $128, %rax
    ja sys_call_setlabel
    jmp sys_call_label
sys_call_setlabel:
    movq $0, %rax
sys_call_label:
	leaq system_call_table(%rip), %rbx
	callq *(%rbx,%rax,8)

    movq %rax, 0x80(%rsp) # return value
    
    movabs $schedule, %rdx
	callq *%rdx

.globl _sys_ret
_sys_ret:
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
    swapgs
	popq %rax
    movq %r10, %rsp
    sysretq


// rdi: reg pointer
.globl _call_sys_ret
_call_sys_ret:
    movq %rdi, %rsp
    jmp _sys_ret

/// rdi param1
/// rsi param2
/// rdx param3
/// rcx param4
/// r8 func
/// r9 new_stack
.globl _switch_stack
_switch_stack:
    pushq %r15
    movq %rsp, %r15
    movq %r9, %rsp
    call *%r8
    movq %r15, %rsp 
    popq %r15
    retq
# ======================================================
.section .unpaged.text

.globl _unpaged_reload_segment
_unpaged_reload_segment:
    movq reload_unpaged_seg_ptr_x(%rip), %rax
    sub $16, %rsp
    movq %rdi, 8(%rsp)
    movq %rax, (%rsp)
    retfq
reload_unpaged_seg_ptr_x:
    .quad reload_unpaged_seg_x
reload_unpaged_seg_x:
    movq %rsi, %ds
    movq %rsi, %es
    movq %rsi, %gs
    movq %rsi, %ss
    ret

.globl _unpaged_reload_kernel_virtual_address
# rdi offset
_unpaged_reload_kernel_virtual_address:
    sub $16, %rsp
    movq %cs, %rsi
    movq %rsi, 8(%rsp)
    movq reload_kernel_virtual_address_ptr_x(%rip), %rax
    addq %rdi , %rax
    addq %rdi, reload_kernel_virtual_address_ptr_x(%rip)
    movq %rax, (%rsp)
    retfq
reload_kernel_virtual_address_ptr_x:
    .quad reload_kernel_virtual_address_x
reload_kernel_virtual_address_x:
    addq %rdi, %rsp
    ret
    ret