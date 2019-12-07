OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)
base_ap_phy_addr = 0x20000;
base_virtual_addr = 0xffff800000000000;
base_phy_addr = 0x100000;
kernel_offset = base_virtual_addr;
SECTIONS
{
    . = base_phy_addr;
    _file_start = .;
    .unpaged.text : 
    {
        KEEP(*(.start_kernel))
        KEEP(*(.start_multiboot2))
        *(.unpaged.text)
    }
    . = ALIGN(8);
    .unpaged.data : 
    {
        *(.unpaged.data)
    }

    . += kernel_offset;
    _start_of_kernel_data = . - kernel_offset;
    .text : AT(ADDR(.text) - kernel_offset)
    {
        *(.text*)
    }

    . = ALIGN(8);
    .data : AT(ADDR(.data) - kernel_offset)
    {
        *(.data .rodata* COMMON)
    }
    . = ALIGN(8);
    .init_array : AT(ADDR(.init_array) - kernel_offset)
    {
        __init_array_start = .;
        KEEP (*(.init_array*))
        __init_array_end = .;
    }
    _end_of_init_array = .;
    . = ALIGN(8);
    _end_kernel_data = .  - kernel_offset;
    
    _file_end = . - kernel_offset;
    _bss_start = . - kernel_offset;
    . = ALIGN(8);
    .bss : AT(ADDR(.bss) - kernel_offset)
    {
        _bss_start_addr = . - kernel_offset;
        *(.bss)
        . = ALIGN(8);
        _bss_end_addr = . - kernel_offset;
    }
    . = ALIGN(8);
    _bss_end = . - kernel_offset;
    . = ALIGN(4);
    . = _bss_start;
    
    .unpaged.bss :
    {
        _bss_unpaged_start = .;
        *(.unpaged.bss)
        . = ALIGN(8);
        _bss_unpaged_end = .;
    }
    
    _bss_len = _bss_end - _bss_start;
}