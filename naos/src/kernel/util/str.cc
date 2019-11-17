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

int strcopy(char *dst, const char *src, int max_len)
{
    int i = 0;
    while (*src != 0 && i++ < max_len)
        *dst++ = *src++;
    return i;
}

void strcopy(char *dst, const char *src)
{
    while (*src != 0)
        *dst++ = *src++;
}

int strlen(const char *str)
{
    int i = 0;
    while (*str++ != 0)
        i++;
    return i++;
}
} // namespace util
