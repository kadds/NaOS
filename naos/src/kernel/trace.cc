#include "kernel/trace.hpp"
#include "kernel/arch/com.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/arch/video/vga/vga.hpp"
#include "kernel/ucontext.hpp"
#include "kernel/util/ring_buffer.hpp"
#include "kernel/util/str.hpp"
#include <stdarg.h>

namespace trace
{
namespace PrintAttr
{
u8 testable_attributes[] = {bold,  italic, underline, blink, reverse, hide, default_foreground, default_background,
                            framed};

} // namespace PrintAttr

bool output_debug = true;
const console_attribute
    default_console_attribute((u64)trace::Color::LightGray::index | ((u64)trace::Color::Black::index << 32), 0);

console_attribute kernel_console_attribute;
util::ring_buffer *ring_buffer = nullptr;

void early_init()
{
    kernel_console_attribute = default_console_attribute;
    arch::device::com::init();
}

void init()
{
    ring_buffer = memory::New<util::ring_buffer>(memory::KernelCommonAllocatorV,
                                                 memory::KernelBuddyAllocatorV->allocate(memory::page_size, 0),
                                                 memory::page_size);
}

void print_inner(const char *str, console_attribute &current_attribute)
{
    arch::device::vga::putstring(str, current_attribute);
    if (unlikely(ring_buffer != nullptr))
    {
        ring_buffer->write(str, util::strlen(str));
    }
    while (*str != 0)
    {
        arch::device::com::write((byte)*str++);
    }
}

/// https://en.wikipedia.org/wiki/ANSI_escape_code
/// TODO: print escaped str
void print_SGR(const char *str, console_attribute &current_attribute)
{
    console_attribute &attr = current_attribute;
    const char *s = str;
    while (*s != 0)
    {
        if (*s == '\e')
        {
            if (*s + 1 == '[')
            {
                int c = 0;
                do
                {
                    if (*s <= '9' && *s >= '0')
                        c = c * 10 + *s - '0';
                    else
                    {
                        if (c > 108)
                            return;
                        else
                            attr.set_attribute(c);
                    }
                } while (*s != 'm');
            }
        }

        arch::device::com::write((byte)*s);
        s++;
    }

    if (unlikely(ring_buffer != nullptr))
    {
        ring_buffer->write(str, (u64)(s - str));
    }
}

/// TODO: get escape str head
void format_SGR(const char *str, char *output, console_attribute &request_attribute)
{
    char txt[32];
    char *cur = txt;
    *cur = 0;
    console_attribute cat = request_attribute;

    if (cat.has_any_attribute())
    {
        if (cat.has_attribute(PrintAttr::reset))
        {
            util::strcopy(cur, "\e[0m");
            return;
        }

        *cur++ = '\e';
        *cur++ = '[';
        for (auto i : PrintAttr::testable_attributes)
        {
            if (cat.has_attribute(i))
            {
                *cur++ = i % 10 + '0';

                if (i >= 10)
                {
                    *cur++ = i / 10 % 10 + '0';
                }
            }
        }
        // if (cat.is_full_color())
        // {
        //     if (cat.has_attribute(PrintAttr::set_foreground))
        //     {
        //     }
        // }
        // else
        // {
        //     int index;
        //     index = cat.get_foreground();
        //     if (index >= 16)
        //         ;
        //     if (index < 10)
        //         *cur++ = '0' + index;
        //     else
        //     {
        //         *cur++ = '0' + index % 10;
        //         *cur++ = '0' + index / 10 % 10;
        //     }
        // }
        if (cat.has_attribute(PrintAttr::set_foreground))
        {
            // if (cat.is_full_color())
            // {
            //     util::strcopy(cur, "38;");
            //     *cur += 3;
            //     *cur++ = '2';
            //     *cur++ = ';';
            // }
        }
        *cur++ = 'm';
    }
    request_attribute = cat;
}

NoReturn void keep_panic(const arch::idt::regs_t *regs)
{
    uctx::UnInterruptableContext uic;
    print_stack(regs, 30);
    trace::print(kernel_console_attribute, trace::Foreground<trace::Color::Pink>(),
                 "Kernel panic! Try to connect with debugger.\n", trace::PrintAttr::Reset());
    arch::device::vga::flush();
    for (;;)
        ;
}

} // namespace trace
