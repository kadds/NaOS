#pragma once
#include "common.hpp"
#include "kernel/common/font/font.hpp"
#include "vga.hpp"

namespace arch::device::vga
{
class output_graphics : public output
{
  private:
    char reserved_space[64];
    u32 font_width;
    u32 font_height;
    u32 text_count_per_line;
    u32 text_count_vertical;
    rectangle dirty_rectangle;
    font::font *cur_font;
    void move_pen(i32 x, i32 y, u32 newline_alignment);
    void scroll(i32 n);
    void set_bit(u32 x, u32 y, u32 color);

  public:
    using output::output;
    virtual void init() override;
    virtual void cls() override;
    virtual void putchar(char ch, font_attribute &attribute) override;
    virtual void flush(void *vraw) override;
};
} // namespace arch::device::vga