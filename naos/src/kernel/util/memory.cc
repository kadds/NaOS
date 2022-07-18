#include "kernel/util/memory.hpp"
namespace util
{
void memset(void *dst, u64 val, u64 size)
{
    const char *s = reinterpret_cast<const char *>(&val);
    char *d = reinterpret_cast<char *>(dst);

    if ((reinterpret_cast<u64>(dst) & 0x7) == 0)
    {
        u64 *d64 = (u64 *)dst;
        u64 count = size / sizeof(u64);
        u64 rest = size % sizeof(u64);
        for (u64 i = 0; i < count; i++)
        {
            *d64++ = val;
        }
        d = reinterpret_cast<char *>(d64);
        size = rest;
    }
    size_t n = 0;
    for (uint64_t i = 0; i < size; i++)
    {
        *d++ = *s++;
        if (n++ >= sizeof(u64))
        {
            s = reinterpret_cast<const char *>(&val);
            n = 0;
        }
    }
}

void memzero(void *dst, u64 size) { memset(dst, 0, size); }

void memcopy(void *dst, const void *src, u64 size)
{
    if (unlikely(dst == src))
    {
        return;
    }
    const char *s = reinterpret_cast<const char *>(src);
    char *d = reinterpret_cast<char *>(dst);
    u64 dst_address = reinterpret_cast<u64>(dst);
    u64 src_address = reinterpret_cast<u64>(src);
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
        for (uint64_t i = 0; i < size; i++)
        {
            *--d = *--s;
        }
    }
    else
    {
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
    }
}

int memcmp(const void *lhs, const void *rhs, u64 size)
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

} // namespace util
