
#include "kernel/kernel.hpp"
#include "common.hpp"
#include "kernel/arch/arch.hpp"
#include "kernel/irq.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/task.hpp"
#include "kernel/trace.hpp"

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

ExportC void _kstart(const kernel_start_args *args);

ExportC Unpaged_Text_Section u64 _init_unpaged(const kernel_start_args *args)
{
    bss_init((void *)_bss_unpaged_start_addr, (void *)_bss_unpaged_end_addr);
    bss_init((void *)_bss_start_addr, (void *)_bss_end_addr);
    arch::temp_init(args);
    return (u64)&_kstart;
}

ExportC NoReturn void _kstart(const kernel_start_args *args)
{
    static_init();
    arch::init(args);
    irq::init();
    memory::vm::init();

    task::init();
    trace::info("kernel main");
    task::start_task_idle(args);
    trace::panic("Should not be running at here.");
}
