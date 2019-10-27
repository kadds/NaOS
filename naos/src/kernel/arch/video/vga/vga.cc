#include "kernel/arch/video/vga/vga.hpp"
#include "kernel/arch/video/vga/output_graphics.hpp"
#include "kernel/arch/video/vga/output_text.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/timer.hpp"
#include "kernel/trace.hpp"
#include "kernel/ucontext.hpp"
#include <stdarg.h>
#include <type_traits>

namespace arch::device::vga
{
output *current_output;
u64 frame_size;
void *frame_buffer;
bool is_auto_flush = false;

Aligned(8) char reserved_space[sizeof(output_text) > sizeof(output_graphics) ? sizeof(output_text)
                                                                             : sizeof(output_graphics)];

void init(const kernel_start_args *args)
{
    using namespace trace;
    frame_buffer = (void *)args->fb_addr;
    bool is_text_mode = args->fb_type == 2;
    frame_size = args->fb_width * args->fb_height * args->fb_bbp / 8;

    void *back_buffer = nullptr;
    // Find a frame size memory as backbuffer
    kernel_memory_map_item *mm_item =
        ((kernel_memory_map_item *)memory::kernel_phyaddr_to_virtaddr(args->mmap) + args->mmap_count - 1);
    const u64 align = 0x1000;
    for (i64 i = args->mmap_count - 1; i >= 0; i--, mm_item--)
    {
        if (mm_item->map_type == map_type_t::available)
        {
            u64 start = ((u64)mm_item->addr + align - 1) & ~(align - 1);
            u64 end = ((u64)mm_item->addr + mm_item->len + align - 1) & ~(align - 1);
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
    {
        // ERROR
        for (;;)
            ;
    }

    if (likely(!is_text_mode))
    {
        current_output = new (&reserved_space)
            output_graphics(args->fb_width, args->fb_height, back_buffer, args->fb_pitch, args->fb_bbp);
        current_output->init();
        current_output->cls();
        print(kernel_console_attribute, Foreground<Color::LightGreen>(), "VGA graphics mode. ", args->fb_width, "X",
              args->fb_height, ". bit ", args->fb_bbp, ". frame size ", frame_size >> 10, "kb.\n");
    }
    else
    {
        current_output = new (&reserved_space)
            output_text(args->fb_width, args->fb_height, back_buffer, args->fb_pitch, args->fb_bbp);
        current_output->init();
        current_output->cls();
        print(kernel_console_attribute, Foreground<Color::LightGreen>(), "VGA text mode. ", args->fb_width, "X",
              args->fb_height, ". bit ", args->fb_bbp, ". frame size ", frame_size >> 10, "kb.\n");
    }

    test();
    trace::debug("Video address ", (void *)frame_buffer);
    trace::debug("Video backbuffer ", (void *)back_buffer, "-", (void *)((char *)back_buffer + frame_size));
}

void test()
{
    using namespace trace;
    print(kernel_console_attribute, Background<Color::LightGray>(), Foreground<Color::Black>(), "VGA Test Begin",
          PrintAttr::Reset());
    print(kernel_console_attribute, Foreground<Color::Black>(), "\n Black ");
    print(kernel_console_attribute, Foreground<Color::Blue>(), " Blue ");
    print(kernel_console_attribute, Foreground<Color::Green>(), " Green ");
    print(kernel_console_attribute, Foreground<Color::Cyan>(), " Cyan ");
    print(kernel_console_attribute, Foreground<Color::Red>(), " Red ");
    print(kernel_console_attribute, Foreground<Color::Magenta>(), " Magenta ");
    print(kernel_console_attribute, Foreground<Color::Brown>(), " Brown ");
    print(kernel_console_attribute, Foreground<Color::LightGray>(), " LightGray ");
    print(kernel_console_attribute, Foreground<Color::DarkGray>(), " DarkGray ");
    print(kernel_console_attribute, Foreground<Color::LightBlue>(), " LightBlue ");
    print(kernel_console_attribute, Foreground<Color::LightGreen>(), " LightGreen ");
    print(kernel_console_attribute, Foreground<Color::LightCyan>(), " LightCyan ");
    print(kernel_console_attribute, Foreground<Color::LightRed>(), " LightRed ");
    print(kernel_console_attribute, Foreground<Color::Pink>(), " Pink ");
    print(kernel_console_attribute, Foreground<Color::Yellow>(), " Yellow ");
    print(kernel_console_attribute, Foreground<Color::White>(), " White \n");
    print(kernel_console_attribute, Background<Color::LightGray>(), Foreground<Color::Black>(), "VGA Test End",
          PrintAttr::Reset());
    print(kernel_console_attribute, '\n');
    print(kernel_console_attribute, Foreground<Color::ColorValue>(0xA0c000), Background<Color::Black>(),
          " NaOS: Nano Operating System (VGA mode) ", PrintAttr::Reset(), '\n');
}

void *get_video_addr() { return frame_buffer; }

void set_video_addr(void *addr)
{
    frame_buffer = addr;
    trace::print(trace::kernel_console_attribute, "VGA frame buffer mapped at ", addr, "\n");
}

void flush() { current_output->flush(frame_buffer); }
void auto_flush(u64 dt, u64 ud)
{
    uctx::UnInterruptableContext icu;
    flush();
    timer::add_watcher(1000000 / 60, auto_flush, 0);

    return;
}

void set_auto_flush()
{
    is_auto_flush = true;
    // 60HZ
    timer::add_watcher(1000000 / 60, auto_flush, 0);
}

u64 putstring(const char *str, u64 max_len, const trace::console_attribute &attribute)
{
    uctx::UnInterruptableContext uic;
    if (max_len == 0)
        max_len = (u64)(-1);
    u64 i = 0;
    while (*str != '\0' && i < max_len)
    {
        if (*str == '\e')
        {
            do
            {
                str++;
                if (*str == '\0')
                    return i;
            } while (*str != 'm');
        }
        current_output->putchar(*str++, attribute);
        i++;
    }

    if (unlikely(!is_auto_flush))
        flush();
    return i;
}
void tag_memory()
{
    memory::tag_zone_buddy_memory(
        memory::kernel_virtaddr_to_phyaddr((char *)current_output->get_addr()),
        (memory::kernel_virtaddr_to_phyaddr((char *)current_output->get_addr() + frame_size)));
}

} // namespace arch::device::vga
