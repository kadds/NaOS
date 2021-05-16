#pragma once
#include "../../../common/font/font.hpp"
#include "common.hpp"
#include "vga.hpp"

namespace arch::device::vga::graphics
{
struct rectangle
{
    u32 left;
    u32 top;
    u32 right;
    u32 bottom;
    rectangle(){};
    rectangle(u32 left, u32 top, u32 right, u32 bottom)
        : left(left)
        , top(top)
        , right(right)
        , bottom(bottom){};
    rectangle &operator+=(const rectangle &rc)
    {
        if (left == right || top == bottom)
            *this = rc;

        if (left > rc.left)
            left = rc.left;
        if (top > rc.top)
            top = rc.top;

        if (right < rc.right)
            right = rc.right;
        if (bottom < rc.bottom)
            bottom = rc.bottom;

        return *this;
    }
    void clean()
    {
        left = 0;
        top = 0;
        right = 0;
        bottom = 0;
    }
};

void init(u64 w, u64 h, byte *buffer, u64 pitch, u64 bbp);
void cls(text_cursor_t &cur);
void putchar(text_cursor_t &cur, char ch);

void set_buffer(byte *buffer);

void draw_line(u64 x, u64 y, u64 color);
void draw_rectangle(const rectangle &rect, u64 color);
void draw_pixel(u64 px, u64 py, u64 color);
void draw_text(u64 px, u64 py, const char *text);
void draw_placeholder(text_cursor_t &cur, u32 color);

void flush(byte *vraw);
} // namespace arch::device::vga::graphics