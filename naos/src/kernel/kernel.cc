
#include "common.hpp"
#include "kernel/ScreenPrinter.hpp"
#include "kernel/gdt.hpp"
#include "kernel/idt.hpp"
#include "kernel/memory.hpp"
#include "kernel/paging.hpp"

ExportC void kmain()
{
    while (1)
        ;
}

ExportC void _start_kernel(kernel_start_args *args)
{
    static_init();
    gdt::init_before_paging();
    idt::init_before_paging();
    memory::init(args->data_base + args->data_size, 0x100000);
    paging::init();
    gdt::init_after_paging();
    tss::init((void *)args->stack_base, (void *)0x7c00);
    idt::init_after_paging();

    ScreenPrinter printer(80, 25);
    gPrinter = &printer;
    printer.cls();
    printer.printf("hello kernel! ");
    int a = args->data_size;
    int b = 0;
    int c = a / b;
    printer.printf("%d", c);

    kmain();
}
