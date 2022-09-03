#pragma once
#include "./arch/com.hpp"
#include "./arch/regs.hpp"
#include "./kernel.hpp"
#include "common.hpp"
#include "freelibcxx/circular_buffer.hpp"
#include "freelibcxx/formatter.hpp"
#include "lock.hpp"
#include "ucontext.hpp"
#include <initializer_list>
#include <type_traits>
#include <utility>

namespace term
{

void reset_panic_term();
}

/// kernel output and debug components
namespace trace
{
// set if print debug
extern bool output_debug;

void init();
void early_init();

template <typename T> using span = freelibcxx::span<T>;

template <typename E> struct prefix_hex_formatter
{
    E val;
};

template <typename In> struct format
{
    /// \brief The buffer length must >= 32 \n
    /// which can fully contain the maximum value of u64 and the maximum result length of double.
    const char *operator()(const In &in, span<char> buf) { return in; }
};

template <> struct format<const char *>
{
    using In = const char *;
    /// \brief return original input string
    const char *operator()(const char *in, span<char> buf) { return in; }
};

template <> struct format<char *>
{
    using In = char *;
    const char *operator()(const In &in, span<char> buf) { return in; }
};

template <> struct format<i64>
{
    const char *operator()(const i64 &in, span<char> buf)
    {
        auto pos = freelibcxx::int642str(buf, in).value_or(0);
        buf[pos] = 0;
        return buf.get();
    }
};

template <> struct format<i32>
{
    const char *operator()(const i32 &in, span<char> buf)
    {
        auto pos = freelibcxx::int2str(buf, in).value_or(0);
        buf[pos] = 0;
        return buf.get();
    }
};

template <> struct format<i16>
{
    const char *operator()(const i16 &in, span<char> buf)
    {
        auto pos = freelibcxx::int2str(buf, in).value_or(0);
        buf[pos] = 0;
        return buf.get();
    }
};

template <> struct format<i8>
{
    const char *operator()(const i8 &in, span<char> buf)
    {
        auto pos = freelibcxx::int2str(buf, in).value_or(0);
        buf[pos] = 0;
        return buf.get();
    }
};

template <> struct format<u64>
{
    const char *operator()(const u64 &in, span<char> buf)
    {
        auto pos = freelibcxx::uint642str(buf, in).value_or(0);
        buf[pos] = 0;
        return buf.get();
    }
};

template <> struct format<u32>
{
    const char *operator()(const u32 &in, span<char> buf)
    {
        auto pos = freelibcxx::uint2str(buf, in).value_or(0);
        buf[pos] = 0;
        return buf.get();
    }
};

template <> struct format<u16>
{
    const char *operator()(const u16 &in, span<char> buf)
    {
        auto pos = freelibcxx::uint2str(buf, in).value_or(0);
        buf[pos] = 0;
        return buf.get();
    }
};

template <> struct format<u8>
{
    const char *operator()(const u8 &in, span<char> buf)
    {
        auto pos = freelibcxx::uint2str(buf, in).value_or(0);
        buf[pos] = 0;
        return buf.get();
    }
};

template <> struct format<char>
{
    const char *operator()(const char &in, span<char> buf)
    {
        buf[0] = in;
        buf[1] = 0;
        return buf.get();
    }
};

template <> struct format<void *>
{
    using In = void *;
    const char *operator()(const In &in, span<char> buf)
    {
        buf[0] = '0';
        buf[1] = 'x';

        auto pos = freelibcxx::uint642str(buf.subspan(2), (uint64_t)in, 16).value_or(0);
        buf[pos + 2] = 0;
        return buf.get();
    }
};

template <> struct format<const void *>
{
    using In = const void *;
    const char *operator()(const In &in, span<char> buf)
    {
        buf[0] = '0';
        buf[1] = 'x';

        auto pos = freelibcxx::uint642str(buf.subspan(2), (uint64_t)in, 16).value_or(0);
        buf[pos + 2] = 0;
        return buf.get();
    }
};

template <> struct format<prefix_hex_formatter<u32>>
{
    using In = prefix_hex_formatter<u32>;
    const char *operator()(const In &in, span<char> buf)
    {
        constexpr int chars = sizeof(in.val) * 2;
        buf[0] = '0';
        buf[1] = 'x';

        auto pos = freelibcxx::uint2str(buf.subspan(2), in.val, 16).value_or(0);
        if (pos < chars)
        {
            memmove(buf.get() + 2 + chars - pos, buf.get() + 2, chars - pos);
            for (int i = pos; i < chars; i++)
            {
                buf[i + 2 - pos] = '0';
            }
            pos = chars;
        }
        buf[pos + 2] = 0;
        return buf.get();
    }
};
template <> struct format<prefix_hex_formatter<u64>>
{
    using In = prefix_hex_formatter<u64>;
    const char *operator()(const In &in, span<char> buf)
    {
        constexpr int chars = sizeof(in.val) * 2;
        buf[0] = '0';
        buf[1] = 'x';

        auto pos = freelibcxx::uint642str(buf.subspan(2), in.val, 16).value_or(0);
        if (pos < chars)
        {
            memmove(buf.get() + 2 + chars - pos, buf.get() + 2, chars - pos);
            for (int i = pos; i < chars; i++)
            {
                buf[i + 2 - pos] = '0';
            }
            pos = chars;
        }
        buf[pos + 2] = 0;
        return buf.get();
    }
};

///
/// \brief color namespace includes normal color
///
namespace Color
{
namespace Foreground
{
struct Black
{
    /// escape string
    static inline const char *t = "30";
};

struct Blue
{
    static inline const char *t = "34";
};

struct Green
{
    static inline const char *t = "32";
};

struct Cyan
{
    static inline const char *t = "36";
};

struct Red
{
    static inline const char *t = "31";
};

struct Magenta
{
    static inline const char *t = "35";
};

struct Brown
{
    static inline const char *t = "33";
};

struct LightGray
{
    static inline const char *t = "37";
};

struct DarkGray
{
    static inline const char *t = "90";
};

struct LightBlue
{
    static inline const char *t = "94";
};

struct LightGreen
{
    static inline const char *t = "92";
};

struct LightCyan
{
    static inline const char *t = "96";
};

struct LightRed
{
    static inline const char *t = "91";
};

struct Pink
{
    static inline const char *t = "95";
};

struct Yellow
{
    static inline const char *t = "93";
};

struct White
{
    static inline const char *t = "97";
};

struct FullColor
{
    static inline const char *t = "38;2";
};

struct Default
{
    static inline const char *t = "39";
};

} // namespace Foreground

namespace Background
{

struct Black
{
    static inline const char *t = "40";
};

struct Blue
{
    static inline const char *t = "44";
};

struct Green
{
    static inline const char *t = "42";
};

struct Cyan
{
    static inline const char *t = "46";
};

struct Red
{
    static inline const char *t = "41";
};

struct Magenta
{
    static inline const char *t = "45";
};

struct Brown
{
    static inline const char *t = "43";
};

struct LightGray
{
    static inline const char *t = "47";
};

struct DarkGray
{
    static inline const char *t = "100";
};

struct LightBlue
{
    static inline const char *t = "104";
};

struct LightGreen
{
    static inline const char *t = "102";
};

struct LightCyan
{
    static inline const char *t = "106";
};

struct LightRed
{
    static inline const char *t = "101";
};

struct Pink
{
    static inline const char *t = "105";
};

struct Yellow
{
    static inline const char *t = "103";
};

struct White
{
    static inline const char *t = "107";
};

struct FullColor
{
    static inline const char *t = "48;2";
};

struct Default
{
    static inline const char *t = "49";
};
} // namespace Background

namespace FG = Foreground;
namespace BK = Background;

} // namespace Color

namespace CFG = Color::FG;
namespace CBK = Color::BK;

/// Font attribute container
template <typename... Args> struct PrintAttribute
{
};

template <typename... Args> using PA = PrintAttribute<Args...>;

template <typename T> struct has_member_t
{
    template <typename U> static auto Check(U) -> typename std::decay<decltype(U::t)>::type;
    static void Check(...);
    using type = decltype(Check(std::declval<T>()));
    enum
    {
        value = !std::is_void_v<type>
    };
};

namespace TextAttribute
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
    framed = 51,
};

