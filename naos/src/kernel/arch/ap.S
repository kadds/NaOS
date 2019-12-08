.section .unpaged.text
.code16
.align 8
.globl _ap_code_start
_ap_code_start:
ap_boot: 
    cli
    wbinvd
    mov %cs, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    mov %ax, %fs
    mov %ax, %gs
    lgdtl _gdt_ap_ptr - _ap_code_start
    lidtl _idt_ptr - _ap_code_start
    smsw %ax
    bts $0, %ax
    lmsw %ax
    ljmpl *(jump_code32 - _ap_code_start)
    pause
.balign 4
jump_code32:
    .long	ap_code32 - _ap_code_start + base_ap_phy_addr
	.word	0x8,0	
    // 64 temporary gdt
.balign 4
_gdt_ap_start:
    .quad 0x0 // null
    kernel_code:
    .quad 0x00CF9A000000FFFF //kernel code segment
    kernel_data:
    .quad 0x00CF92000000FFFF // kernel data segment
_gdt_ap_end:

.balign 4
_gdt_ap_ptr:
    .short (_gdt_ap_end - _gdt_ap_start - 1)
    .int _gdt_ap_start
.balign 4
_idt_ptr:
    .short 0
    .int 0
.balign 4
.code32
ap_code32:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    mov %ax, %fs
    mov %ax, %gs
    incl (_ap_count)
spin_lock:
    xorl   %ecx, %ecx
    incl   %ecx
spin_lock_retry:
    xorl   %eax, %eax
    lock; cmpxchg %cx, (_ap_standby)
    jnz    spin_lock_retry
    movl (_ap_stack), %esp
    movl $0, (_ap_stack)
    pushl $0
    pushl (_target)
    xchg %bx, %bx
    movl $go_to_amd64_ap, %eax
    calll *%eax

hlt_code:
    pause
    jmp hlt_code
_ap_code_end:
.globl _ap_code_end

.balign 4
.globl _ap_standby
_ap_standby:
    .int 1
.globl _ap_stack   
_ap_stack:
    .int 0
.globl _ap_count
_ap_count:
    .int 0
_target:
    .quad _ap_64

.balign 8
.globl go_to_amd64_ap
go_to_amd64_ap:
    CLI
// clean PG
    movl %cr0, %eax
    andl $0x7FFFFFFF, %eax
    movl %eax, %cr0

// save data
    movl 4(%esp), %edi
    movl 8(%esp), %esi

// enable PAE
    movl %cr4, %eax
    orl $0x20, %eax
    movl %eax, %cr4
// set PML4
    movl $0x90000, %eax
    movl %eax, %cr3

// open LME. enter ia32e
    movl $0xC0000080, %ecx
    rdmsr
    orl $0x100, %eax
    wrmsr


// open PE & PG
    movl %cr0, %eax
    orl $0x80000001, %eax
    movl %eax, %cr0
// set long jmp data
    xchg %esi, %edi
 // load temporary gdt
    lgdt _gdt_ptr
    movl $0x10, %eax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss
    
    ljmp  $0x8, $jmp_dst
jmp_dst:
    jmp *%esi

// 64 temporary gdt
_gdt_start:
    .quad 0x0000000000000000 // null
    .quad 0x0020980000000000 //kernel code segment
    .quad 0x0000920000000000 // kernel data segment
_gdt_end:
.align 32
_gdt_ptr:
    .short (_gdt_end - _gdt_start - 1)
    .int _gdt_start
    .int 0

.code64
_ap_64:
    pushq $0
    callq _init_unpaged
    movabsq $base_virtual_addr, %rbx
    xchg %bx, %bx
    addq %rbx, %rsp
    movq $0, %rdi
    callq *%rax

end_ap:
    pause
    jmp end_ap
