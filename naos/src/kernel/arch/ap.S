.section .unpaged.text
.code16
.balign 8
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
    ljmpl $0x8, $ap_code32
    pause
.balign 4
    // 64 temporary gdt
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
    lock incl (_ap_count)
spin_lock:
    movl $1, %ecx
spin_lock_retry:
    xorl   %eax, %eax
    mfence
    lock cmpxchg %ecx, (_ap_startup_spin_flag)
    mfence
    jnz    spin_lock

    // template esp
    movl $0x80000, %esp 
    pushl $0
    pushl (_target)
    xchg %bx, %bx
    movl $go_to_amd64_ap, %eax
    calll *%eax

hlt_code:
    pause
    jmp hlt_code
.globl _ap_code_end
_ap_code_end:

// data 

.balign 4
.globl _ap_startup_spin_flag
_ap_startup_spin_flag:
    .int 0
.globl _ap_count
_ap_count:
    .int 0
    
.balign 8
.globl _ap_stack   
_ap_stack:
    .quad 0
_target:
    .quad _ap_64

// amd64 mode code 

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
    movl $base_tmp_page_entries, %eax
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
.balign 8
_ap_64:
    andq $0xFFFFFFFFFFFFFFF0, %rsp
    xorq %rdi, %rdi
    movabsq $_init_unpaged, %rax
    call *%rax
    movq $_ap_stack, %rax
    movq (%rax), %rsp
    mfence
    movq $0, (%rax)
    movabsq $_kstart, %rax
    xorq %rdi, %rdi
    jmp *%rax

end_ap:
    pause
    jmp end_ap
