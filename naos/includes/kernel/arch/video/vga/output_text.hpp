#pragma once
#include "kernel/mm/memory.hpp"
#include "kernel/util/bit_set.hpp"
#include "vga.hpp"

namespace arch::device::vga
{
class output_text : public output
{
  private:
    void move_pen(i32 x, i32 y, u32 newline_alignment);
    void scroll(i32 n);
    char reserved_space[32];
    memory::FixMemoryAllocator allocator;
    util::bit_set<u64, 1> bitmap;
    bool has_write;

  public:
    output_text(u32 width, u32 height, void *video, u32 bbp, u32 pitch);
    virtual void init() override;
    virtual void cls() override;
    virtual void putchar(char ch, font_attribute &attribute) override;
    virtual void flush(void *vraw) override;
};
} // namespace arch::device::vga