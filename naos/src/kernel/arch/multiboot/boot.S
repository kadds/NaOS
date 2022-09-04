#define ASM_FILE        1
#include "kernel/arch/multiboot/multiboot2.hpp"

#define MULTIBOOT_HEADER_CHECKSUM (-(MULTIBOOT2_HEADER_MAGIC + MULTIBOOT_ARCHITECTURE_I386 + (_header_multiboot_end-_header_multiboot_start)))

.section .start_multiboot2
.align  16
_header_multiboot_start:
// magic
.long MULTIBOOT2_HEADER_MAGIC
// ISA
.long MULTIBOOT_ARCHITECTURE_I386
// length
.long _header_multiboot_end-_header_multiboot_start
// checksum
.long MULTIBOOT_HEADER_CHECKSUM

_address_start:
.short MULTIBOOT_HEADER_TAG_ADDRESS
.short 0
.long _address_end-_address_start
// header_addr
.long _header_multiboot_start
//load_addr
.long base_phy_addr
//load_end_addr
.long 0
//bss_end_addr
.long _bss_end
_address_end:

.align 8
_entry_start:
.short MULTIBOOT_HEADER_TAG_ENTRY_ADDRESS
.short 0
.long _entry_end-_entry_start
//entry_addr
.long _multiboot_start
_entry_end:

.align 8
// video info
_frame_buffer_start:
.short MULTIBOOT_HEADER_TAG_FRAMEBUFFER
.short 0
.long _frame_buffer_end-_frame_buffer_start
.long 800
.long 600
.long 32
_frame_buffer_end:

.align 8
_module_start:
.short MULTIBOOT_HEADER_TAG_MODULE_ALIGN
.short 0
.long _module_end-_module_start
_module_end:

.align 8
.short MULTIBOOT_HEADER_TAG_END
.short 0
.long 8
_header_multiboot_end:

magic:
    .long MULTIBOOT2_BOOTLOADER_MAGIC

.code32
.section .unpaged.text
.align  16
_multiboot_start:
    cmpl (magic), %eax
    jnz jmp_label    
    movl $0x90000, %esp
    pushl   $0
    popf
    movl $_multiboot_main, %edi
    movl %ebx, %esi
    push %esi
    push %edi
    movl $go_to_amd64, %eax
    call *%eax
jmp_label:
    pause
    jmp  jmp_label
    