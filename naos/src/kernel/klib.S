.section .text


.globl _load_gdt
_load_gdt:
    lgdt (%rdi)
    ret
    
.globl _load_idt
_load_idt:
    lidt (%rdi)
    ret

.globl _load_tss_descriptor
_load_tss_descriptor:
    mov %rdi, %rax
    ltr %ax
    ret

.globl _load_page
_load_page:
    movq %rdi, %cr3
    ret

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
.globl _cli
_cli:
    cli
    ret
.globl _sti
_sti:
    sti
    ret
.globl _get_cr2
_get_cr2:
    movq %cr2, %rax
    ret

