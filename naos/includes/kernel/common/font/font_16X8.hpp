#pragma once
#include "font.hpp"
#include "freelibcxx/tuple.hpp"

namespace font
{
class font_16X8 : public pixel_font
{
  private:
    static u8 vga_font[];
    glyph get_font(u32 index)
    {
        glyph glyph;
        auto p = reinterpret_cast<byte *>(vga_font);
        glyph.bytes = freelibcxx::span(p + index * 16, 16);
        glyph.line_bytes = 1;
        return glyph;
    }

  public:
    virtual glyph get_glyph(char32_t code) override
    {
        if (code > 255)
        {
            return get_font(254);
        }
        return get_font(code);
    };
    virtual freelibcxx::tuple<u32, u32> get_size() override { return freelibcxx::make_tuple(8, 16); };
};
} // namespace font
