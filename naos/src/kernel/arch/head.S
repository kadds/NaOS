
.section .start_kernel
_text_start_addr:
    .quad 0xEEFFFFEE00FFFF00 # magic number
    .quad _start - _text_start_addr
    .quad _bss_len
    .quad 0xAA5555AAAA0000AA # magic number
.globl _start
_start: 
    movl $0x8000, %esp
    push %rdi
    call _init_unpaged
    pop %rdi
    # rax is _kstart addr
    # jmp to _kstart
    sub $16, %rsp
    movq %cs, %rdx
    movq %rdx, 8(%rsp)
    movabs $base_virtual_addr, %rdx
    movq %rax, jmp_kernel_ptr_x(%rip)
    movq jmp_kernel_ptr_x(%rip), %rax
    movq %rax, (%rsp)
    addq %rdx, %rsp
    addq %rdx, %rdi
    lretq
jmp_kernel_ptr_x:
    .quad loop
loop:
    pause
    jmp loop
