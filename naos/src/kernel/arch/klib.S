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
.globl _flush_tlb
_flush_tlb:
    movq %cr3, %rax
    movq %rax, %cr3
    ret

# ======================================================
.section .unpaged.text

.globl _unpaged_load_gdt
_unpaged_load_gdt:
    lgdt (%rdi)
    ret
    
.globl _unpaged_load_idt
_unpaged_load_idt:
    lidt (%rdi)
    ret

.globl _unpaged_load_page
_unpaged_load_page:
    movq %rdi, %cr3
    ret

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