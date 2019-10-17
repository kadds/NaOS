#pragma once
#include "kernel/mm/memory.hpp"
#include "kernel/util/bit_set.hpp"
#include "vga.hpp"

namespace arch::device::vga
{
class output_text : public output
{
  private:
    void move_pen(i32 x, i32 y);
    void scroll(i32 n);

    // Reserve for bitmap
    char reserved_space[32];
    memory::FixMemoryAllocator allocator;

    // Each bit indicates which line has changed
    util::bit_set<u64, 1> bitmap;

    // Shows whether it has been modified since last flush
    bool has_write;

  public:
    output_text(u32 width, u32 height, void *video, u32 bbp, u32 pitch);
    virtual void init() override;
    virtual void cls() override;
    virtual void putchar(char ch, trace::console_attribute &attribute) override;
    virtual void flush(void *vraw) override;
};
} // namespace arch::device::vga