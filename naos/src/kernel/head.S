
.section .start_kernel
_text_start_addr:
    .quad _start - _text_start_addr
    .quad _bss_len
.globl _start
_start: 
    movl 0x10(%edi), %esp
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
    retfq
jmp_kernel_ptr_x:
    .quad loop
loop:
    hlt
    jmp loop
