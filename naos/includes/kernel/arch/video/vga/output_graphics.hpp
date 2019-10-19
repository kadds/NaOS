#pragma once
#include "common.hpp"
#include "kernel/common/font/font.hpp"
#include "vga.hpp"

namespace arch::device::vga
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

class output_graphics : public output
{
  private:
    // Reserve for font struct. So font struct size must less than 64 bytes.
    char reserved_space[64];
    u32 font_width;
    u32 font_height;
    u32 text_count_per_line;
    u32 text_count_vertical;
    rectangle dirty_rectangle;
    font::font *cur_font;

    void move_pen(i32 x, i32 y);
    void scroll(i32 n);
    void set_bit(u32 x, u32 y, u32 color);

  public:
    using output::output;
    virtual void init() override;
    virtual void cls() override;
    virtual void putchar(char ch, const trace::console_attribute &attribute) override;
    virtual void flush(void *vraw) override;
};
} // namespace arch::device::vga