#pragma once
#include "arch/video/vga/vga.hpp"
#include "common.hpp"

namespace trace
{

extern bool output_debug;
template <typename... Args> NoReturn void panic(const Args &... args)
{
    using namespace arch::device::vga;
    print(AlignmentValue(10), Foreground<Color::LightRed>(), "[panic]   ", Foreground<Color::LightGray>(), args...);
    print('\n');
    flush();
    while (1)
        ;
}

template <typename... Args> void info(const Args &... args)
{
    using namespace arch::device::vga;
    print(AlignmentValue(10), Foreground<Color::Green>(), "[info]    ", Foreground<Color::LightGray>(), args...);
    print('\n');
}

template <typename... Args> void debug(const Args &... args)
{
    using namespace arch::device::vga;
    if (!output_debug)
        return;
    print(AlignmentValue(10), Foreground<Color::Brown>(), "[debug]   ", Foreground<Color::LightGray>(), args...);
    print('\n');
}

template <typename... Args> void assert_runtime(const char *exp, const char *file, int line, const Args &... args)
{
    using namespace arch::device::vga;
    print(AlignmentValue(10), Foreground<Color::LightRed>(), "[assert]  ", Foreground<Color::Red>(),
          "runtime assert failed: at: ", file, ':', line, "\n    assert expr: ", exp, '\n');
    panic("from assert failed. ", args...);
}

} // namespace trace

#ifndef NDEBUG
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
