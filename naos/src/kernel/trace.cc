#include "kernel/trace.hpp"
#include "kernel/arch/com.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/arch/video/vga/vga.hpp"
#include "kernel/mm/buddy.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/ucontext.hpp"
#include "kernel/util/str.hpp"
#include <stdarg.h>

namespace trace
{
bool output_debug = true;

util::ring_buffer *ring_buffer = nullptr;
byte *line_buffer;

arch::device::com::serial serial_device;

void early_init() { serial_device.init(arch::device::com::select_port(0)); }

void init()
{
    ring_buffer = memory::New<util::ring_buffer>(memory::KernelCommonAllocatorV, memory::page_size, 8,
                                                 util::ring_buffer::strategy::discard, memory::KernelCommonAllocatorV,
                                                 memory::KernelBuddyAllocatorV);
    line_buffer = (byte *)memory::KernelBuddyAllocatorV->allocate(1, 0);
}
util::ring_buffer &get_kernel_log_buffer() { return *ring_buffer; }

lock::spinlock_t spinlock;
bool last_IF;

void begin_print()
{
    last_IF = arch::idt::save_and_disable();
    spinlock.lock();
}

void end_print()
{
    spinlock.unlock();
    if (last_IF)
    {
        arch::idt::enable();
    }
    else
    {
        arch::idt::disable();
    }
}

void print_inner(const char *str)
{
    if (unlikely(ring_buffer != nullptr))
    {
        u64 len = util::strlen(str);
        ring_buffer->write((const byte *)str, len);
        serial_device.write((const byte *)str, len);
    }
    else
    {
        // early init
        // write log at once
        u64 len = arch::device::vga::putstring(str, 0);
        serial_device.write((const byte *)str, len);
    }
}

NoReturn void keep_panic(const regs_t *regs)
{
    uctx::UnInterruptableContext uic;
    trace::begin_print();
    print_stack(regs, 30);
    trace::print<trace::PrintAttribute<trace::CFG::Pink>>("Kernel panic! Try to connect with debugger.\n");
    trace::print<trace::PrintAttribute<trace::TextAttribute::Reset>>();
    trace::end_print();
    arch::device::vga::flush();
    for (;;)
    {
        cpu_pause();
    }
}

ExportC NoReturn void panic_once(const char *string) { trace::panic(string); }

} // namespace trace
