/// \file common.hpp
/// \brief Head file for general definition.
///
/// Macro and type definition.
///
/// \author Kadds

#pragma once
#include <cstddef>
#include <cstdint>

#ifdef _MSC_VER
#error Not support msvc.
#endif

#if defined __GNUC__ || defined __clang__
#define ExportC extern "C"
#endif

#define NoReturn [[noreturn]]
#define PackStruct __attribute__((packed))
#define Aligned(v) __attribute__((aligned(v)))
#define Section(v) __attribute__((section(v)))

// type defs
typedef int8_t i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;
typedef __int128_t i128;

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef __uint128_t u128;
typedef void *addr_t;
typedef const void *coaddr_t;

/// easy use likely or unlikely
#ifndef likely
#define likely(x) __builtin_expect(!!(x), 1)
#endif
#ifndef unlikely
#define unlikely(x) __builtin_expect(!!(x), 0)
#endif

/// C++17 byte
typedef std::byte byte;
typedef u64 flag_t;

class phy_addr_t
{
  public:
    phy_addr_t() {}
    phy_addr_t(std::nullptr_t)
        : ptr(nullptr)
    {
    }
    template <typename T> static phy_addr_t from(T v) { return phy_addr_t(reinterpret_cast<void *>(v)); }
    byte *operator()() const { return ptr; }
    phy_addr_t operator+(ptrdiff_t val) const { return ptr + val; }
    phy_addr_t operator-(ptrdiff_t val) const { return ptr - val; }
    phy_addr_t &operator+=(ptrdiff_t val)
    {
        ptr += val;
        return *this;
    }
    phy_addr_t &operator-=(ptrdiff_t val)
    {
        ptr -= val;
        return *this;
    }

    ptrdiff_t operator-(phy_addr_t rhs) const { return ptr - rhs.ptr; }

    bool operator>(phy_addr_t rhs) const { return ptr > rhs.ptr; }
    bool operator<(phy_addr_t rhs) const { return ptr < rhs.ptr; }
    bool operator>=(phy_addr_t rhs) const { return ptr >= rhs.ptr; }
    bool operator<=(phy_addr_t rhs) const { return ptr <= rhs.ptr; }
    bool operator==(phy_addr_t rhs) const { return ptr == rhs.ptr; }
    bool operator!=(phy_addr_t rhs) const { return ptr != rhs.ptr; }

    byte *get() const { return ptr; }

  private:
    phy_addr_t(void *ptr)
        : ptr(reinterpret_cast<byte *>(ptr))
    {
    }
    byte *ptr;
};

constexpr u64 memory_size_kb(u64 kb) { return kb * 0x400UL; }
constexpr u64 memory_size_mb(u64 mb) { return mb * 0x100000UL; }
constexpr u64 memory_size_gb(u64 gb) { return gb * 0x40000000UL; }
constexpr u64 memory_size_tb(u64 tb) { return tb * 0x10000000000UL; }

/// C++ init function
typedef void (*InitFunc)(void);
/// define by compiler
ExportC InitFunc __init_array_start;
ExportC InitFunc __init_array_end;

/// Initialize for c++ global variables
static inline void static_init()
{
    InitFunc *pFunc = &__init_array_start;

    for (; pFunc < &__init_array_end; ++pFunc)
    {
        (*pFunc)();
    }
}

extern "C" void *memcpy(void *__restrict dest, const void *__restrict src, size_t n) noexcept;

extern "C" void *memmove(void *dest, const void *src, size_t n) noexcept;

extern "C" void *memset(void *dest, int val, size_t n) noexcept;

extern "C" size_t strlen(const char *s) noexcept;

extern "C" char *strcpy(char *dest, const char *src) noexcept;

extern "C" const char *strstr(const char *haystack, const char *needle) noexcept;

extern "C" int strcmp(const char *str1, const char *str2) noexcept;

extern "C" int memcmp(const void *a, const void *b, size_t size) noexcept;

extern "C" void __cxa_pure_virtual();