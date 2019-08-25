OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)
base_virtual_addr = 0xffff800000000000;
base_phy_addr = 0x100000;
kernel_offset = base_virtual_addr;
SECTIONS
{
    . = base_phy_addr;
    .unpaged.text : 
    {
        *(.start_kernel)
        *(.unpaged.text)
    }
    . = ALIGN(4);
    .unpaged.data : 
    {
        *(.unpaged.data)
    }
    . = ALIGN(4);
    .unpaged.bss : 
    {
        _bss_unpaged_start_addr = .;
        *(.unpaged.bss)
        _bss_unpaged_end_addr = .;
    }
    . += kernel_offset;
    .text : AT(ADDR(.text) - kernel_offset)
    {
        *(.text*)
    }
    . = ALIGN(4);
    .data : AT(ADDR(.data) - kernel_offset)
    {
        *(.data .rodata*)
    }
    . = ALIGN(4);
    .init_array : AT(ADDR(.init_array) - kernel_offset)
    {
        __init_array_start = .;
        KEEP (*(.init_array*))
        __init_array_end = .;
    }
    _end_of_init_array = .;
    . = ALIGN(4);
    .bss : AT(ADDR(.bss) - kernel_offset)
    {
        _bss_start_addr = . - kernel_offset;
        *(.bss COMMON)
        . = ALIGN(4);
        _bss_end_addr = . - kernel_offset;
    }
    _bss_len = _bss_end_addr - (_end_of_init_array - kernel_offset);
    _end = .;
}