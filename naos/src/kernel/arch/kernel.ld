OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)
base_ap_phy_addr = 0x70000;
base_tmp_page_entries = 0x1000;
base_virtual_addr = 0xffff800000000000;
base_phy_addr = 0x100000;

SECTIONS
{
    . = base_phy_addr;

    .unpaged.final.text : 
    {
        KEEP(*(.start_multiboot2))
        KEEP(*(.start_kernel))
        KEEP(*(.unpaged.text))
    }
    . = ALIGN(8);
    .unpaged.final.data :
    {
        *(.unpaged.data.*)
    } 
    . = ALIGN(8); 
    .unpaged.final.bss : 
    {
        _bss_unpaged_start = .;
        *(.unpaged.bss)
        . = ALIGN(8);
        _bss_unpaged_end = .;
    }
    _unpaged_end = .;
    _unpaged_size = _unpaged_end - base_phy_addr;

    . = base_virtual_addr;
    . = base_virtual_addr + _unpaged_end;

    . = ALIGN(32);
    .text : AT(ADDR(.text) - base_virtual_addr)
    {
        *(.text*)
    }

    . = ALIGN(8);
    .data : AT(ADDR(.data) - base_virtual_addr)
    {
        *(.data.*)
        *(.data .rodata* COMMON)
    }
    . = ALIGN(8);
    .init_array : AT(ADDR(.init_array) - base_virtual_addr)
    {
        __init_array_start = .;
        KEEP (*(.init_array*))
        __init_array_end = .;
    }
    _end_of_init_array = .;
    . = ALIGN(8);
    _end_kernel_data = .  - base_virtual_addr;
    
    _bss_start = . - base_virtual_addr;
    . = ALIGN(8);
    .bss : AT(ADDR(.bss) - base_virtual_addr)
    {
        _bss_start_addr = . - base_virtual_addr;
        *(.bss)
        . = ALIGN(8);
        _bss_end_addr = . - base_virtual_addr;
    }
    . = ALIGN(8);
    _bss_end = . - base_virtual_addr;

    _bss_len = _bss_end - _bss_start;
     
    /DISCARD/ : 
    {
        *(.gnu.hash)
        *(.hash)
        *(.comment)
        *(.note.gnu.property)
    }
    .dynsym : 
    {
        *(.dynsym)
    }
    .dynstr : 
    {
        *(.dynstr)
    }
    .interp : 
    {
        *(.interp)
    }
    .dynamic : 
    {
        *(.dynamic)
    }

}