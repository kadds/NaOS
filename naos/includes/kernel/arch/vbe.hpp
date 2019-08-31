#pragma once
#include "../util/formatter.hpp"
#include "common.hpp"
#include <type_traits>
namespace arch::device::vbe
{
void init();
void mm_addr();

void set_size(u32 width, u32 height);
void putchar(char ch);
void putstring(const char *str);

void cls();

void move_pen(int px, int py);
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

template <typename Head> void print_fmt(const Head &head)
{
    char buf[128];
    using RealType = typename remove_extent<std::decay_t<decltype(head)>>::type;
    util::formatter::format<RealType> fmt;
    putstring(fmt(head, buf, 128));
}
template <typename Head, typename... Args> void print_fmt(const Head &head, const Args &... args)
{
    char buf[128];
    using RealType = typename remove_extent<std::decay_t<decltype(head)>>::type;
    util::formatter::format<RealType> fmt;

    putstring(fmt(head, buf, 128));
    print_fmt(args...);
}
template <typename... Args> void print(const Args &... args) { print_fmt(args...); }

} // namespace arch::device::vbe
