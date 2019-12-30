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
u64 line_bytes;
u64 full_bytes;
font::font *cur_font;
byte *video_addr;
u64 window_height;
u64 char_window_height;

void init(u64 w, u64 h, byte *buffer, u64 pitch, u64 bbp)
{
    width = w;
    height = h;
    line_bytes = w * sizeof(u32);
    full_bytes = line_bytes * h;
    video_addr = buffer;
    window_height = 0;
    char_window_height = 0;
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
    window_height = 0;
    char_window_height = 0;
}

void set_buffer(byte *buffer) { video_addr = buffer; }

void scroll(cursor_t &cur, i32 n)
{
    if (n <= 0)
        return;
    char_window_height = (char_window_height + n) % text_count_vertical;
    u64 old_window_height = window_height;

    if (char_window_height == 0)
    {
        window_height = 0;
        util::memzero(video_addr + old_window_height * line_bytes, full_bytes - old_window_height * line_bytes);
        util::memzero(video_addr, window_height * line_bytes);
    }
    else
    {
        window_height = (window_height + n * font_height) % height;
        if (old_window_height > window_height)
        {
            util::memzero(video_addr + old_window_height * line_bytes, full_bytes - old_window_height * line_bytes);
            util::memzero(video_addr, window_height * line_bytes);
        }
        else
        {
            util::memzero(video_addr + old_window_height * line_bytes,
                          (window_height - old_window_height) * line_bytes);
        }
    }

    dirty_rectangle = rectangle(0, 0, width, height);
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

u32 get_py(cursor_t &cur) { return (cur.py + char_window_height) % (text_count_vertical); }

u32 *get_video_line_start(cursor_t &cur)
{
    return (u32 *)((byte *)video_addr + line_bytes * get_py(cur) * (u64)font_height);
}

struct font_state
{
    u32 foreground, background;
    u64 flags;
};

font_state global_state;
font_state current_esc_state;
u64 current_num;
int esc_state;
static u32 color_table[] = {0x000000, 0xAA0000, 0x00AA00, 0xAA5500, 0x0000AA, 0xAA00AA, 0x00AAAA, 0xAAAAAA,
                            0x555555, 0xFF5555, 0x55FF55, 0xFFFF55, 0x5555FF, 0xFF55FF, 0x55FFFF, 0xFFFFFF};

void set_attr()
{
    if (current_num == 0)
    {
        current_esc_state.foreground = color_table[7];
        current_esc_state.background = color_table[0];
    }
    else if (current_num >= 30 && current_num <= 39)
    {
        if (current_num < 38)
        {
            current_esc_state.foreground = color_table[current_num - 30];
        }
        else if (current_num == 39)
        {
            current_esc_state.foreground = color_table[7];
        }
        else
        {
            /// TODO: when num == 38 next num == 2
        }
    }
    else if (current_num >= 40 && current_num <= 49)
    {
        if (current_num < 48)
        {
            current_esc_state.background = color_table[current_num - 40];
        }
        else if (current_num == 49)
        {
            current_esc_state.background = color_table[0];
        }
        else
        {
            /// TODO: when num == 38 next num == 2
        }
    }
    else if (current_num >= 90 && current_num <= 97)
    {
        current_esc_state.foreground = color_table[current_num - 90 + 8];
    }
    else if (current_num >= 100 && current_num <= 107)
    {
        current_esc_state.background = color_table[current_num - 100 + 8];
    }
}

bool set_state(char ch)
{
    if (ch == '\e')
    {
        if (esc_state == 0)
            esc_state = 1;
        else
            esc_state = 0;
        return true;
    }
    else if (ch == '[')
    {
        if (esc_state == 1)
        {
            current_num = 0;
            esc_state = 2;
            return true;
        }
        else
            esc_state = 0;
    }
    else if (esc_state == 2 || esc_state == 3)
    {
        if (ch >= '0' && ch <= '9')
        {
            esc_state = 3;
            current_num = current_num * 10 + ch - '0';
        }
        else if (ch == ';' || ch == ':')
        {
            set_attr();
            esc_state = 2;
            current_num = 0;
        }
        else if (ch == 'm')
        {
            set_attr();
            current_num = 0;
            esc_state = 0;
            global_state = current_esc_state;
        }
        return true;
    }
    else
    {
        esc_state = 0;
    }
    return false;
}

void putchar(cursor_t &cur, char ch)
{

    if (ch != '\t' && ch != '\n')
    {
        void *font_data = cur_font->get_unicode(ch);
        if (set_state(ch))
            return;

        u32 fg = global_state.foreground;
        u32 bg = global_state.background;

        u32 *v_start = get_video_line_start(cur) + cur.px * (u64)font_width;

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
    {
        u64 top_bytes = window_height * line_bytes;
        u64 bottom_bytes = line_bytes * height - top_bytes;
        util::memcopy(vraw, (byte *)video_addr + top_bytes, bottom_bytes);
        util::memcopy(vraw + bottom_bytes, (void *)video_addr, top_bytes);
    }
    else
    {
        u32 bytes = (right - left) * sizeof(u32);
        u32 l = left * sizeof(u32);
        i64 off = (top + window_height) % height;

        for (u32 y = top, i = off; y < bottom; y++, i++)
        {
            if (unlikely(i >= height))
            {
                i = 0;
            }
            util::memcopy((char *)vraw + y * line_bytes + l, (char *)video_addr + i * line_bytes + l, bytes);
        }
    }
    dirty_rectangle.clean();
}

void draw_pixel(u64 px, u64 py, u64 color) { *((u32 *)video_addr + px + py * width) = color; }

} // namespace arch::device::vga::graphics