#include "kernel/kernel.hpp"
#include "common.hpp"
#include "kernel/arch/arch.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/clock.hpp"
#include "kernel/cmdline.hpp"
#include "kernel/cpu.hpp"
#include "kernel/dev/device.hpp"
#include "kernel/fs/pipefs/pipefs.hpp"
#include "kernel/fs/rootfs/rootfs.hpp"
#include "kernel/fs/vfs/defines.hpp"
#include "kernel/fs/vfs/file.hpp"
#include "kernel/fs/vfs/vfs.hpp"
#include "kernel/handle.hpp"
#include "kernel/io/io_manager.hpp"
#include "kernel/irq.hpp"
#include "kernel/ksybs.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/smp.hpp"
#include "kernel/task.hpp"
#include "kernel/terminal.hpp"
#include "kernel/timer.hpp"
#include "kernel/trace.hpp"

kernel_start_args *kernel_args;

ExportC Unpaged_Text_Section void bss_init(void *start, void *end)
{
    u64 s = reinterpret_cast<u64>(start);
    u64 e = reinterpret_cast<u64>(end);
    if (unlikely(s > e))
        return;

    const u64 block_size = sizeof(u64);
    u64 num = (e - s) / block_size;
    u64 rest = (e - s) % block_size;

    u64 *data = reinterpret_cast<u64 *>(start);
    while (num--)
    {
        *(data) = 0;
    }
    char *p = reinterpret_cast<char *>(data);
    while (rest--)
    {
        *(p) = 0;
    }
}

ExportC Unpaged_Text_Section void _init_unpaged(const kernel_start_args *args)
{
    if (args != 0) // bsp
    {
        bss_init((void *)_bss_unpaged_start, (void *)_bss_unpaged_end);
    }
    arch::temp_init(args);
}

u64 build_version_timestamp = BUILD_VERSION_TS;

handle_t<fs::vfs::file>* dmesg_file;

void trace_callback(const byte *data, u64 len) 
{
    (*dmesg_file)->write(data, len, 0);
}

void fs_init() 
{
    // create /var/log/dmesg
    fs::vfs::mkdir("/var", fs::vfs::global_root, fs::vfs::global_root, 0);
    fs::vfs::mkdir("/var/log", fs::vfs::global_root, fs::vfs::global_root, 0);
    auto file = fs::vfs::open("/var/log/dmesg", fs::vfs::global_root, fs::vfs::global_root,
        fs::mode::write, fs::path_walk_flags::auto_create_file | fs::path_walk_flags::file);
    if (!file) 
    {
        trace::panic("create dmesg fail failed");
    }

    dmesg_file = memory::KernelCommonAllocatorV->New<handle_t<fs::vfs::file>>(file);

    trace::register_callback(true, trace_callback);
}

NoReturn void kstart_bsp(kernel_start_args *args)
{
    memset((void *)((u64)_bss_start), 0, (u64)_bss_end - (u64)_bss_start);
    kernel_args = args;
    static_init();

    arch::early_init(args);

    trace::info("Build version ", build_version_timestamp);
    cmdline::init();

    arch::init(args);
    cpu::init();
    irq::init();
    memory::listen_page_fault();

    arch::init2();

    timer::init();
    SMP::init();

    // -----spec routine for bsp----
    fs::vfs::init();
    fs::ramfs::init();
    fs::rootfs::init(memory::pa2va<byte*>(phy_addr_t::from(args->rfsimg_start)), args->rfsimg_size);
    fs::pipefs::init();
    ksybs::init();

    fs_init();
    term::init();

    io::init();
    dev::init();

    task::init();
    arch::init_drivers();
    trace::info("Bsp kernel main is running");
    arch::post_init();
    //  -----------------------------
    task::start_task_idle();
    trace::panic("Unreachable control flow in _kstart");
}

NoReturn void kstart_ap()
{
    arch::early_init(nullptr);

    arch::init(nullptr);
    cpu::init();
    irq::init();
    timer::init();
    SMP::init();
    task::init();
    task::start_task_idle();
    trace::panic("Unreachable control flow in _kstart");
}

ExportC NoReturn void _kstart(kernel_start_args *args)
{
    if (args == 0) // ap
    {
        kstart_ap();
    }
    else
    {
        kstart_bsp(args);
    }
}
