#pragma once
#include "common.hpp"
#include "freelibcxx/allocator.hpp"
#include "freelibcxx/circular_buffer.hpp"
#include "freelibcxx/tuple.hpp"
#include "freelibcxx/unicode.hpp"
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

    // void fill_placeholder(u32 row, u32 col, u32 fg, u32 bg);

    freelibcxx::tuple<u32, u32> rows_cols()
    {
        u32 rows = fb_.height / font_height_;
        u32 cols = fb_.width / font_width_;
        return freelibcxx::make_tuple(rows, cols);
    }

    framebuffer_t fb() const { return fb_; }

    u32 frame_bytes() const { return fb_.width * fb_.height * fb_.bbp / 8; }

  private:
    framebuffer_t fb_;
    font::pixel_font *font_;
    u32 font_height_;
    u32 font_width_;
};
} // namespace fb