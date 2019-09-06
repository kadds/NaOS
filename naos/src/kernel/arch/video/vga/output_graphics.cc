#include "kernel/arch/video/vga/output_graphics.hpp"
#include "kernel/common/font/font_16X8.hpp"
#include <new>
namespace arch::device::vga
{

void output_graphics::init()
{
    cur_font = new (&reserved_space) font::font_16X8();
    cur_font->init();
    cur_font->get_size(font_width, font_height);
    text_count_per_line = width / font_width;
    text_count_vertical = height / font_height;
}
void output_graphics::cls()
{
    for (u32 i = 0; i < width * height; i++)
        *((u32 *)video_addr + i) = 0;
}
void output_graphics::set_bit(u32 x, u32 y, u32 color) { *((u32 *)video_addr + x + y * width) = color; }
void output_graphics::scroll(i32 n)
{
    const u64 bits = height * width;
    volatile u32 *fb = (u32 *)video_addr;

    if (likely(n > 0))
    {
        // move each line
        u64 disp = n * width * font_height;

        for (u64 i = 0; i < bits - disp; i++, fb++)
            *fb = *(fb + disp);

        for (u64 i = bits - disp; i < bits; i++, fb++)
            *fb = 0; // clean 0
    }
    else if (n < 0)
    {
        // move each line up
        u64 disp = -n * width * font_height;
        for (u64 i = bits - 1; i >= disp; i--)
            *((u32 *)video_addr + i) = *((u32 *)video_addr + i - disp);

        for (u64 i = 0; i < disp; i++)
            *((u32 *)video_addr + i) = 0; // clean 0
    }
    else // n == 0
    {
        return;
    }

    py -= n;
}
void output_graphics::move_pen(i32 x, i32 y, u32 newline_alignment)
{
    py += y;
    px += x;
    if (y != 0)
        px += newline_alignment;
    if (px >= text_count_per_line)
    {
        px = newline_alignment;
        py++;
    }
    if (py >= text_count_vertical)
        scroll(py - text_count_vertical + 1);
}

void output_graphics::putchar(char ch, font_attribute &attribute)
{
    if (ch != '\t' && ch != '\n')
    {
        void *font_data = cur_font->get_unicode(ch);

        u32 fg = attribute.get_foreground();
        u32 bg = attribute.get_background();

        u32 *v_start = (u32 *)video_addr + py * (u64)width * (u64)font_height + px * (u64)font_width;

        for (u32 y = 0; y < font_height; y++)
        {
            for (u32 x = 0; x < font_width; x++)
            {
                *(v_start + x + y * (u64)width) = cur_font->hit(font_data, x, y) ? fg : bg;
            }
        }

        move_pen(1, 0, attribute.get_newline_alignment());
    }
    else
    {
        move_pen(-px, 1, attribute.get_newline_alignment());
    }
}
} // namespace arch::device::vga