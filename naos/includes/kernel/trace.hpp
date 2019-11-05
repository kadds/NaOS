#pragma once
#include "common.hpp"
#include "kernel/arch/idt.hpp"
#include "kernel/ucontext.hpp"
#include "kernel/util/formatter.hpp"
#include <type_traits>

namespace trace
{

extern bool output_debug;
void init();
void early_init();

//---------------start color definition------------------
namespace Color
{
struct Black
{
    static const u32 color = 0x000000;
    static const u8 index = 0;
};

struct Blue
{
    static const u32 color = 0x0000AA;
    static const u8 index = 4;
};

struct Green
{
    static const u32 color = 0x00AA00;
    static const u8 index = 2;
};

struct Cyan
{
    static const u32 color = 0x00AAAA;
    static const u8 index = 6;
};

struct Red
{
    static const u32 color = 0xAA0000;
    static const u8 index = 1;
};

struct Magenta
{
    static const u32 color = 0xAA00AA;
    static const u8 index = 5;
};

struct Brown
{
    static const u32 color = 0xAA5500;
    static const u8 index = 3;
};

struct LightGray
{
    static const u32 color = 0xAAAAAA;
    static const u8 index = 7;
};

struct DarkGray
{
    static const u32 color = 0x555555;
    static const u8 index = 8;
};

struct LightBlue
{
    static const u32 color = 0x5555FF;
    static const u8 index = 12;
};

struct LightGreen
{
    static const u32 color = 0x55FF55;
    static const u8 index = 10;
};

struct LightCyan
{
    static const u32 color = 0x55FFFF;
    static const u8 index = 14;
};

struct LightRed
{
    static const u32 color = 0xFF5555;
    static const u8 index = 9;
};

struct Pink
{
    static const u32 color = 0xFF55FF;
    static const u8 index = 13;
};

struct Yellow
{
    static const u32 color = 0xFFFF55;
    static const u8 index = 11;
};

struct White
{
    static const u32 color = 0xFFFFFF;
    static const u8 index = 15;
};

struct ColorValue
{
    u32 color;
    ColorValue(u32 color)
        : color(color){};
};
} // namespace Color
//---------------end of color definition-------------

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
template <typename T> struct has_member_index
{
    template <typename U> static auto Check(U) -> typename std::decay<decltype(U::index)>::type;
    static void Check(...);
    using type = decltype(Check(std::declval<T>()));
    enum
    {
        value = !std::is_void_v<type>
    };
};
template <typename T> struct has_member_color
{
    template <typename U> static auto Check(U) -> typename std::decay<decltype(U::color)>::type;
    static void Check(...);
    using type = decltype(Check(std::declval<T>()));
    enum
    {
        value = !std::is_void_v<type>
    };
};
template <typename T> struct has_member_attr
{
    template <typename U> static auto Check(U) -> typename std::decay<decltype(U::attr)>::type;
    static void Check(...);
    using type = decltype(Check(std::declval<T>()));
    enum
    {
        value = !std::is_void_v<type>
    };
};

template <typename T> struct has_member_flag
{
    template <typename U> static auto Check(U) -> typename std::decay<decltype(U::flag)>::type;
    static void Check(...);
    using type = decltype(Check(std::declval<T>()));
    enum
    {
        value = !std::is_void_v<type>
    };
};

namespace PrintAttr
{
enum text_attribute
{
    reset = 0,
    bold = 1,
    italic = 3,
    underline = 4,
    blink = 5,
    reverse = 7,
    hide = 8,
    default_foreground = 39,
    default_background = 49,
    framed = 51,
};
struct Reset
{
    static const u8 flag = 0;
};
struct DefaultForeground
{
    static const u8 flag = 1;
};
struct DefaultBackground
{
    static const u8 flag = 2;
};

struct Bold
{
    static const u8 attr = 1;
};
struct Italic
{
    static const u8 attr = 3;
};
struct UnderLine
{
    static const u8 attr = 4;
};
struct Blink
{
    static const u8 attr = 5;
};
struct Reverse
{
    static const u8 attr = 7;
};
struct Hide
{
    static const u8 attr = 8;
};
struct Framed
{
    static const u8 attr = 51;
};

} // namespace PrintAttr

struct console_attribute
{
    /// color index or color value
    u64 color;
    u64 attr;
    char sgr_string[64];
    char temp_format_str[128];
    console_attribute()
        : color(0)
        , attr(0)
    {
        set_changed();
    }

