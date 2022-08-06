#include "kernel/trace.hpp"
#include "freelibcxx/circular_buffer.hpp"
#include "freelibcxx/string.hpp"
#include "kernel/arch/com.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/cmdline.hpp"
#include "kernel/cpu.hpp"
#include "kernel/mm/buddy.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/smp.hpp"
#include "kernel/ucontext.hpp"
#include <stdarg.h>

namespace trace
{
bool output_debug = true;

freelibcxx::circular_buffer<byte> *kernel_log_buffer = nullptr;

arch::device::com::serial serial_device;
bool serial_device_enable = false;

byte *early_address = nullptr;
byte *early_pointer = nullptr;
byte *early_top_address = nullptr;

void early_init(void *address, void *top_address)
{
    early_address = static_cast<byte *>(address);
    early_top_address = static_cast<byte *>(top_address);
    early_pointer = early_address;
    bool enable_serial = cmdline::early_get_bool("kernel_log_serial", false);
    if (enable_serial) {
        serial_device.init(arch::device::com::get_control_port(0));
        serial_device_enable = true;
    }
}

void init()
{
    cmdline::space_t space = cmdline::early_get_space("kernel_log_buffer_size", cmdline::space_t(memory::page_size * 4));
    kernel_log_buffer = memory::New<freelibcxx::circular_buffer<byte>>(memory::KernelCommonAllocatorV,
                                                                       memory::KernelBuddyAllocatorV, space.space);
    // copy early_init_log
    kernel_log_buffer->write(early_address, early_pointer - early_address);
    early_address = nullptr;
    early_pointer = nullptr;
    early_top_address = nullptr;
}

freelibcxx::circular_buffer<byte> &get_kernel_log_buffer() { return *kernel_log_buffer; }

lock::spinlock_t spinlock;

void print_inner(char ch) { print_inner(&ch, 1); }

void print_inner(const char *str)
{
    u64 len = strlen(str);
    print_inner(str, len);
}

void write_to_early_buffer(const byte *buf, u64 len)
{
    if (early_top_address - early_pointer < (i64)len)
    {
        // overflow
        return;
    }
    memcpy(early_pointer, buf, len);
    early_pointer += len;
}

void print_inner(const char *str, u64 len)
{
    if (unlikely(len != 0 && str[len - 1] == 0))
        len--;

    if (likely(kernel_log_buffer != nullptr))
    {
        kernel_log_buffer->write((const byte *)str, len);
        if (unlikely(serial_device_enable)) {
            serial_device.write((const byte *)str, len);
        }
    }
    else
    {
        write_to_early_buffer((byte *)str, len);
        // early init
        // write log at once
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

NoReturn void keep_panic(const regs_t *regs)
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
    for (;;)
    {
        cpu_pause();
    }
}

ExportC NoReturn void panic_once(const char *string) { trace::panic(string); }

} // namespace trace
