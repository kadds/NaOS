
#include "kernel/kernel.hpp"
#include "common.hpp"
#include "kernel/arch/arch.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/clock.hpp"
#include "kernel/cpu.hpp"
#include "kernel/dev/device.hpp"
#include "kernel/fs/pipefs/pipefs.hpp"
#include "kernel/fs/rootfs/rootfs.hpp"
#include "kernel/fs/vfs/vfs.hpp"
#include "kernel/io/io_manager.hpp"
#include "kernel/irq.hpp"
#include "kernel/ksybs.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/smp.hpp"
#include "kernel/task.hpp"
#include "kernel/timer.hpp"
#include "kernel/trace.hpp"
#include "kernel/util/memory.hpp"

kernel_start_args *kernel_args;

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

ExportC Unpaged_Text_Section u64 _init_unpaged(const kernel_start_args *args)
{
    if (args != 0) // bsp
    {
        bss_init((void *)_bss_unpaged_start, (void *)_bss_unpaged_end);
    }
    arch::temp_init(args);
    return (u64)&_kstart;
}

u64 timestamp_version = BUILD_VERSION_TS;

ExportC NoReturn void _kstart(kernel_start_args *args)
{
    if (args == 0) // ap
    {
        arch::init(args);
        cpu::init();
        irq::init();
        timer::init();
        SMP::init();
        task::init();
        task::start_task_idle();
        trace::panic("Unreachable control flow in _kstart");
    } // else bsp
    util::memzero((void *)((u64)_bss_start + (u64)base_phy_addr), (u64)_bss_end - (u64)_bss_start);
    kernel_args = args;
    static_init();
    arch::init(args);
    trace::info("Build version ", timestamp_version);
    cpu::init();
    irq::init();
    memory::listen_page_fault();
    timer::init();
    trace::debug("SMP init");
    SMP::init();
    trace::debug("VFS init");
    fs::vfs::init();
    trace::debug("Root file system init");
    fs::ramfs::init();
    fs::rootfs::init(memory::kernel_phyaddr_to_virtaddr((byte *)args->rfsimg_start), args->rfsimg_size);
    fs::pipefs::init();
    ksybs::init();
    io::init();
    dev::init();
    task::init();
    arch::init_drivers();
    trace::info("Bsp kernel main is running");
    arch::last_init(); // auto flush video now
    task::start_task_idle();
    trace::panic("Unreachable control flow in _kstart");
}
