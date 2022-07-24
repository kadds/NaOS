#include "kernel/arch/video/vga/vga.hpp"
#include "kernel/arch/paging.hpp"
#include "kernel/arch/video/vga/output_graphics.hpp"
#include "kernel/kernel.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/mm/zone.hpp"
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
byte *backbuffer_addr = nullptr;
bool is_auto_flush = false;
text_cursor_t cursor;
u64 placeholder_times;

void init()
{
    auto args = kernel_args;
    using namespace trace;
    bool is_graphics_mode = args->fb_type == 1;
    backbuffer_addr = nullptr;
    placeholder_times = 0;
    if (!is_graphics_mode || args->fb_addr == 0)
    {
        vram_addr = nullptr;
        trace::warning("Can't find a video device!");
        return;
    }

    vram_addr = (byte *)args->fb_addr;
    frame_size = args->fb_width * args->fb_height * args->fb_bbp / 8;
    backbuffer_addr = reinterpret_cast<byte *>(memory::MemoryAllocatorV->allocate(frame_size, 8));

    /// XXX: Find memory as backbuffer
    if (unlikely(backbuffer_addr == nullptr))
    {
        trace::panic("Video backing buffer size is insufficient.");
    }

    graphics::init(args->fb_width, args->fb_height, (byte *)backbuffer_addr, args->fb_pitch, args->fb_bbp);
    graphics::cls(cursor);
    auto fb_width = args->fb_width;
    auto fb_height = args->fb_height;
    auto fb_bbp = args->fb_bbp;
    print<PrintAttribute<CFG::LightGreen>>("VGA graphics mode. ", fb_width, "X", fb_height, ". ", fb_bbp, "bit",
                                           ". frame size ", frame_size >> 10, "KiB.\n");

    test();

    phy_addr_t video_start =
        memory::align_down(phy_addr_t::from(device::vga::get_vram()), paging::frame_size::size_2mb);
    paging::map(paging::current(), (void *)memory::kernel_vga_bottom_address, video_start, paging::frame_size::size_2mb,
                (memory::kernel_vga_top_address - memory::kernel_vga_bottom_address) / paging::frame_size::size_2mb,
                paging::flags::writable | paging::flags::write_through | paging::flags::cache_disable, false);
    paging::reload();

    /// print video card memory info
    trace::debug("Vram address ", trace::hex(vram_addr), " map to ", trace::hex(memory::kernel_vga_bottom_address));
    trace::debug("Video backbuffer(VM) ", trace::hex(backbuffer_addr), "-", trace::hex((backbuffer_addr + frame_size)));
    vram_addr = (byte *)memory::kernel_vga_bottom_address;
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
        uctx::UninterruptibleContext icu;
        u64 size = 64;
        byte buffer[65];
        size = trace::get_kernel_log_buffer().read(buffer, size);

        while (size != 0)
        {
            putstring((const char *)buffer, size);
            size = trace::get_kernel_log_buffer().read(buffer, size);
        }

        flush();
    }
}

void flush_timer(u64 dt, u64 ud)
{
    if (likely(vram_addr != nullptr))
    {
        constexpr u64 speed = 64;
        placeholder_times++;
        if ((placeholder_times % speed) == 1)
        {
            auto tk = placeholder_times / speed;
            if (tk % 2)
            {
                graphics::draw_placeholder(cursor, 0x0);
            }
            else
            {
                graphics::draw_placeholder(cursor, 0xBABABA);
            }
        }
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
        while (*str++ != 0 && i < max_len)
            i++;
        return i;
    }

    if (max_len == 0)
        max_len = (u64)(-1);

    u64 i = 0;

    while (*str != 0 && i < max_len)
    {
        if (*str == '\n')
        {
            graphics::draw_placeholder(cursor, 0);
        }
        graphics::putchar(cursor, *str);
        str++;
        i++;
    }
    if (likely(i > 0))
        placeholder_times = 0;

    if (unlikely(!is_auto_flush))
        flush();
    return i;
}

void tag_memory()
{
    if (likely(backbuffer_addr != nullptr))
    {
        memory::global_zones->tag_alloc(memory::va2pa(backbuffer_addr), memory::va2pa(backbuffer_addr + frame_size));
    }
}

} // namespace arch::device::vga
