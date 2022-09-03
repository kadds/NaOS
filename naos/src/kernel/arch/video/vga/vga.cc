#include "kernel/arch/video/vga/vga.hpp"
#include "common.hpp"
#include "kernel/arch/mm.hpp"
#include "kernel/arch/paging.hpp"
#include "kernel/common/font/font_16X8.hpp"
#include "kernel/framebuffer.hpp"
#include "kernel/kernel.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/mm/vm.hpp"
#include "kernel/terminal.hpp"
#include "kernel/trace.hpp"
#include "kernel/ucontext.hpp"

namespace arch::device::vga
{
void test();

fb::framebuffer_backend *early_backend;

int rows;
int cols;
font::font_16X8 font;
term::minimal_terminal *early_init(fb::framebuffer_t fb)
{
    fb.ptr = memory::pa2va(phy_addr_t::from(fb.ptr));
    auto early_terminal = new (memory::pa2va(phy_addr_t::from(0x21200))) term::minimal_terminal();
    early_backend = new (memory::pa2va(phy_addr_t::from(0x21000))) fb::framebuffer_backend(fb, &font);
    early_terminal->attach_backend(early_backend);
    arch::paging::temp_update_uncached(fb.ptr, (fb.height * fb.pitch + memory::page_size - 1) / memory::page_size);
    return early_terminal;
}

void init()
{
    using namespace trace;
    u32 bytes = early_backend->frame_bytes();
    auto fb = early_backend->fb();

    auto backbuffer = reinterpret_cast<byte *>(memory::MemoryAllocatorV->allocate(bytes, 8));

    /// XXX: Find memory as backbuffer
    if (unlikely(backbuffer == nullptr))
    {
        trace::panic("Video backing buffer size is insufficient.");
    }

    print<PrintAttribute<CFG::LightGreen>>("VGA graphics mode. ", fb.width, "X", fb.height, ". ", fb.bbp, "bit",
                                           ". frame bytes ", bytes >> 10, "KiB.\n");

    test();

    phy_addr_t video_start = memory::align_down(phy_addr_t::from(fb.ptr), paging::frame_size::size_2mb);

    auto pages = (memory::kernel_vga_top_address - memory::kernel_vga_bottom_address) / memory::page_size;

    auto &paging = memory::kernel_vm_info->paging();
    paging.map_to(reinterpret_cast<void *>(memory::kernel_vga_bottom_address), pages, video_start,
                  paging::flags::writable | paging::flags::write_through | paging::flags::cache_disable, 0);
    paging.reload();
    fb.ptr = video_start();

    /// print video card memory info
    trace::debug("Vram address ", trace::hex(video_start()), " map to ", trace::hex(memory::kernel_vga_bottom_address));
    trace::debug("Video backbuffer(VM) ", trace::hex(backbuffer), "-", trace::hex((backbuffer + bytes)));
}

void test()
{
    using namespace trace;
    print<PA<CBK::LightGray, CFG::Black>>("VGA Test Begin");
    print<PA<TextAttribute::Reset>>();
    print<PA<CFG::Black>>("\n Black ");
    print<PA<CFG::Blue>>(" Blue ");
    print<PA<CFG::Green>>(" Green ");
    print<PA<CFG::Cyan>>(" Cyan ");
    print<PA<CFG::Red>>(" Red ");
    print<PA<CFG::Magenta>>(" Magenta ");
    print<PA<CFG::Brown>>(" Brown ");
    print<PA<CFG::LightGray>>(" LightGray ");
    print<PA<CFG::DarkGray>>(" DarkGray ");
    print<PA<CFG::LightBlue>>(" LightBlue ");
    print<PA<CFG::LightGreen>>(" LightGreen ");
    print<PA<CFG::LightCyan>>(" LightCyan ");
    print<PA<CFG::LightRed>>(" LightRed ");
    print<PA<CFG::Pink>>(" Pink ");
    print<PA<CFG::Yellow>>(" Yellow ");
    print<PA<CFG::White>>(" White \n");
    print<PA<CBK::LightGray, CFG::Black>>("VGA Test End\n");
    print<PA<TextAttribute::Reset>>();
}

// void flush_timer(u64 dt, u64 ud)
// {
//     flush_kbuffer();
//     timer::add_watcher(1000000 / 60, flush_timer, 0);
// }

// void auto_flush()
// {
//     // 60HZ
//     timer::add_watcher(1000000 / 60, flush_timer, 0);
// }

} // namespace arch::device::vga
