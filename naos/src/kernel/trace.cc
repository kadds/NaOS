#include "kernel/trace.hpp"
#include "common.hpp"
#include "freelibcxx/circular_buffer.hpp"
#include "freelibcxx/string.hpp"
#include "kernel/arch/com.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/arch/video/vga/vga.hpp"
#include "kernel/cmdline.hpp"
#include "kernel/cpu.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/smp.hpp"
#include "kernel/terminal.hpp"
#include "kernel/ucontext.hpp"
#include <stdarg.h>

namespace trace
{
bool output_debug = true;

freelibcxx::circular_buffer<byte> *kernel_log_buffer = nullptr;

arch::device::com::serial serial_device;
bool serial_device_enable = false;
int slowdown = 0;

void early_init()
{
    bool enable_serial = cmdline::early_get_bool("kernel_log_serial", false);
    if (enable_serial) {
        serial_device.init(arch::device::com::get_control_port(0));
        serial_device_enable = true;
    }
    slowdown = cmdline::early_get_uint("slowdown", 0);
}

void init()
{
    cmdline::space_t space = cmdline::early_get_space("kernel_log_buffer_size", cmdline::space_t(memory::page_size * 4));
    kernel_log_buffer = memory::New<freelibcxx::circular_buffer<byte>>(memory::KernelCommonAllocatorV,
                                                                       memory::KernelBuddyAllocatorV, space.space);
}

freelibcxx::circular_buffer<byte> &get_kernel_log_buffer() { return *kernel_log_buffer; }

lock::spinlock_t spinlock;

void print_klog(char ch) { print_klog(&ch, 1); }

void print_klog(const char *str)
{
    u64 len = strlen(str);
    print_klog(str, len);
}

void print_klog(const char *str, u64 len)
{
    if (unlikely(len != 0 && str[len - 1] == 0))
        len--;

    if (unlikely(slowdown))
    {
        volatile int v = 0;
        for (int i = 0; i < slowdown * 10'000; i++)
        {
            v = v + 1;
        }
    }

    if (likely(kernel_log_buffer != nullptr))
    {
        kernel_log_buffer->write((const byte *)str, len);
        term::write_to_klog(freelibcxx::const_string_view(str, len));
        if (unlikely(serial_device_enable)) {
            serial_device.write((const byte *)str, len);
        }
    }
    else
    {
        // early init
        // write log immediately
        term::write_to_klog(freelibcxx::const_string_view(str, len));
        if (unlikely(serial_device_enable)) {
            serial_device.write((const byte *)str, len);
        }
    }
}

void cpu_wait_panic(u64 data)
{
    for (;;)
    {
        cpu_pause();
    }
}

void keep_panic(const regs_t *regs)
{
    {
        uctx::RawSpinLockUninterruptibleContext icu(spinlock);
        print_stack(regs, 30);
        trace::print<trace::PrintAttribute<trace::CFG::Pink>>("Kernel panic! Try to connect with debugger.\n");
        trace::print<trace::PrintAttribute<trace::TextAttribute::Reset>>();
        for (u32 id = 0; id < cpu::count(); id++)
        {
            if (id != cpu::current().id())
                SMP::call_cpu(id, cpu_wait_panic, 0);
        }
    }
    volatile bool stop = false;
    while (!stop)
    {
        cpu_pause();
    }
}

ExportC NoReturn void panic_once(const char *string) { trace::panic(string); }

} // namespace trace
