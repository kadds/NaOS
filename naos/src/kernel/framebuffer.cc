#include "kernel/framebuffer.hpp"
#include "common.hpp"
#include "freelibcxx/string.hpp"
#include "freelibcxx/unicode.hpp"
#include "kernel/common/font/font.hpp"
#include "kernel/common/font/font_16X8.hpp"
#include "kernel/trace.hpp"

namespace fb
{
void framebuffer_backend::commit(u32 row, u32 col, cell_t cell)
{
    u32 *dst = reinterpret_cast<u32 *>(fb_.ptr);
    auto glyph = font_->get_glyph(cell.codepoint);

    dst += (fb_.pitch / sizeof(u32)) * (row * font_height_) + col * font_width_;

    if (unlikely((row + 1) * font_height_ > fb_.height) || (col + 1) * font_width_ > fb_.width)
    {
        trace::panic("row check fail");
        return;
    }

    if (likely(cell.fg == cell.bg))
    {
        for (u32 i = 0; i < font_height_; i++)
        {
            for (u32 j = 0; j < font_width_; j++)
            {
                *(dst + j) = cell.fg;
            }
            dst += fb_.pitch / sizeof(u32);
        }
    }
    else
    {
        for (u32 i = 0; i < font_height_; i++)
        {
            for (u32 j = 0; j < font_width_; j++)
            {
                if (glyph.hit(j, i))
                {
                    *(dst + j) = cell.fg;
                }
                else
                {
                    *(dst + j) = cell.bg;
                }
            }
            dst += fb_.pitch / sizeof(u32);
        }
    }
}

void framebuffer_backend::commit_placeholder(u32 row, u32 col, bool show)
{
    u32 *dst = reinterpret_cast<u32 *>(fb_.ptr);

    dst += (fb_.pitch / sizeof(u32)) * (row * font_height_) + col * font_width_;

    if (unlikely((row + 1) * font_height_ > fb_.height) || (col + 1) * font_width_ > fb_.width)
    {
        trace::panic("row check fail");
        return;
    }

    for (u32 i = 0; i < font_height_; i++)
    {
        for (u32 j = 0; j < font_width_; j++)
        {
            *(dst + j) = show ? 0xA0A0A0 : 0x0;
        }
        dst += fb_.pitch / sizeof(u32);
    }
}

u64 framebuffer_backend::read_bytes(i64 &offset, byte *data, u64 max_size) const
{
    if (offset < 0 || static_cast<u64>(offset) >= frame_bytes() || max_size == 0)
    {
        return 0;
    }

    const u64 available = frame_bytes() - static_cast<u64>(offset);
    const u64 count = freelibcxx::min(max_size, available);
    memcpy(data, reinterpret_cast<const byte *>(fb_.ptr) + offset, count);
    offset += count;
    return count;
}

u64 framebuffer_backend::write_bytes(i64 &offset, const byte *data, u64 size) const
{
    if (offset < 0 || static_cast<u64>(offset) >= frame_bytes() || size == 0)
    {
        return 0;
    }

    const u64 available = frame_bytes() - static_cast<u64>(offset);
    const u64 count = freelibcxx::min(size, available);
    memcpy(reinterpret_cast<byte *>(fb_.ptr) + offset, data, count);
    offset += count;
    return count;
}

} // namespace fb
