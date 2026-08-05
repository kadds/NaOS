#pragma once
#include "freelibcxx/allocator.hpp"
#include "freelibcxx/circular_buffer.hpp"
#include "freelibcxx/tuple.hpp"
#include "freelibcxx/unicode.hpp"
#include "kernel/common.hpp"
#include "kernel/common/font/font.hpp"

namespace fb
{

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
