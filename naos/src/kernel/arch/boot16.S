.code32
.section .unpaged.text


// 64 temporary gdt
_gdt_start:
    .quad 0x0000000000000000 // null
    kernel_code:
    .quad 0x0020980000000000 //kernel code segment
    kernel_data:
    .quad 0x0000920000000000 // kernel data segment
_gdt_end:
.align 32
_gdt_ptr:
    .short (_gdt_end - _gdt_start - 1)
    .int _gdt_start
    .int 0

.globl _is_support_x64
_is_support_x64:
    push %ebx
    mov $0x80000000, %eax
    cpuid
    cmp $0x80000001, %eax
    jb no_long_mode
    mov $0x80000001, %eax
    cpuid
    test $0x20000000, %edx
    jz no_long_mode
    movl $1, %eax
    pop %ebx
    ret
no_long_mode:
    movl $0, %eax
    ret
.globl go_to_amd64
go_to_amd64:
    CLI
    movb $0xFF, %al
    outb %al, $0xA1
    outb %al, $0x21
    nop
    nop
// clean PG
    movl %cr0, %eax
    andl $0x7FFFFFFF, %eax
    movl %eax, %cr0

// set page
    movl $0x90000, %edi
    xor %eax, %eax
    movl $4096, %ecx
    // rep stosd
    rep stosl
    movl $0x90000, %edi
    // 2MB * 512 page
    // PML4T[0] -> 0x91000
    movl $0x91003, (%edi)
    addl $0x1000, %edi
    // PDPT[0] -> 0x92000
    movl $0x92003, (%edi)
    addl $0x1000, %edi

    movl $20, %ecx
    movl $0x83, %eax
loop_start:
    movl %eax, (%edi)
    addl $0x8, %edi
    addl $0x200000, %eax
    loop loop_start


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
    movl $(kernel_data - _gdt_start), %eax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss
    
    ljmp  $(kernel_code - _gdt_start), $jmp_dst

.code64
.section .unpaged.text
jmp_dst:
    andq $0xFFFFFFFFFFFFFFF0, %rsp
    // rsi = multiboot2_main
    movq %rsi, %r9
    subq $0x10, %rsp
    movq %rsp, %rsi
    movq %rsp, %rdx
    addq $0x8, %rdx
    callq *%r9
    // return args
    // (%rsp) is kstart function address
    movq %rax, %rdi
    movq (%rsp), %r9
    movq 8(%rsp), %rax
    movq %rax, %rsp
    jmp *%r9

    nop
    nop