OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(main)

SECTIONS
{
    . = 0x400000;
    .text :
    {
        *(.start)
        *(.text*)
    }
    . = ALIGN(4);
    .data : 
    {
        *(.data .rodata*)
    }
    . = ALIGN(4);
    .bss :
    {
        *(.bss COMMON)
    }
}