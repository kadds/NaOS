#include "kernel/trace.hpp"
#include "kernel/arch/com.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/arch/video/vga/vga.hpp"
#include "kernel/cpu.hpp"
#include "kernel/mm/buddy.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/smp.hpp"
#include "kernel/ucontext.hpp"
#include "kernel/util/str.hpp"
#include <stdarg.h>

namespace trace
{
bool output_debug = true;

util::ring_buffer *ring_buffer = nullptr;

arch::device::com::serial serial_device;

void early_init() { serial_device.init(arch::device::com::get_control_port(0)); }

void init()
{
    ring_buffer = memory::New<util::ring_buffer>(memory::KernelCommonAllocatorV, memory::page_size, 8,
                                                 util::ring_buffer::strategy::discard, memory::KernelCommonAllocatorV,
                                                 memory::KernelBuddyAllocatorV);
}
util::ring_buffer &get_kernel_log_buffer() { return *ring_buffer; }

lock::spinlock_t spinlock;

void print_inner(const char *str)
{
    u64 len = util::strlen(str);
    print_inner(str, len);
}

void print_inner(const char *str, u64 len)
{
    if (len != 0 && str[len - 1] == 0)
        len--;
    if (unlikely(ring_buffer != nullptr))
    {
        ring_buffer->write((const byte *)str, len);
        serial_device.write((const byte *)str, len);
    }
    else
    {
        uctx::UninterruptibleContext icu;
        // early init
        // write log at once
        u64 len = arch::device::vga::putstring(str, 0);
        serial_device.write((const byte *)str, len);
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
    uctx::UninterruptibleContext uic;
    print_stack(regs, 30);
    trace::print<trace::PrintAttribute<trace::CFG::Pink>>("Kernel panic! Try to connect with debugger.\n");
    trace::print<trace::PrintAttribute<trace::TextAttribute::Reset>>();
    arch::device::vga::flush_kbuffer();
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
