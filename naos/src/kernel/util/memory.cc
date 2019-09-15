#include "kernel/util/memory.hpp"
namespace util
{
void memset(void *dst, u64 val, u64 size)
{
    u64 arest = (u64)dst % sizeof(u64);
    u64 *start_aligned = (u64 *)((char *)dst + arest);
    size -= arest;
    u64 rest = size % sizeof(u64);
    u64 count = size / sizeof(u64);

    if (arest != 0)
    {
        char *dc = (char *)dst;
        do
        {
            *dc++ = val;
        } while (arest-- != 0);
    }

    for (u64 i = 0; i < count; i++)
    {
        *start_aligned++ = val;
    }

    if (rest != 0)
    {
        char *dc = (char *)start_aligned;
        do
        {
            *dc++ = val;
        } while (rest-- != 0);
    }
}

void memzero(void *dst, u64 size) { memset(dst, 0, size); }

void memcopy(void *dst, void *src, u64 size)
{
    u64 *d = (u64 *)dst;
    u64 *s = (u64 *)src;
    u64 count = size / sizeof(u64);
    u64 rest = size % sizeof(u64);
    for (u64 i = 0; i < count; i++)
    {
        *d++ = *s++;
    }
    if (rest != 0)
    {
        char *dc = (char *)d;
        char *sc = (char *)s;
        do
        {
            *dc++ = *sc++;
        } while (rest-- != 0);
    }
}
} // namespace util
