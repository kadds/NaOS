OUTPUT_FORMAT("elf32-i386")
OUTPUT_ARCH(i386)
ENTRY(_start)
SECTIONS
{
    . = 0x1000000;
    .text :
    {
        *(.header_multiboot)
        *(.text*)
        *(.boot)
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
        *(.bss)
        _bss_end_addr = .;
    }
}