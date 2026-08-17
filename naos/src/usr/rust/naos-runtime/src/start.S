.text
.section .text._start, "ax"
.globl _start
.type _start, @function
_start:
    xorq %rbp, %rbp
    movq %rsp, %rdi
    callq __naos_runtime_start
    ud2
.size _start, .-_start
