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

} // namespace fb