OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data PT_LOAD FLAGS(6);
    stack PT_GNU_STACK FLAGS(6);
}

SECTIONS
{
    . = 0x400000 + SIZEOF_HEADERS;

    .text :
    {
        KEEP(*(.text._start))
        *(.text .text.*)
    } :text

    . = ALIGN(0x1000);
    .rodata :
    {
        *(.rodata .rodata.*)
    } :rodata

    . = ALIGN(0x1000);
    .data :
    {
        *(.data .data.*)
    } :data

    .bss :
    {
        *(.bss .bss.*)
        *(COMMON)
    } :data

    /* Keep a real writable PT_LOAD even for a binary without mutable statics.
     * The NaOS ELF loader treats a zero-sized load segment as invalid. */
    .naos_data_marker :
    {
        BYTE(0)
    } :data

    /DISCARD/ :
    {
        *(.eh_frame .eh_frame.*)
        *(.gcc_except_table .gcc_except_table.*)
        *(.comment)
        *(.note .note.*)
    }
}
