#pragma once
#include "common.hpp"
#include "freelibcxx/span.hpp"
#include "freelibcxx/tuple.hpp"
namespace font
{

struct glyph
{
    freelibcxx::span<byte> bytes;
    u32 line_bytes;
    bool hit(u32 x, u32 y) { return (u8)bytes[y * line_bytes] & (1 << (8 * line_bytes - x - 1)); }
};

class pixel_font
{
  public:
    // Parameter code is encoded with unicode to support multiple language
    virtual glyph get_glyph(char32_t code) = 0;
    virtual freelibcxx::tuple<u32, u32> get_size() = 0;
};

} // namespace font
