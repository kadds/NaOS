#pragma once
#include "./arch/com.hpp"
#include "./arch/regs.hpp"
#include "./kernel.hpp"
#include "./util/formatter.hpp"
#include "./util/ring_buffer.hpp"
#include "common.hpp"
#include "lock.hpp"
#include "ucontext.hpp"
#include <initializer_list>
#include <type_traits>

/// kernel output and debug components
namespace trace
{
// set if print debug
extern bool output_debug;

void init();
void early_init();

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

extern lock::spinlock_t spinlock;

/// print string to screen
void print_inner(const char *str);
void print_inner(const char *str, u64 len);

/// the kernel print to the serial device. recv com1
extern arch::device::com::serial serial_device;

util::ring_buffer &get_kernel_log_buffer();

/// ---------- template functions for kernel print logic ----------------

template <typename Head> Trace_Section void dispatch(const Head &head)
{
    using RealType = typename remove_extent<std::decay_t<decltype(head)>>::type;
    util::formatter::format<RealType> fmt;
    char fmt_str[64];
    print_inner(fmt(head, fmt_str, 64));
}

template <typename Head> Trace_Section void cast_fmt()
{
    print_inner(Head::t);
    print_inner("m");
}

template <typename Head, typename Second, typename... Args> Trace_Section void cast_fmt()
{
    print_inner(Head::t);
    print_inner(";");
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
        print_inner("\e[");
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
/// \note call begin_print() before print<...>() and call end_print() after print<>(), if not, race conditions may occur
/// in the print<...>()
///
template <typename TPrintAttribute = PrintAttribute<>, typename... Args> Trace_Section void print(Args &&... args)
{
    print_fmt<>(TPrintAttribute());
    auto i = std::initializer_list<int>{(dispatch(args), 0)...};
}

#pragma GCC diagnostic pop

NoReturn void keep_panic(const regs_t *regs = 0);

///\brief stop all cpu and report an error. not realiable
template <typename... Args> NoReturn Trace_Section void panic(Args &&... args)
{
    uctx::RawSpinLockUninterruptibleContext icu(spinlock);
    print<PrintAttribute<Color::Foreground::LightRed>>("[panic]   ");
    print<PrintAttribute<TextAttribute::Reset>>();
    print<>(std::forward<Args>(args)...);
    print<>('\n');
    keep_panic();
}

template <typename... Args> NoReturn Trace_Section void panic_stack(const regs_t *regs, Args &&... args)
{
    uctx::RawSpinLockUninterruptibleContext icu(spinlock);
    print<PrintAttribute<Color::Foreground::LightRed>>("[panic]   ");
    print<PrintAttribute<TextAttribute::Reset>>();
    print<>(std::forward<Args>(args)...);
    print<>('\n');
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

template <typename... Args> Trace_Section void info(Args &&... args)
{
    uctx::RawSpinLockUninterruptibleContext icu(spinlock);
    print<PrintAttribute<Color::Foreground::Green>>("[info]    ");
    print<PrintAttribute<TextAttribute::Reset>>();
    print<>(std::forward<Args>(args)...);
    print<>('\n');
}

template <typename... Args> Trace_Section void debug(Args &&... args)
{
    if (!output_debug)
        return;
    uctx::RawSpinLockUninterruptibleContext icu(spinlock);
    print<PrintAttribute<Color::Foreground::Brown>>("[debug]   ");
    print<PrintAttribute<TextAttribute::Reset>>();
    print<>(std::forward<Args>(args)...);
    print<>('\n');
}

template <typename... Args>
Trace_Section void assert_runtime(const char *exp, const char *file, int line, Args &&... args)
{
    {
        uctx::RawSpinLockUninterruptibleContext icu(spinlock);
        print<PrintAttribute<Color::Foreground::LightRed>>("[assert]  ");
        print<PrintAttribute<Color::Foreground::Red>>("runtime assert failed: at: ", file, ':', line,
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