    console_attribute(u64 c, u64 attr)
        : color(c)
        , attr(attr)
    {
    }
    void set_foreground(u32 color)
    {
        this->color = (this->color & 0xFFFFFFFFFF000000) | (color & 0xFFFFFFUL);
        set_changed();
    };
    void set_background(u32 color)
    {
        this->color = (this->color & 0xFF000000FFFFFFFF) | ((color & 0xFFFFFFUL) << 32);
        set_changed();
    };
    u32 get_foreground() const { return this->color & 0xFFFFFF; }
    u32 get_background() const { return (this->color >> 32) & 0xFFFFFF; }

    bool has_attribute(u8 attr_index) const { return attr & (1ul << attr_index); }
    void set_attribute(u8 attr_index)
    {
        attr |= (1ul << attr_index);
        set_changed();
    }
    void clean_attribute(u8 attr_index)
    {
        attr &= ~(1ul << attr_index);
        set_changed();
    }
    bool has_any_attribute() const { return attr != 0; }

    bool is_back_full_color() const { return color & (1ul << 63); }
    void set_back_full_color()
    {
        color |= (1ul << 63);
        set_changed();
    }
    void clean_back_full_color()
    {
        color &= ~(1ul << 63);
        set_changed();
    }

    bool is_fore_full_color() const { return color & (1ul << 62); }
    void set_fore_full_color()
    {
        color |= (1ul << 62);
        set_changed();
    }
    void clean_fore_full_color()
    {
        color &= ~(1ul << 62);
        set_changed();
    }

