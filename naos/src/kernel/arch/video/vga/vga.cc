#include "kernel/arch/video/vga/vga.hpp"
#include "kernel/arch/video/vga/output_graphics.hpp"
#include "kernel/kernel.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/timer.hpp"
#include "kernel/trace.hpp"
#include "kernel/ucontext.hpp"
#include <stdarg.h>
#include <type_traits>

namespace arch::device::vga
{
void test();

u64 frame_size;
byte *vram_addr;
byte *backbuffer_addr;
bool is_auto_flush = false;
cursor_t cursor;

void init()
{
    auto args = kernel_args;
    using namespace trace;
    bool is_graphics_mode = args->fb_type == 1;
    backbuffer_addr = nullptr;

    if (!is_graphics_mode || args->fb_addr == 0)
    {
        vram_addr = nullptr;
        trace::warning("Can't find a video device!");
        return;
    }

    vram_addr = (byte *)args->fb_addr;
    frame_size = args->fb_width * args->fb_height * args->fb_bbp / 8;

    /// XXX: Find memory as backbuffer
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
                backbuffer_addr = memory::kernel_phyaddr_to_virtaddr((byte *)(end - frame_size));
                break;
            }
        }
    }
    if (unlikely(backbuffer_addr == nullptr))
    {
        trace::panic("Video backing buffer size is insufficient.");
    }

    graphics::init(args->fb_width, args->fb_height, (byte *)backbuffer_addr, args->fb_pitch, args->fb_bbp);
    graphics::cls(cursor);
    auto fb_width = args->fb_width;
    auto fb_height = args->fb_height;
    auto fb_bbp = args->fb_bbp;
    print<PrintAttribute<CFG::LightGreen>>("VGA graphics mode. ", fb_width, "X", fb_height, ". bit ", fb_bbp,
                                           ". frame size ", frame_size >> 10, "kb.\n");

    test();

    /// print video card memory info
    trace::debug("Vram address ", (void *)vram_addr);
    trace::debug("Video backbuffer ", (void *)backbuffer_addr, "-", (void *)((char *)backbuffer_addr + frame_size));
}

void test()
{
    using namespace trace;
    print<PrintAttribute<CBK::LightGray, CFG::Black>>("VGA Test Begin");
    print<PrintAttribute<TextAttribute::Reset>>();
    print<PrintAttribute<CFG::Black>>("\n Black ");
    print<PrintAttribute<CFG::Blue>>(" Blue ");
    print<PrintAttribute<CFG::Green>>(" Green ");
    print<PrintAttribute<CFG::Cyan>>(" Cyan ");
    print<PrintAttribute<CFG::Red>>(" Red ");
    print<PrintAttribute<CFG::Magenta>>(" Magenta ");
    print<PrintAttribute<CFG::Brown>>(" Brown ");
    print<PrintAttribute<CFG::LightGray>>(" LightGray ");
    print<PrintAttribute<CFG::DarkGray>>(" DarkGray ");
    print<PrintAttribute<CFG::LightBlue>>(" LightBlue ");
    print<PrintAttribute<CFG::LightGreen>>(" LightGreen ");
    print<PrintAttribute<CFG::LightCyan>>(" LightCyan ");
    print<PrintAttribute<CFG::LightRed>>(" LightRed ");
    print<PrintAttribute<CFG::Pink>>(" Pink ");
    print<PrintAttribute<CFG::Yellow>>(" Yellow ");
    print<PrintAttribute<CFG::White>>(" White \n");
    print<PrintAttribute<CBK::LightGray, CFG::Black>>("VGA Test End\n");
    print<PrintAttribute<TextAttribute::Reset>>();
}

byte *get_vram() { return vram_addr; }

void set_vram(byte *addr)
{
    vram_addr = addr;
    flush();
}

byte *get_backbuffer() { return backbuffer_addr; }

void set_backbuffer(byte *addr)
{
    /// TODO: release old buffer
    backbuffer_addr = addr;
    graphics::set_buffer(addr);
}

void flush()
{
    if (likely(vram_addr != nullptr))
    {
        graphics::flush(vram_addr);
    }
}

void flush_kbuffer()
{
    if (likely(vram_addr != nullptr))
    {
        uctx::UnInterruptableContext icu;
        u64 size;
        byte *buffer = trace::get_kernel_log_buffer().read_buffer(&size);

        while (size != 0)
        {
            putstring((const char *)buffer, size);
            buffer = trace::get_kernel_log_buffer().read_buffer(&size);
        }

        flush();
    }
}

void flush_timer(u64 dt, u64 ud)
{
    if (likely(vram_addr != nullptr))
    {
        flush_kbuffer();
        timer::add_watcher(1000000 / 60, flush_timer, 0);
    }
}

void auto_flush()
{
    if (likely(vram_addr != nullptr))
    {

        is_auto_flush = true;
        // 60HZ
        timer::add_watcher(1000000 / 60, flush_timer, 0);
    }
}

u64 putstring(const char *str, u64 max_len)
{
    if (unlikely(vram_addr == nullptr))
    {
        u64 i = 0;
        while (*str != '\0' && i < max_len)
            i++;
        return i;
    }

    if (max_len == 0)
        max_len = (u64)(-1);
    u64 i = 0;
    while (*str != '\0' && i < max_len)
    {
        graphics::putchar(cursor, *str++);
        i++;
    }

    if (unlikely(!is_auto_flush))
        flush();
    return i;
}

void tag_memory()
{
    if (likely(backbuffer_addr != nullptr))
    {
        memory::tag_zone_buddy_memory(memory::kernel_virtaddr_to_phyaddr((byte *)backbuffer_addr),
                                      (memory::kernel_virtaddr_to_phyaddr((byte *)backbuffer_addr + frame_size)));
    }
}

} // namespace arch::device::vga
