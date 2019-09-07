#include "kernel/arch/video/vga/vga.hpp"
#include "kernel/arch/video/vga/output_graphics.hpp"
#include "kernel/arch/video/vga/output_text.hpp"
#include "kernel/mm/memory.hpp"
#include <stdarg.h>
#include <type_traits>
namespace arch::device::vga
{
output *current_output;

void *frame_buffer;

Aligned(8) char reserved_space[sizeof(output_text) > sizeof(output_graphics) ? sizeof(output_text)
                                                                             : sizeof(output_graphics)];

void init(const kernel_start_args *args)
{
    frame_buffer = (void *)args->fb_addr;
    bool is_text_mode = args->fb_type == 2;
    u32 frame_size = args->fb_width * args->fb_height * args->fb_bbp / 8;

    void *back_buffer = nullptr;
    // Find a frame size memory as backbuffer
    kernel_memory_map_item *mm_item = memory::kernel_phyaddr_to_virtaddr(args->get_mmap_ptr()) + args->mmap_count - 1;
    const u64 align = 0x1000;
    for (i64 i = args->mmap_count - 1; i >= 0; i--, mm_item--)
    {
        if (mm_item->map_type == map_type_t::available)
        {
            u64 start = ((u64)mm_item->addr + align - 1) & ~(align - 1);
            u64 end = ((u64)mm_item->addr + mm_item->len - 1) & ~(align - 1);
            if (start > end)
                break;
            if (end - start >= frame_size)
            {
                back_buffer = memory::kernel_phyaddr_to_virtaddr((void *)(end - frame_size));
                break;
            }
        }
    }
    if (unlikely(back_buffer == nullptr))
        return;

    if (likely(!is_text_mode))
    {
        current_output = new (&reserved_space)
            output_graphics(args->fb_width, args->fb_height, back_buffer, args->fb_pitch, args->fb_bbp);
        current_output->init();
        current_output->cls();
        print(Foreground<Color::LightGreen>(), "VGA graphics mode. ", args->fb_width, "X", args->fb_height, ". bit ",
              args->fb_bbp, ". frame size ", frame_size >> 10, "kb.\n");
    }
    else
    {
        current_output = new (&reserved_space)
            output_text(args->fb_width, args->fb_height, back_buffer, args->fb_pitch, args->fb_bbp);
        current_output->init();
        current_output->cls();
        print(Foreground<Color::LightGreen>(), "VGA text mode. ", args->fb_width, "X", args->fb_height, ". bit ",
              args->fb_bbp, ". frame size ", frame_size >> 10, "kb.\n");
    }

    test();
    trace::debug("Video address ", (void *)frame_buffer);
}

void test()
{
    print(Background<Color::LightGray>(), Foreground<Color::Black>(), "VGA Test Begin\n");
    print(Background<Color::ColorValue>(0x9f9f9f), Foreground<Color::Black>(), "NaOS\n");
    print(Foreground<Color::Black>(), "Black ");
    print(Foreground<Color::Blue>(), "Blue ");
    print(Foreground<Color::Green>(), "Green ");
    print(Foreground<Color::Cyan>(), "Cyan ");
    print(Foreground<Color::Red>(), "Red ");
    print(Foreground<Color::Magenta>(), "Magenta ");
    print(Foreground<Color::Brown>(), "Brown ");
    print(Foreground<Color::LightGray>(), "LightGray ");
    print(Foreground<Color::DarkGray>(), "DarkGray ");
    print(Foreground<Color::LightBlue>(), "LightBlue ");
    print(Foreground<Color::LightGreen>(), "LightGreen ");
    print(Foreground<Color::LightCyan>(), "LightCyan ");
    print(Foreground<Color::LightRed>(), "LightRed ");
    print(Foreground<Color::Pink>(), "Pink ");
    print(Foreground<Color::Yellow>(), "Yellow ");
    print(Foreground<Color::White>(), "White \n");
    print(Background<Color::LightGray>(), Foreground<Color::Black>(), "VGA Test End\n");
}

void *get_video_addr() { return frame_buffer; }

void set_video_addr(void *addr) { frame_buffer = addr; }

void flush() { current_output->flush(frame_buffer); }

void putstring(const char *str, font_attribute &attribute)
{
    while (*str != '\0')
    {
        current_output->putchar(*str++, attribute);
    }
    flush();
}

} // namespace arch::device::vga
