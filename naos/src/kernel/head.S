
.section .start_kernel
_text_start_addr:
    .quad _start - _text_start_addr
    .quad _bss_end_addr - _text_start_addr
.globl _start
_start:
    movl 0x10(%edi), %esp
    call _start_kernel
loop:
    jmp loop
