.section .text


.globl _lib_sys_call
_lib_sys_call:
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    movq %rdx, %r12
    movq %rcx, %r13
    movq %rsp, %r10
    syscall
    popq %r13
    popq %r12
    pop %r11
    pop %r10
    retq




