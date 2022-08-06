#include "common.hpp"
#include "kernel/trace.hpp"
#include <cstdint>
#include <sys/types.h>

extern "C" void *memset(void *dst, int val, u64 size) noexcept
{
    const char *s = reinterpret_cast<const char *>(&val);
    char *d = reinterpret_cast<char *>(dst);

    if ((reinterpret_cast<uintptr_t>(dst) & 0x4) == 0)
    {
        u32 *d32 = (u32 *)dst;
        u32 count = size / sizeof(u32);
        u32 rest = size % sizeof(u32);
        for (u32 i = 0; i < count; i++)
        {
            *d32++ = val;
        }
        d = reinterpret_cast<char *>(d32);
        size = rest;
    }
    size_t n = 0;
    for (uint64_t i = 0; i < size; i++)
    {
        *d++ = *s++;
        if (n++ >= sizeof(u32))
        {
            s = reinterpret_cast<const char *>(&val);
            n = 0;
        }
    }
    return dst;
}

extern "C" void *memcpy(void *__restrict dst, const void *__restrict src, size_t size) noexcept
{
    const char *s = reinterpret_cast<const char *>(src);
    char *d = reinterpret_cast<char *>(dst);
    uintptr_t dst_address = reinterpret_cast<uintptr_t>(dst);
    uintptr_t src_address = reinterpret_cast<uintptr_t>(src);
#ifdef _DEBUG
    if (unlikely(dst == src))
    {
        return dst;
    }
    if (dst_address >= src_address && dst_address < src_address + size)
    {
        trace::panic("memcpy check fail");
    }
#endif
    if (unlikely((src_address & 0x7) == 0 && (dst_address & 0x7) == 0))
    {
        u64 *d64 = reinterpret_cast<u64 *>(dst);
        const u64 *s64 = reinterpret_cast<const u64 *>(src);
        u64 count = size / sizeof(u64);
        u64 rest = size % sizeof(u64);
        for (u64 i = 0; i < count; i++)
        {
            *d64++ = *s64++;
        }
        size = rest;
        d = reinterpret_cast<char *>(d64);
        s = reinterpret_cast<const char *>(s64);
    }
    for (uint64_t i = 0; i < size; i++)
    {
        *d++ = *s++;
    }
    return dst;
}

extern "C" int memcmp(const void *lhs, const void *rhs, size_t size) noexcept
{
    if (lhs == rhs)
    {
        return 0;
    }
    auto l = (char *)lhs;
    auto r = (char *)rhs;
    for (u64 i = 0; i < size; i++, l++, r++)
    {
        if (*l > *r)
            return 1;
        else if (*l < *r)
            return -1;
    }
    return 0;
}

extern "C" void *memmove(void *dst, const void *src, size_t size) noexcept
{
    if (unlikely(dst == src))
    {
        return dst;
    }
    const char *s = reinterpret_cast<const char *>(src);
    char *d = reinterpret_cast<char *>(dst);
    uintptr_t dst_address = reinterpret_cast<uintptr_t>(dst);
    uintptr_t src_address = reinterpret_cast<uintptr_t>(src);
    if (unlikely(dst_address < src_address + size))
    {
        if (unlikely((src_address & 0x7) == 0 && (dst_address & 0x7) == 0))
        {
            u64 *d64 = reinterpret_cast<u64 *>(dst_address + size);
            const u64 *s64 = reinterpret_cast<const u64 *>(src_address + size);
            u64 count = size / sizeof(u64);
            u64 rest = size % sizeof(u64);
            for (u64 i = 0; i < count; i++)
            {
                *--d64 = *--s64;
            }
            size = rest;
            d = reinterpret_cast<char *>(d64);
            s = reinterpret_cast<const char *>(s64);
        }
        else
        {
            s += size;
            d += size;
        }
        for (uint64_t i = 0; i < size; i++)
        {
            *--d = *--s;
        }
    }
    else
    {
        return memcpy(dst, src, size);
    }
    return dst;
}

extern "C" size_t strlen(const char *s) noexcept
{
    if (s == nullptr)
    {
        return 0;
    }
    int i = 0;
    while (*s++ != 0)
        i++;
    return i++;
}

extern "C" char *strcpy(char *dest, const char *src) noexcept
{
    char *d = dest;
    if (src == nullptr || dest == nullptr)
    {
        return dest;
    }
    while (*src != 0)
    {
        *dest++ = *src++;
    }
    *dest = 0;
    return d;
}

extern "C" const char *strstr(const char *haystack, const char *needle) noexcept
{
    size_t len = strlen(haystack);
    size_t pos = 0;
    while (pos < len)
    {
        int i = 0;
        for (; needle[i] != 0 && haystack[i] == needle[i]; i++)
        {
        }
        if (needle[i] == 0)
        {
            return haystack;
        }
        pos += i + 1;
        haystack += i + 1;
    }
    return nullptr;
}

extern "C" int strcmp(const char *str1, const char *str2) noexcept
{
    while (1)
    {
        char s1, s2;
        s1 = *str1++;
        s2 = *str2++;
        if (s1 != s2)
            return s1 < s2 ? -1 : 1;
        if (!s1)
            break;
    }
    return 0;
}

extern "C" void abort() { trace::panic("abort"); }

extern "C" void __cxa_pure_virtual()
{
    while (1)
        ;
}

void operator delete(void *p, size_t size) // or delete(void *, std::size_t)
{
    trace::info("delete ", trace::hex(p), " size ", size);
}

void operator delete(void *p) { trace::info("delete ", trace::hex(p)); }
