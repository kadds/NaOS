#pragma once
#include "common.hpp"
#include "kernel/util/formatter.hpp"
#include <type_traits>
namespace arch::device::vga
{

namespace Color
{
struct Black
{
    static const u32 color = 0x000000;
};
struct Blue
{
    static const u32 color = 0x0000AA;
};

struct Green
{
    static const u32 color = 0x00AA00;
};

struct Cyan
{
    static const u32 color = 0x00AAAA;
};
struct Red
{
    static const u32 color = 0xAA0000;
};
struct Magenta
{
    static const u32 color = 0xAA00AA;
};
struct Brown
{
    static const u32 color = 0xAA5500;
};
struct LightGray
{
    static const u32 color = 0xAAAAAA;
};
struct DarkGray
{
    static const u32 color = 0x555555;
};
struct LightBlue
{
    static const u32 color = 0x5555FF;
};
struct LightGreen
{
    static const u32 color = 0x55FF55;
};
struct LightCyan
{
    static const u32 color = 0x55FFFF;
};
struct LightRed
{
    static const u32 color = 0xFF5555;
};
struct Pink
{
    static const u32 color = 0xFF55FF;
};
struct Yellow
{
    static const u32 color = 0xFFFF55;
};
struct White
{
    static const u32 color = 0xFFFFFF;
};
struct ColorValue
{
    u32 color;
    ColorValue(u32 color)
        : color(color){};
};
} // namespace Color

template <typename Color> struct Background
{
    Color value;
    template <typename... Args>
    Background(const Args &... args)
        : value(args...)
    {
    }
};
template <typename Color> struct Foreground
{
    Color value;
    template <typename... Args>
    Foreground(const Args &... args)
        : value(args...)
    {
    }
};

struct AlignmentValue
{
    u32 nums;
    AlignmentValue(u32 nums)
        : nums(nums){};
};

struct font_attribute
{
    u64 color;
    u64 attr;
    font_attribute()
        : color(0)
        , attr(0)
    {
    }
    void set_foreground(u32 color) { this->color = (this->color & 0xFFFFFFFFFF000000) | (color & 0xFFFFFFUL); };
    void set_background(u32 color) { this->color = (this->color & 0x00000000FFFFFFFF) | ((color & 0xFFFFFFUL) << 32); };
    void set_newline_alignment(u32 nums) { this->attr = (this->attr & 0xFFFFFFFF00000000) | nums; }
    u32 get_newline_alignment() { return this->attr & 0xFFFFFFFF; }
    u32 get_foreground() { return this->color & 0xFFFFFF; }
    u32 get_background() { return (this->color >> 32) & 0xFFFFFF; }
};
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
    virtual void putchar(char ch, font_attribute &attribute) = 0;
    void *get_addr() { return (void *)video_addr; }
    void set_addr(void *video) { video_addr = video; }
    virtual void flush(void *vraw) = 0;

    void set_pen(u32 x, u32 y)
    {
        px = x;
        py = y;
    };
};

struct rectangle
{
    u32 left;
    u32 top;
    u32 right;
    u32 bottom;
    rectangle(){};
    rectangle(u32 left, u32 top, u32 right, u32 bottom)
        : left(left)
        , top(top)
        , right(right)
        , bottom(bottom){};
    rectangle &operator+=(const rectangle &rc)
    {
        if (left == right || top == bottom)
            *this = rc;

        if (left > rc.left)
            left = rc.left;
        if (top > rc.top)
            top = rc.top;

        if (right < rc.right)
            right = rc.right;
        if (bottom < rc.bottom)
            bottom = rc.bottom;

        return *this;
    }
    void clean()
    {
        left = 0;
        top = 0;
        right = 0;
        bottom = 0;
    }
};

void init(const kernel_start_args *args);
void *get_video_addr();
void set_video_addr(void *addr);

void test();
void putstring(const char *str, font_attribute &attribute);
void flush();

void set_foreground(u32 color);
void set_background(u32 color);

template <typename T> struct remove_extent
{
    using type = T;
};

template <typename T> struct remove_extent<T[]>
{
    using type = T *;
};

template <typename T, std::size_t N> struct remove_extent<T[N]>
{
    using type = T *;
};

template <typename T> struct remove_extent<const T[]>
{
    using type = const T *;
};

template <typename T, std::size_t N> struct remove_extent<const T[N]>
{
    using type = const T *;
};

template <typename TColor> void set_font_attr(font_attribute &attribute, const Foreground<TColor> &cv)
{
    attribute.set_foreground(cv.value.color);
}

template <typename TColor> void set_font_attr(font_attribute &attribute, const Background<TColor> &cv)
{
    attribute.set_background(cv.value.color);
}

inline void set_text_alignment(font_attribute &attribute, const AlignmentValue &av)
{
    attribute.set_newline_alignment(av.nums);
}

template <typename Head> void print_fmt_text(font_attribute &attribute, const Head &head)
{
    char buf[128];
    using RealType = typename remove_extent<std::decay_t<decltype(head)>>::type;
    util::formatter::format<RealType> fmt;
    putstring(fmt(head, buf, 128), attribute);
}
struct dispatcher
{
    template <typename Head, std::enable_if_t<!std::is_same_v<Head, AlignmentValue>> * = nullptr>
    auto dispatch(font_attribute &attribute, const Head &head)
    {
        return print_fmt_text(attribute, head);
    }
    template <template <typename S> typename Head, typename S>
    auto dispatch(font_attribute &attribute, const Head<S> &head)
    {
        return set_font_attr(attribute, head);
    }

    template <typename Head, std::enable_if_t<std::is_same_v<Head, AlignmentValue>> * = nullptr>
    auto dispatch(font_attribute &attribute, const Head &head)
    {
        return set_text_alignment(attribute, head);
    }
};

template <typename Head> void print_fmt(font_attribute &attribute, const Head &head)
{
    dispatcher dispatch;
    dispatch.dispatch(attribute, head);
}

template <typename Head, typename... Args>
void print_fmt(font_attribute &attribute, const Head &head, const Args &... args)
{
    dispatcher dispatch;
    dispatch.dispatch(attribute, head);
    print_fmt(attribute, args...);
}

template <typename... Args> void print(const Args &... args)
{
    font_attribute attribute;
    attribute.set_foreground(Color::LightGray::color);
    attribute.set_background(Color::Black::color);

    print_fmt(attribute, args...);
}

} // namespace arch::device::vga
