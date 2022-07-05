.section .text


.globl _start
_start:
    movq (%rsp), %rdi
    movq 0x8(%rsp), %rsi
    movq 0xF(%rsp), %rdx
    callq main
    