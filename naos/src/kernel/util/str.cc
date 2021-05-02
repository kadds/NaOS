#include "kernel/util/str.hpp"
namespace util
{
int strcmp(const char *str1, const char *str2)
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

i64 strcopy(char *dst, const char *src, i64 max_len)
{
    i64 i = 0;
    do
    {
        *dst++ = *src++;
    } while (*src != 0 && i++ < max_len);

    return i - 1;
}

i64 strcopy(char *dst, const char *src)
{
    const char *s = src;
    do
    {
        *dst++ = *src++;
    } while (*src != 0);

    return src - s - 1;
}

i64 strlen(const char *str)
{
    i64 i = 0;
    while (*str++ != 0)
        i++;
    return i++;
}
} // namespace util
