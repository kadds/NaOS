#pragma once
#include "font.hpp"

namespace font
{
class font_16X8 : public font
{
  private:
    static u8 vga_font[];
    void *get_font(u32 index) { return (char *)vga_font + index * 16; }

  public:
    virtual void init() override{};
    virtual void *get_unicode(u32 code) override
    {
        if (code > 255)
            return get_font(254);
        return get_font(code);
    };
    virtual u8 hit(void *char_data, u32 x, u32 y) override
    {
        u8 *d = (u8 *)char_data + y;
        return *d & (1 << (8 - x));
    }
    virtual void get_size(u32 &width, u32 &height) override
    {
        width = 8;
        height = 16;
    };
};
} // namespace font
