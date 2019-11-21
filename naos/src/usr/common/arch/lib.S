.section .text


.globl _lib_sys_call
_lib_sys_call:
    pushq %r10
    pushq %r11
    pushq %r12
    movq %rcx, %r12
    movq %rsp, %r10
    syscall
    popq %r12
    popq %r11
    popq %r10
    retq




