#include "kernel/arch/video/vga/vga.hpp"
#include "kernel/arch/mm.hpp"
#include "kernel/arch/paging.hpp"
#include "kernel/common.hpp"
#include "kernel/common/font/font_16X8.hpp"
#include "kernel/framebuffer.hpp"
#include "kernel/kernel.hpp"
#include "kernel/log.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/mm/vm.hpp"
#include "kernel/terminal.hpp"
#include "kernel/ucontext.hpp"

KLOG_MODULE(arch);
namespace arch::device::vga
{
void test();

fb::framebuffer_backend *early_backend;

int rows;
int cols;
font::font_16X8 font;
term::minimal_terminal *early_init(fb::framebuffer_t fb)
{
    if (fb.bbp != 32)
    {
        KLOG_PANIC("Unsupported framebuffer format: only 32bpp is supported");
    }
    fb.ptr = memory::pa2va(fb.physical_addr);
    auto early_terminal = new (memory::pa2va(phy_addr_t::from(0x21200))) term::minimal_terminal();
    early_backend = new (memory::pa2va(phy_addr_t::from(0x21000))) fb::framebuffer_backend(fb, &font);
    early_terminal->attach_backend(early_backend);
    // arch::paging::temp_update_uncached(fb.ptr, (fb.height * fb.pitch + memory::page_size - 1) / memory::page_size);
    return early_terminal;
}

void init()
{
    u32 bytes = early_backend->frame_bytes();
    auto fb = early_backend->fb();

    KLOG_INFO("VGA graphics mode. {}X{}. {}bit. frame bytes {}KiB", fb.width, fb.height, fb.bbp, bytes >> 10);

    test();
}

void test()
{
    KLOG_RAW("\x1b[37;40mVGA Test Begin\x1b[0m\n"
             "\x1b[30m Black \x1b[34m Blue \x1b[32m Green \x1b[36m Cyan \x1b[31m Red "
             "\x1b[35m Magenta \x1b[33m Brown \x1b[37m LightGray \x1b[90m DarkGray\x1b[0m\n"
             "\x1b[94m LightBlue \x1b[92m LightGreen \x1b[96m LightCyan \x1b[91m LightRed "
             "\x1b[95m Pink \x1b[93m Yellow \x1b[97m White\x1b[0m\n"
             "\x1b[37;40mVGA Test End\x1b[0m\n");
}

} // namespace arch::device::vga
