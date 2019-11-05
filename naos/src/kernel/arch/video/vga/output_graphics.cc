#include "kernel/arch/video/vga/output_graphics.hpp"
#include "kernel/common/font/font_16X8.hpp"
#include "kernel/trace.hpp"
#include "kernel/util/memory.hpp"
#include <new>

namespace arch::device::vga::graphics
{

char reserved_space[256];

u32 font_width;
u32 font_height;
u32 text_count_per_line;
u32 text_count_vertical;
rectangle dirty_rectangle;
u64 width;
u64 height;
font::font *cur_font;
byte *video_addr;

void init(u64 w, u64 h, byte *buffer, u64 pitch, u64 bbp)
{
    width = w;
    height = h;
    video_addr = buffer;
    /// XXX: check reserved space size
    cur_font = new (&reserved_space) font::font_16X8();
    cur_font->init();
    cur_font->get_size(font_width, font_height);
    text_count_per_line = width / font_width;
    text_count_vertical = height / font_height;
}

void cls(cursor_t &cur)
{
    for (u32 i = 0; i < width * height; i++)
        *((u32 *)video_addr + i) = 0;
    cur.px = 0;
    cur.py = 0;
}

void set_buffer(byte *buffer) { video_addr = buffer; }

void scroll(cursor_t &cur, i32 n)
{
    const u64 bits = height * width;
    u32 *fb = (u32 *)video_addr;

    if (likely(n > 0))
    {
        // move each line
        u64 disp = n * width * font_height;

        for (u64 i = 0; i < bits - disp; i++, fb++)
            *fb = *(fb + disp);

        for (u64 i = bits - disp; i < bits; i++, fb++)
            *fb = 0; // clean 0
        dirty_rectangle += rectangle(0, 0, width, height);
    }
    else if (n < 0)
    {
        // move each line up
        u64 disp = -n * width * font_height;
        for (u64 i = bits - 1; i >= disp; i--)
            *((u32 *)video_addr + i) = *((u32 *)video_addr + i - disp);

        for (u64 i = 0; i < disp; i++)
            *((u32 *)video_addr + i) = 0; // clean 0
        dirty_rectangle += rectangle(0, 0, width, height);
    }
    else // n == 0
    {
        return;
    }

    cur.py -= n;
}

void move_pen(cursor_t &cur, i32 x, i32 y)
{
    cur.py += y;
    cur.px += x;
    if (cur.px >= text_count_per_line)
    {
        cur.px = 0;
        cur.py++;
    }
    if (cur.py >= text_count_vertical)
        scroll(cur, cur.py - text_count_vertical + 1);
}

void putchar(cursor_t &cur, char ch, const trace::console_attribute &attribute)
{
    static u32 color_table[] = {0x000000, 0xAA0000, 0x00AA00, 0xAA5500, 0x0000AA, 0xAA00AA, 0x00AAAA, 0xAAAAAA,
                                0x555555, 0xFF5555, 0x55FF55, 0xFFFF55, 0x5555FF, 0xFF55FF, 0x55FFFF, 0xFFFFFF};

    if (ch != '\t' && ch != '\n')
    {
        void *font_data = cur_font->get_unicode(ch);

        u32 fg;
        u32 bg;

        if (attribute.is_fore_full_color())
            fg = attribute.get_foreground();
        else
            fg = color_table[attribute.get_foreground()];

        if (attribute.is_back_full_color())
            bg = attribute.get_background();
        else
            bg = color_table[attribute.get_background()];

        u32 *v_start = (u32 *)video_addr + cur.py * (u64)width * (u64)font_height + cur.px * (u64)font_width;

        for (u32 y = 0; y < font_height; y++)
        {
            for (u32 x = 0; x < font_width; x++)
            {
                *(v_start + x + y * (u64)width) = cur_font->hit(font_data, x, y) ? fg : bg;
            }
        }
        dirty_rectangle += rectangle(cur.px * font_width, cur.py * font_height, cur.px * font_width + font_width,
                                     cur.py * font_height + font_height);
        move_pen(cur, 1, 0);
    }
    else
    {
        move_pen(cur, -cur.px, 1);
    }
}

void flush(byte *vraw)
{
    u32 left = dirty_rectangle.left;
    u32 right = dirty_rectangle.right;
    if (unlikely(right > width))
        right = width;
    if (unlikely(right <= left))
        return;

    u32 top = dirty_rectangle.top;
    u32 bottom = dirty_rectangle.bottom;
    if (unlikely(bottom > height))
        bottom = height;
    if (unlikely(bottom <= top))
        return;
    if (unlikely(left == 0 && top == 0 && right == width && bottom == height))
        util::memcopy(vraw, (void *)video_addr, width * height * sizeof(u32));
    else
    {
        u32 bytes = (right - left) * sizeof(u32);
        u32 l = left * sizeof(u32);
        u32 pre_line_bytes = width * sizeof(u32);
        for (u32 y = top; y < bottom; y++)
        {
            util::memcopy((char *)vraw + y * pre_line_bytes + l, (char *)video_addr + y * pre_line_bytes + l, bytes);
        }
    }
    dirty_rectangle.clean();
}

void draw_pixel(u64 px, u64 py, u64 color) { *((u32 *)video_addr + px + py * width) = color; }

} // namespace arch::device::vga::graphics