struct Reset
{
    static inline const char *t = "0";
    static const u8 flag = 0;
};

struct Bold
{
    static inline const char *t = "1";
    static const u8 attr = 1;
};
struct Italic
{
    static inline const char *t = "3";
    static const u8 attr = 3;
};
struct UnderLine
{
    static inline const char *t = "4";
    static const u8 attr = 4;
};
struct Blink
{
    static inline const char *t = "5";
    static const u8 attr = 5;
};
struct Reverse
{
    static inline const char *t = "7";
    static const u8 attr = 7;
};
struct Hide
{
    static inline const char *t = "8";
    static const u8 attr = 8;
};
struct Framed
{
    static inline const char *t = "51";
    static const u8 attr = 51;
};

} // namespace TextAttribute

struct NoneAttr
{
};

extern lock::spinlock_t spinlock;

/// print string to screen
void print_klog(const char *str);
void print_klog(const char *str, u64 len);
void print_klog(char ch);

/// the kernel print to the serial device. recv com1
extern arch::device::com::serial serial_device;

freelibcxx::circular_buffer<byte> &get_kernel_log_buffer();

/// ---------- template functions for kernel print logic ----------------

template <typename Head> Trace_Section void dispatch(const Head &head)
{
    using RealType = typename std::remove_extent<std::decay_t<decltype(head)>>::type;
    format<RealType> fmt;
    char fmt_str[64];
    span<char> buf(fmt_str, sizeof(fmt_str) - 1);
    print_klog(fmt(head, buf));
}

