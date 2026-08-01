#pragma once
#include "common.hpp"
#include "freelibcxx/allocator.hpp"
#include "freelibcxx/circular_buffer.hpp"
#include "freelibcxx/tuple.hpp"
#include "freelibcxx/unicode.hpp"
#include "kernel/common/font/font.hpp"

namespace fb
{

namespace ioctl
{
inline constexpr u64 get_vscreeninfo = 0x4600;
inline constexpr u64 put_vscreeninfo = 0x4601;
inline constexpr u64 get_fscreeninfo = 0x4602;
inline constexpr u64 get_con2fbmap = 0x460F;
inline constexpr u64 put_con2fbmap = 0x4610;
inline constexpr u64 blank = 0x4611;

// NaOS-specific controls for virtual terminals exposed as framebuffer devices.
inline constexpr u64 get_active_terminal = 0x4620;
inline constexpr u64 set_active_terminal = 0x4621;

struct bitfield
{
    u32 offset;
    u32 length;
    u32 msb_right;
};

struct var_screeninfo
{
    u32 xres;
    u32 yres;
    u32 xres_virtual;
    u32 yres_virtual;
    u32 xoffset;
    u32 yoffset;
    u32 bits_per_pixel;
    u32 grayscale;
    bitfield red;
    bitfield green;
    bitfield blue;
    bitfield transp;
    u32 nonstd;
    u32 activate;
    u32 height;
    u32 width;
    u32 accel_flags;
    u32 pixclock;
    u32 left_margin;
    u32 right_margin;
    u32 upper_margin;
    u32 lower_margin;
    u32 hsync_len;
    u32 vsync_len;
    u32 sync;
    u32 vmode;
    u32 rotate;
    u32 colorspace;
    u32 reserved[4];
};

struct fix_screeninfo
{
    char id[16];
    u64 smem_start;
    u32 smem_len;
    u32 type;
    u32 type_aux;
    u32 visual;
    u16 xpanstep;
    u16 ypanstep;
    u16 ywrapstep;
    u32 line_length;
    u64 mmio_start;
    u32 mmio_len;
    u32 accel;
    u16 capabilities;
    u16 reserved[2];
};

struct con2fbmap
{
    u32 console;
    u32 framebuffer;
};

static_assert(sizeof(var_screeninfo) == 160);
static_assert(sizeof(fix_screeninfo) == 80);
static_assert(sizeof(con2fbmap) == 8);
} // namespace ioctl

struct cell_t
{
    char32_t codepoint;
    u32 fg;
    u32 bg;
    u32 flags;
};

struct framebuffer_t
{
    void *ptr;
    phy_addr_t physical_addr;
    u32 pitch;
    u32 width;
    u32 height;
    u32 bbp;
};

class framebuffer_backend
{
  public:
    framebuffer_backend(framebuffer_t fb, font::pixel_font *font)
        : fb_(fb)
        , font_(font)
    {
        auto [width, height] = font_->get_size();
        font_width_ = width;
        font_height_ = height;
    }

    void commit(u32 row, u32 col, cell_t cell);

    void commit_placeholder(u32 row, u32 col, bool show);

    freelibcxx::tuple<u32, u32> rows_cols()
    {
        u32 rows = fb_.height / font_height_;
        u32 cols = fb_.width / font_width_;
        return freelibcxx::make_tuple(rows, cols);
    }

    framebuffer_t &fb() { return fb_; }
    const framebuffer_t &fb() const { return fb_; }

    u32 frame_bytes() const { return fb_.pitch * fb_.height; }

    u64 read_bytes(i64 &offset, byte *data, u64 max_size) const;
    u64 write_bytes(i64 &offset, const byte *data, u64 size) const;

  private:
    framebuffer_t fb_;
    font::pixel_font *font_;
    u32 font_height_;
    u32 font_width_;
};
} // namespace fb
