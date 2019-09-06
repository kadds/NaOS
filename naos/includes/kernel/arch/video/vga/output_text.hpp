#pragma once
#include "vga.hpp"
namespace arch::device::vga
{
class output_text : public output
{
  private:
    void move_pen(i32 x, i32 y, u32 newline_alignment);
    void scroll(i32 n);

  public:
    using output::output;
    virtual void init() override;
    virtual void cls() override;
    virtual void putchar(char ch, font_attribute &attribute) override;
};
} // namespace arch::device::vga