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
char early_buffer[4096];
int early_buffer_offset = 0;
lock::spinlock_t spinlock;
trace_callback tcallback = nullptr;

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
    // 32Kib
    cmdline::space_t space = cmdline::early_get_space("kernel_log_buffer_size", cmdline::space_t(memory::page_size * 8));
    kernel_log_buffer = memory::New<freelibcxx::circular_buffer<byte>>(memory::KernelCommonAllocatorV,
                                                                       memory::KernelBuddyAllocatorV, space.space);
    // copy early_buffer to kernel_log_buffer
    kernel_log_buffer->write(reinterpret_cast<const byte*>(early_buffer), early_buffer_offset);
}

void register_callback(bool trace_all, trace_callback callback) 
{
    tcallback = callback;
    if (trace_all) 
    {
        byte buf[128];
        while (true) 
        {
            auto len = kernel_log_buffer->read(buf, sizeof(buf));
            tcallback(buf, len);
            if (len == 0) 
            {
                break;
            }
        }
    }
}

void print_klog(char ch) { print_klog(&ch, 1); }

void print_klog(const char *str)
{
    u64 len = strlen(str);
    print_klog(str, len);
}

void write_early_buffer(const char *str, u64 len) 
{
    const char overflow_str[] = "\n...kbuffer overflow.\n";
    u64 rest = sizeof(early_buffer) - early_buffer_offset - sizeof(overflow_str);

    bool overflow = false;
    if (len > rest) 
    {
        overflow = true;
        len = rest;
    }

    memcpy(early_buffer + early_buffer_offset, str, len);
    early_buffer_offset += len;

    if (overflow) 
    {
        if (rest >= sizeof(overflow_str)) 
        {
            strcpy(early_buffer + early_buffer_offset, overflow_str); 
            early_buffer_offset += sizeof(overflow_str);
        }
    }
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
        if (tcallback) [[likely]] 
        {
            tcallback(reinterpret_cast<const byte *>(str), len);
        }

        if (serial_device_enable) [[unlikely]]
        {
            serial_device.write((const byte *)str, len);
        }
    }
    else
    {
        // write log immediately
        term::write_to_klog(freelibcxx::const_string_view(str, len));
        write_early_buffer(str, len);

        if (serial_device_enable) [[unlikely]]
        {
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
