
#include "kernel/kernel.hpp"
#include "common.hpp"
#include "kernel/arch/arch.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/clock.hpp"
#include "kernel/fs/rootfs/rootfs.hpp"
#include "kernel/fs/vfs/vfs.hpp"
#include "kernel/irq.hpp"
#include "kernel/ksybs.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/task.hpp"
#include "kernel/timer.hpp"
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

ExportC NoReturn void _kstart(const kernel_start_args *args);

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
    memory::listen_page_fault();
    trace::debug("VFS init...");
    fs::vfs::init();
    trace::debug("Root file system init...");
    fs::rootfs::init(memory::kernel_phyaddr_to_virtaddr((byte *)args->get_rfs_ptr()), args->rfs_size);
    ksybs::init();

    task::init();
    trace::info("kernel main");

    timer::init();

    arch::last_init();
    task::start_task_idle(args);
    trace::panic("Unreachable control flow in _kstart.");
}
