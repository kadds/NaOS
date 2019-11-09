#include "kernel/trace.hpp"
#include "kernel/arch/com.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/arch/video/vga/vga.hpp"
#include "kernel/mm/buddy.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/ucontext.hpp"
#include "kernel/util/ring_buffer.hpp"
#include "kernel/util/str.hpp"
#include <stdarg.h>

namespace trace
{
namespace PrintAttr
{
u8 testable_attributes[] = {bold, italic, underline, blink, reverse, hide, framed};
u64 testable_attributes_flags =
    (1ul << bold | 1ul << italic | 1ul << underline | 1ul << blink | 1ul << reverse | 1ul << hide | 1ul << framed);
} // namespace PrintAttr

bool output_debug = true;
const console_attribute default_console_attribute(
    ((u64)trace::Color::LightGray::index | ((u64)trace::Color::Black::index << 32) | (1ul) << 61),
    (1ul << PrintAttr::default_background | 1ul << PrintAttr::default_foreground));

console_attribute kernel_console_attribute;
util::ring_buffer *ring_buffer = nullptr;
int format_SGR(char *output, console_attribute &request_attribute);

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
    if (current_attribute.has_changed())
    {
        format_SGR(current_attribute.sgr_string, current_attribute);
        current_attribute.clean_changed();
        char *p = current_attribute.sgr_string;
        while (*p != '\0')
            arch::device::com::write((byte)*p++);
    }

    u64 len = arch::device::vga::putstring(str, 0, current_attribute);
    if (unlikely(ring_buffer != nullptr))
    {
        ring_buffer->write(str, len);
    }
    arch::device::com::write((const byte *)str, len);
}

u32 get_num(const char *&str)
{
    u32 c = 0;
    while (*str != '\0' && *str <= '9' && *str >= '0')
    {
        c = c * 10 + *str - '0';
        str++;
    }

    return c;
}

bool is_split(char c) { return c == ';' || c == ':'; }

const char *format_SGR(const char *str, console_attribute &current_attribute);

/// https://en.wikipedia.org/wiki/ANSI_escape_code
/// print escape sequences
void print_SGR(const char *str, console_attribute &current_attribute)
{
    uctx::UnInterruptableContext icu;
    while (*str != '\0')
    {
        const char *sgr_start = str, *next, *new_next = str;
        u64 len;
        do
        {
            next = new_next;
            new_next = format_SGR(next, current_attribute);
            len = new_next - sgr_start;
        } while (new_next != next);

        if (unlikely(ring_buffer != nullptr))
            ring_buffer->write(str, len);
        arch::device::com::write((const byte *)str, len);
        str = next;
        if (*next == '\0')
            break;

        arch::device::vga::putstring(str, 1, current_attribute);
        if (unlikely(ring_buffer != nullptr))
            ring_buffer->write(str, 1);
        arch::device::com::write((byte)*str);
        str++;
    }
}

const char *format_SGR(const char *str, console_attribute &current_attribute)
{
    console_attribute &attr = current_attribute;
    const char *s = str;
    if (*s == '\e')
    {
        if (*(s + 1) == '[')
        {
            s += 2;
            do
            {
                u32 n = get_num(s);
                if (*s == '\0')
                    return s;
                if ((n >= 30 && n <= 37) || (n >= 90 && n <= 97))
                {
                    attr.clean_fore_full_color();
                    if (n <= 37)
                        attr.set_foreground(n - 30);
                    else
                        attr.set_foreground(n - 90 + 8);
                }
                else if (n == 38)
                {
                    if (!is_split(*s))
                        return s;
                    if (2 != get_num(++s))
                        return s;
                    if (*s == '\0')
                        return s;
                    if (!is_split(*s))
                        return s;
                    u32 r = get_num(++s);
                    if (*s == '\0')
                        return s;
                    if (!is_split(*s))
                        return s;
                    u32 g = get_num(++s);
                    if (*s == '\0')
                        return s;
                    if (!is_split(*s))
                        return s;
                    u32 b = get_num(++s);
                    if (*s == '\0')
                        return s;
                    attr.set_fore_full_color();
                    attr.set_foreground((r & 0xFF) << 16 | (g & 0xFF) << 8 | (b & 0xFF));
                }
                else if (n == 39)
                {
                    attr.clean_fore_full_color();
                    attr.set_foreground(default_console_attribute.get_foreground());
                }
                else if ((n >= 40 && n <= 47) || (n >= 100 && n <= 107))
                {
                    attr.clean_back_full_color();
                    if (n <= 47)
                        attr.set_background(n - 40);
                    else
                        attr.set_background(n - 100 + 8);
                }
                else if (n == 48)
                {
                    if (!is_split(*s))
                        return s;
                    if (2 != get_num(++s))
                        return s;
                    if (*s == '\0')
                        return s;
                    if (!is_split(*s))
                        return s;
                    u32 r = get_num(++s);
                    if (*s == '\0')
                        return s;
                    if (!is_split(*s))
                        return s;
                    u32 g = get_num(++s);
                    if (*s == '\0')
                        return s;
                    if (!is_split(*s))
                        return s;
                    u32 b = get_num(++s);
                    if (*s == '\0')
                        return s;
                    attr.set_back_full_color();
                    attr.set_background((r & 0xFF) << 16 | (g & 0xFF) << 8 | (b & 0xFF));
                }
                else if (n == 49)
                {
                    attr.clean_back_full_color();
                    attr.set_background(default_console_attribute.get_background());
                }
                else if (n <= 64 && PrintAttr::testable_attributes_flags & (1ul << n))
                {
                    attr.set_attribute(n);
                }
                else if (n == 0)
                {
                    attr = default_console_attribute;
                }
                if (!is_split(*s++))
                    return s;

            } while (*s != 'm');
        }
    }
    return s;
}

