#include "kernel/arch/video/vga/output_text.hpp"

namespace arch::device::vga
{
void output_text::init() {}
void output_text::cls()
{
    for (u64 i = 0; i < width * height; i++)
        *((u16 *)video_addr + i) = 0;
}
void output_text::scroll(i32 n)
{
    if (likely(n > 0))
    {
        // move each line
        u64 disp = n * width;
        volatile u16 *fb = (u16 *)video_addr;
        for (u64 i = 0; i < height * width - disp; i++, fb++)
            *fb = *(fb + disp);

        for (u64 i = height * width - disp; i < height * width; i++, fb++)
            *fb = 0; // clean 0
    }
    else if (n < 0)
    {
        // move each line up
        u64 disp = -n * width;
        for (u64 i = height * width - 1; i >= disp; i--)
            *((u16 *)video_addr + i) = *((u16 *)video_addr + i - disp);

        for (u64 i = 0; i < disp; i++)
            *((u16 *)video_addr + i) = 0; // clean 0
    }
    else // n == 0
    {
        return;
    }

    py -= n;
}
void output_text::move_pen(i32 x, i32 y, u32 newline_alignment)
{
    py += y;
    px += x;
    if (y != 0)
        px += newline_alignment;
    if (px >= width)
    {
        px = newline_alignment;
        py++;
    }
    if (py >= height)

        scroll(py - height + 1);
}
u8 similar_color_index(u32 color)
{
    static u32 color_table[] = {0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
                                0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF};
    for (int i = 0; i < 16; i++)
    {
        if (color == color_table[i])
            return i;
    }
    return 15;
}
void output_text::putchar(char ch, font_attribute &attribute)
{
    if (ch != '\t' && ch != '\n')
    {
        u8 fg = similar_color_index(attribute.get_foreground());
        u8 bg = similar_color_index(attribute.get_background());
        *((u8 *)video_addr + (px + py * width) * 2) = ch;
        *((u8 *)video_addr + (px + py * width) * 2 + 1) = fg | (bg << 4);
        move_pen(1, 0, attribute.get_newline_alignment());
    }
    else
    {
        move_pen(-px, 1, attribute.get_newline_alignment());
    }
}
} // namespace arch::device::vga