    bool has_changed() const { return color & (1ul << 61); }
    void set_changed() { color |= (1ul << 61); }
    void clean_changed() { color &= ~(1ul << 61); }
};
extern const console_attribute default_console_attribute;
extern console_attribute kernel_console_attribute;
/// print escape sequences
void print_SGR(const char *str, console_attribute &current_attribute);

// -------------------- help function------------------
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
// ---------------------end of help function-------------

template <typename TColor, typename std::enable_if_t<has_member_index<TColor>::value> * = nullptr>
void set_font_color_attr(console_attribute &attribute, const Foreground<TColor> &cv)
{
    attribute.set_foreground(TColor::index);
    attribute.clean_attribute(PrintAttr::default_foreground);
    attribute.clean_fore_full_color();
}
template <typename TColor, typename std::enable_if_t<has_member_index<TColor>::value> * = nullptr>
void set_font_color_attr(console_attribute &attribute, const Background<TColor> &cv)
{
    attribute.set_background(TColor::index);
    attribute.clean_attribute(PrintAttr::default_background);
    attribute.clean_back_full_color();
}

template <typename TColor, typename std::enable_if_t<!has_member_index<TColor>::value> * = nullptr>
void set_font_color_attr(console_attribute &attribute, const Foreground<TColor> &cv)
{
    attribute.set_foreground(cv.value.color);
    attribute.clean_attribute(PrintAttr::default_foreground);
    attribute.set_fore_full_color();
}

template <typename TColor, typename std::enable_if_t<!has_member_index<TColor>::value> * = nullptr>
void set_font_color_attr(console_attribute &attribute, const Background<TColor> &cv)
{
    attribute.set_background(cv.value.color);
    attribute.clean_attribute(PrintAttr::default_background);
    attribute.set_back_full_color();
}

/// print string directly with console attribute context
void print_inner(const char *str, console_attribute &current_attribute);

template <typename AttrClass, std::enable_if_t<std::is_same_v<AttrClass, PrintAttr::Reset>> * = nullptr>
void set_font_text_flag(console_attribute &attribute, const AttrClass &cv)
{
    attribute = default_console_attribute;
    attribute.set_attribute(PrintAttr::reset);
    print_inner("\0", attribute);
}

template <typename AttrClass, std::enable_if_t<std::is_same_v<AttrClass, PrintAttr::DefaultBackground>> * = nullptr>
void set_font_text_flag(console_attribute &attribute, const AttrClass &cv)
{
    attribute.set_background(default_console_attribute.get_background());
    attribute.set_attribute(PrintAttr::default_background);
}

template <typename AttrClass, std::enable_if_t<std::is_same_v<AttrClass, PrintAttr::DefaultForeground>> * = nullptr>
void set_font_text_flag(console_attribute &attribute, const AttrClass &cv)
{
    attribute.set_foreground(default_console_attribute.get_foreground());
    attribute.set_attribute(PrintAttr::default_foreground);
}

template <typename Head> void print_fmt_text(console_attribute &attribute, const Head &head)
{
    using RealType = typename remove_extent<std::decay_t<decltype(head)>>::type;
    util::formatter::format<RealType> fmt;
    print_inner(fmt(head, attribute.temp_format_str, 128), attribute);
}

template <typename Head,
          typename std::enable_if_t<!has_member_attr<Head>::value & !has_member_flag<Head>::value> * = nullptr>
auto dispatch(console_attribute &attribute, const Head &head)
{
    return print_fmt_text(attribute, head);
}

template <template <typename S> typename Head, typename S>
auto dispatch(console_attribute &attribute, const Head<S> &head)
{
    return set_font_color_attr(attribute, head);
}

template <typename Head, typename std::enable_if_t<has_member_attr<Head>::value> * = nullptr>
auto dispatch(console_attribute &attribute, const Head &head)
{
    return set_font_text_attr(attribute, head);
}

template <typename Head, typename std::enable_if_t<has_member_flag<Head>::value> * = nullptr>
auto dispatch(console_attribute &attribute, const Head &head)
{
    return set_font_text_flag(attribute, head);
}

template <typename Head> void print_fmt(console_attribute &attribute, const Head &head) { dispatch(attribute, head); }

template <typename Head, typename... Args>
void print_fmt(console_attribute &attribute, const Head &head, const Args &... args)
{
    dispatch(attribute, head);
    print_fmt(attribute, args...);
}

template <typename... Args> void print(console_attribute &current_attribute, const Args &... args)
{
    print_fmt(current_attribute, args...);
}

NoReturn void keep_panic(const arch::idt::regs_t *regs = 0);

template <typename... Args> NoReturn void panic(const Args &... args)
{
    uctx::UnInterruptableContext icu;
    print(kernel_console_attribute, Foreground<Color::LightRed>(), "[panic]   ", PrintAttr::Reset(), args...);
    print(kernel_console_attribute, '\n');
    keep_panic();
}

template <typename... Args> NoReturn void panic_stack(const arch::idt::regs_t *regs, const Args &... args)
{
    uctx::UnInterruptableContext icu;
    print(kernel_console_attribute, Foreground<Color::LightRed>(), "[panic]   ", PrintAttr::Reset(), args...);
    print(kernel_console_attribute, '\n');
    keep_panic(regs);
}

template <typename... Args> void warning(const Args &... args)
{
    print(kernel_console_attribute, Foreground<Color::LightCyan>(), "[warning] ", PrintAttr::Reset(), args...);
    print(kernel_console_attribute, '\n');
}

template <typename... Args> void info(const Args &... args)
{
    print(kernel_console_attribute, Foreground<Color::Green>(), "[info]    ", PrintAttr::Reset(), args...);
    print(kernel_console_attribute, '\n');
}

template <typename... Args> void debug(const Args &... args)
{
    if (!output_debug)
        return;
    print(kernel_console_attribute, Foreground<Color::Brown>(), "[debug]   ", PrintAttr::Reset(), args...);
    print(kernel_console_attribute, '\n');
}

template <typename... Args> void assert_runtime(const char *exp, const char *file, int line, const Args &... args)
{
    print(kernel_console_attribute, Foreground<Color::LightRed>(), "[assert]  ", Foreground<Color::Red>(),
          "runtime assert failed: at: ", file, ':', line, "\n    assert expr: ", exp, '\n');
    panic("from assert failed. ", args...);
}

} // namespace trace

#ifdef _DEBUG
#define kassert(exp, ...)                                                                                              \
    do                                                                                                                 \
    {                                                                                                                  \
        if (unlikely(!(exp)))                                                                                          \
        {                                                                                                              \
            (trace::assert_runtime(#exp, __FILE__, __LINE__, __VA_ARGS__));                                            \
        }                                                                                                              \
    } while (0)
#else
#define kassert(exp, ...) void(0)
#endif
