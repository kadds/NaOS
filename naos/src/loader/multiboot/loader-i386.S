// http://www.gnu.org/software/grub/manual/multiboot/multiboot.html
#define ASM_FILE 1
#include "loader/multiboot.hpp"

#define MULTIBOOT_HEADER_FLAGS           MULTIBOOT_PAGE_ALIGN | MULTIBOOT_VIDEO_MODE | MULTIBOOT_MEMORY_INFO | MULTIBOOT_AOUT_KLUDGE
#define MULTIBOOT_HEADER_CHECKSUM        -(MULTIBOOT_HEADER_MAGIC + MULTIBOOT_HEADER_FLAGS)
#define BASIC_ADDR 0x0
// 1MB
#define STACK_SIZE 0x7fff


.section .boot
_header_multiboot_start:
// magic
.long MULTIBOOT_HEADER_MAGIC
// flags
.long MULTIBOOT_HEADER_FLAGS
// checksum
.long MULTIBOOT_HEADER_CHECKSUM
// header_addr
.long _header_multiboot_start + BASIC_ADDR
//load_addr
.long _header_multiboot_start + BASIC_ADDR
//load_end_addr
.long 0
//bss_end_addr
.long _bss_end_addr
//entry_addr
.long _start

// video info
.long 1
.long 80
.long 25
.long 0


.globl _start

_start:
    movl $(STACK_SIZE + _bss_end_addr), %esp
    pushl   %ebx
    pushl   %eax
    call _main

    hlt
    jmp .


.align  4
.globl _stack_size
_stack_size:
.int STACK_SIZE

.align  4
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
.globl run_kernel
run_kernel:
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
jmp_dst:
    jmp *%esi

    nop
    nop

