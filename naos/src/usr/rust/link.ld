OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

PHDRS
{
    text PT_LOAD FILEHDR PHDRS FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data PT_LOAD FLAGS(6);
    tls PT_TLS FLAGS(6);
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

    .tdata :
    {
        *(.tdata .tdata.*)
        /* Keep the static TLS image contiguous and file-backed.  LLVM's
         * local-exec relocations for the NaOS target use the end of this
         * output section as TP; folding zero-initialized TLS into the same
         * section makes PT_TLS.p_filesz == PT_TLS.p_memsz and keeps the
         * runtime's TCB immediately after the complete image. */
        *(.tbss .tbss.*)
    } :data :tls

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
