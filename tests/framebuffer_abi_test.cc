#include <cstddef>
#include <cstdint>

#include "catch2_compat.hpp"

#include <linux/fb.h>
#include <naos/framebuffer.h>

static_assert(FBIOGET_VSCREENINFO == 0x4600);
static_assert(FBIOGET_FSCREENINFO == 0x4602);
static_assert(FBIOGETCMAP == 0x4604);
static_assert(FBIOPUTCMAP == 0x4605);
static_assert(FB_TYPE_PACKED_PIXELS == 0);
static_assert(FB_VISUAL_TRUECOLOR == 2);
static_assert(FB_ACCEL_NONE == 0);
static_assert(sizeof(fb_bitfield) == 12);
static_assert(offsetof(fb_fix_screeninfo, line_length) == 48);
static_assert(offsetof(fb_var_screeninfo, xres) == 0);
static_assert(offsetof(fb_var_screeninfo, bits_per_pixel) == 24);
static_assert(offsetof(fb_var_screeninfo, red) == 32);
static_assert(offsetof(fb_var_screeninfo, transp) == 68);
static_assert(sizeof(naos_fb_fixed_info) == sizeof(fb_fix_screeninfo));
static_assert(sizeof(naos_fb_variable_info) == sizeof(fb_var_screeninfo));
static_assert(offsetof(naos_fb_fixed_info, line_length) == offsetof(fb_fix_screeninfo, line_length));
static_assert(offsetof(naos_fb_variable_info, xres) == offsetof(fb_var_screeninfo, xres));

TEST_CASE("framebuffer ABI layout", "[framebuffer][abi]") {}
