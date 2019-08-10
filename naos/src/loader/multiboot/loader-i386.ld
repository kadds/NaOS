OUTPUT_FORMAT("elf32-i386")
OUTPUT_ARCH(i386)
ENTRY(_start)
SECTIONS
{
    . = 0x1000000;
    .text :
    {
        *(.boot)
        *(.text*)
    }
    . = ALIGN(4);
    .data : 
    {
        *(.data .rodata*)
    }
    . = ALIGN(4);
    .init_array :
    {
        __init_array_start = .;
        KEEP (*(.init_array*))
        __init_array_end = .;
    }

    .bss :
    {
        *(.bss COMMON)
        _bss_end_addr = .;
    }
}