int i2s(u32 c, char *buffer)
{
    char *ptr = buffer;
    if (c >= 10)
    {
        if (c >= 100)
        {
            *ptr++ = '0' + c / 100 % 10;
        }
        *ptr++ = '0' + c / 10 % 10;
    }
    *ptr++ = '0' + c % 10;
    return (int)(ptr - buffer);
}

/// get escape sequences str head
int format_SGR(char *output, console_attribute &request_attribute)
{
    char *cur = output;
    console_attribute &cat = request_attribute;
    *cur++ = '\e';
    *cur++ = '[';
    if (cat.has_any_attribute())
    {
        if (cat.has_attribute(PrintAttr::reset))
        {
            *cur++ = '0';
            *cur++ = 'm';
            *cur = '\0';
            cat.clean_attribute(PrintAttr::reset);

            return cur - output;
        }
        else
        {
            for (auto i : PrintAttr::testable_attributes)
                if (cat.has_attribute(i))
                {
                    cur += i2s(i, cur);
                    *cur++ = ';';
                }
        }
    }
    if (cat.has_attribute(PrintAttr::default_foreground))
    {
        *cur++ = '3';
        *cur++ = '9';
    }
    else if (cat.is_fore_full_color())
    {
        *cur++ = '3';
        *cur++ = '8';
        *cur++ = ';';
        *cur++ = '2';
        *cur++ = ';';
        u32 bg = cat.get_foreground();
        cur += i2s((bg & 0xFF0000) >> 16, cur);
        *cur++ = ';';
        cur += i2s((bg & 0x00FF00) >> 8, cur);
        *cur++ = ';';
        cur += i2s((bg & 0x0000FF), cur);
    }
    else
    {
        u32 index = cat.get_foreground();
        if (index > 15)
            index = 15;
        if (index > 7)
            index += 90 - 8;
        else
            index += 30;
        cur += i2s(index, cur);
    }
    *cur++ = ';';

    if (cat.has_attribute(PrintAttr::default_background))
    {
        *cur++ = '4';
        *cur++ = '9';
    }
    else if (cat.is_back_full_color())
    {
        *cur++ = '4';
        *cur++ = '8';
        *cur++ = ';';
        *cur++ = '2';
        *cur++ = ';';
        u32 bg = cat.get_background();
        cur += i2s((bg & 0xFF0000) >> 16, cur);
        *cur++ = ';';
        cur += i2s((bg & 0x00FF00) >> 8, cur);
        *cur++ = ';';
        cur += i2s((bg & 0x0000FF), cur);
    }
    else
    {
        u32 index = cat.get_background();
        if (index > 15)
            index = 15;
        if (index > 7)
            index += 100 - 8;
        else
            index += 40;
        cur += i2s(index, cur);
    }
    *cur++ = 'm';
    *cur = '\0';
    return cur - output;
}

NoReturn void keep_panic(const arch::idt::regs_t *regs)
{
    uctx::UnInterruptableContext uic;
    print_stack(regs, 30);
    trace::print(kernel_console_attribute, trace::Foreground<trace::Color::Pink>(),
                 "Kernel panic! Try to connect with debugger.", trace::PrintAttr::Reset(), '\n');
    arch::device::vga::flush();
    for (;;)
        ;
}

} // namespace trace
