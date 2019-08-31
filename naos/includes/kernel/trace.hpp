#pragma once
#include "arch/vbe.hpp"
#include "common.hpp"
namespace trace
{
extern bool output_debug;
template <typename... Args> NoReturn void panic(const Args &... args)
{
    arch::device::vbe::print("[panic] ", args...);
    arch::device::vbe::print('\n');
    while (1)
        ;
}
template <typename... Args> void info(const Args &... args)
{
    arch::device::vbe::print("[info] ", args...);
    arch::device::vbe::print('\n');
}

template <typename... Args> void debug(const Args &... args)
{
    if (!output_debug)
        return;
    arch::device::vbe::print("[debug] ", args...);
    arch::device::vbe::print('\n');
}

template <typename... Args> void assert_runtime(const char *exp, const char *file, int line, const Args &... args)
{
    arch::device::vbe::print("[assert] runtime assert failed: at: ", file, ':', line, "\n    assert expr: ", exp, '\n');
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