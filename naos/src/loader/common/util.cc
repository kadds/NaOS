#include "loader/util.hpp"
ExportC void memcpy(void *dst, const void *source, u32 len)
{
    char *d = (char *)dst;
    const char *s = (const char *)source;
    for (u32 i = 0; i < len; i++)
    {
        *d++ = *s++;
    }
}
void memzero(void *dst, u32 len)
{
    char *d = (char *)dst;
    for (u32 i = 0; i < len; i++)
    {
        *d++ = 0;
    }
}
