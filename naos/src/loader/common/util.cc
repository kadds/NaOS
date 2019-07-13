#include "loader/util.hpp"
void memcopy(void *dst, void *source, u32 len)
{
    char *d = (char *)dst;
    char *s = (char *)source;
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