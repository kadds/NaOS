#pragma once
#include "common.hpp"
namespace trace
{
struct console_attribute;
};
namespace arch::device::vga
{
class output
{
  protected:
    u32 width;
    u32 height;
    u32 pitch;
    void *video_addr;

    u32 bbp;

    u32 px;
    u32 py;

  public:
    output(u32 width, u32 height, void *video, u32 bbp, u32 pitch)
        : width(width)
        , height(height)
        , pitch(pitch)
        , video_addr(video)
        , bbp(bbp)
        , px(0)
        , py(0){};
    virtual void init() = 0;
    virtual void cls() = 0;
    virtual void putchar(char ch, trace::console_attribute &attribute) = 0;
    void *get_addr() { return (void *)video_addr; }
    void set_addr(void *video) { video_addr = video; }
    virtual void flush(void *vraw) = 0;

    void set_pen(u32 x, u32 y)
    {
        px = x;
        py = y;
    };
};

extern bool is_auto_flush;

void init(const kernel_start_args *args);
void *get_video_addr();
void set_video_addr(void *addr);

// show color test info
void test();
void set_auto_flush();

void putstring(const char *str, trace::console_attribute &attribute);
void flush();
void tag_memory();

} // namespace arch::device::vga