template <typename Head> Trace_Section void cast_fmt()
{
    print_klog(Head::t);
    print_klog('m');
}

template <typename Head, typename Second, typename... Args> Trace_Section void cast_fmt()
{
    print_klog(Head::t);
    print_klog(';');
    cast_fmt<Second, Args...>();
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"

///
/// \brief print format characters
///
/// \tparam Args
///
template <typename... Args> Trace_Section void print_fmt(PrintAttribute<Args...>)
{
    if constexpr (sizeof...(Args) != 0)
    {
        print_klog("\e[", 2);
        cast_fmt<Args...>();
    }
}

// -------------------------------

///
/// \brief kernel print string
///
/// \tparam PrintAttribute<...> the print attribute: color, bold, etc..
/// \tparam Args
/// \param args strings to print
///
template <typename TPrintAttribute = PrintAttribute<>, typename... Args> Trace_Section void print(Args &&... args)
{
    print_fmt<>(TPrintAttribute());
    auto i = std::initializer_list<int>{(dispatch(args), 0)...};
}

inline static Trace_Section void print_reset() { print<PrintAttribute<TextAttribute::Reset>>(); }

#pragma GCC diagnostic pop

void keep_panic(const regs_t *regs = 0);

///\brief stop all cpu and report an error.
template <typename... Args> NoReturn Trace_Section void panic(Args &&... args)
{
    term::reset_panic_term();
    {
        uctx::RawSpinLockUninterruptibleContext icu(spinlock);
        print<PrintAttribute<Color::Foreground::LightRed>>("[panic]   ");
        print<PrintAttribute<TextAttribute::Reset>>();
        print<>(std::forward<Args>(args)...);
        print<>('\n');
    }
    keep_panic();
    while (true)
    {
    }
}

template <typename... Args> Trace_Section void panic_stack(const regs_t *regs, Args &&...args)
{
    term::reset_panic_term();
    {
        uctx::RawSpinLockUninterruptibleContext icu(spinlock);
        print<PrintAttribute<Color::Foreground::LightRed>>("[panic]   ");
        print<PrintAttribute<TextAttribute::Reset>>();
        print<>(std::forward<Args>(args)...);
        print<>('\n');
    }
    keep_panic(regs);
}

template <typename... Args> Trace_Section void warning(Args &&... args)
{
    uctx::RawSpinLockUninterruptibleContext icu(spinlock);
    print<PrintAttribute<Color::Foreground::LightCyan>>("[warning] ");
    print<PrintAttribute<TextAttribute::Reset>>();
    print<>(std::forward<Args>(args)...);
    print<>('\n');
}

template <typename... Args> Trace_Section void info(Args &&...args)
{
    uctx::RawSpinLockUninterruptibleContext icu(spinlock);
    print<PrintAttribute<Color::Foreground::Green>>("[info]    ");
    print<PrintAttribute<TextAttribute::Reset>>();
    print<>(std::forward<Args>(args)...);
    print<>('\n');
}

template <typename... Args> Trace_Section void debug(Args &&...args)
{
    if (!output_debug)
        return;
    uctx::RawSpinLockUninterruptibleContext icu(spinlock);
    print<PrintAttribute<Color::Foreground::Brown>>("[debug]   ");
    print<PrintAttribute<TextAttribute::Reset>>();
    print<>(std::forward<Args>(args)...);
    print<>('\n');
}

template <typename T> void *hex(T t) { return reinterpret_cast<void *>(t); }
template <typename T> prefix_hex_formatter<u64> hex16(T t)
{
    return prefix_hex_formatter<u64>{reinterpret_cast<u64>(t)};
}
template <typename T> prefix_hex_formatter<u32> hex8(T t)
{
    return prefix_hex_formatter<u32>{reinterpret_cast<u32>(t)};
}

template <typename... Args>
Trace_Section void assert_runtime(const char *exp, const char *file, int line, Args &&... args)
{
    {
        uctx::RawSpinLockUninterruptibleContext icu(spinlock);
        print<PrintAttribute<Color::Foreground::LightRed>>("[assert]  ");
        print<PrintAttribute<Color::Foreground::Red>>("runtime assert failed at: ", file, ':', line,
                                                      "\n    assert expr: ", exp, '\n');
    }
    panic<>("from assert failed. ", std::forward<Args>(args)...);
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
