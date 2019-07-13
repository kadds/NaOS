OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)
SECTIONS
{
    . = 0x100000;
    .text :
    {
        *(.start_kernel)
        *(.text*)
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