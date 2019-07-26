
#include "kernel/kernel.hpp"
#include "common.hpp"
#include "kernel/ScreenPrinter.hpp"
#include "kernel/gdt.hpp"
#include "kernel/idt.hpp"
#include "kernel/memory.hpp"
#include "kernel/paging.hpp"
ExportC Unpaged_Text_Section void bss_init(void *start, void *end)
{
    char *s = (char *)start;
    char *e = (char *)end;
    if (s > e)
        return;
    const int block_size = sizeof(u64);
    int block_count = (e - s) / block_size;
    int rest = (e - s) % block_size;
    u64 *data = (u64 *)s;
    while (block_count--)
    {
        *(data) = 0;
    }
    s = (char *)data;
    while (rest--)
    {
        *(s) = 0;
    }
}

ExportC void kmain()
{
    while (1)
        ;
}
ExportC void _kstart(kernel_start_args *args);
ExportC Unpaged_Text_Section u64 _init_unpaged(const kernel_start_args *args)
{
    bss_init((void *)_bss_unpaged_start_addr, (void *)_bss_unpaged_end_addr);
    bss_init(memory::unpaged_kernel_virtaddr_to_phyaddr((void *)_bss_start_addr),
             memory::unpaged_kernel_virtaddr_to_phyaddr((void *)_bss_end_addr));

    gdt::init_before_paging();
    paging::temp_init();
    return (u64)&_kstart;
}
ExportC void _kstart(kernel_start_args *args)
{
    static_init();
    memory::init(args, 0x100000);
    paging::init();
    memory::set_virtual();
    args = (kernel_start_args *)memory::kernel_phyaddr_to_virtaddr(args);
    gdt::init_after_paging();
    tss::init((void *)args->stack_base, (void *)0x6000);
    idt::init_after_paging();

    ScreenPrinter printer(80, 25);
    gPrinter = &printer;
    printer.cls();
    printer.printf("hello kernel! \n");

    kmain();
}
