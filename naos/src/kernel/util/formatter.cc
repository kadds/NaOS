#include "kernel/util/formatter.hpp"
#include "kernel/util/str.hpp"

namespace util::formatter
{

int int2str(i64 in, char *buffer, int buffer_len)
{
    int index = 0;
    u64 intger;
    if (in < 0)
        intger = -in;
    else
        intger = in;
    do
    {
        int rest = intger % 10;
        intger = intger / 10;
        buffer[index++] = '0' + rest;
    } while (intger != 0 && index < buffer_len);

    if (in < 0)
    {
        if (index < buffer_len - 1)
        {
            buffer[index++] = '-';
        }
    }
    int c = index / 2;
    for (int i = 0; i < c; i++)
    {
        char t = buffer[i];
        buffer[i] = buffer[index - i - 1];
        buffer[index - i - 1] = t;
    }
    buffer[index] = 0;
    return index;
}

int uint2str(u64 in, char *buffer, int buffer_len)
{
    int index = 0;
    u64 intger = in;
    do
    {
        int rest = intger % 10;
        intger = intger / 10;
        buffer[index++] = '0' + rest;
    } while (intger != 0 && index < buffer_len);

    int c = index / 2;
    for (int i = 0; i < c; i++)
    {
        char t = buffer[i];
        buffer[i] = buffer[index - i - 1];
        buffer[index - i - 1] = t;
    }
    buffer[index] = 0;
    return index;
}

int pointer2str(const void *in, char *buffer, int buffer_len)
{
    int index = 0;
    u64 intger = (u64)in;
    do
    {
        int rest = intger % 16;
        intger = intger / 16;
        buffer[index++] = rest < 10 ? ('0' + rest) : ('a' + rest - 10);
    } while (intger != 0 && index < buffer_len);

    int c = index / 2;
    for (int i = 0; i < c; i++)
    {
        char t = buffer[i];
        buffer[i] = buffer[index - i - 1];
        buffer[index - i - 1] = t;
    }
    buffer[index] = 0;
    return index;
}

const char *str2int(const char *str, const char *end, i64 &result)
{
    result = 0;
    if (unlikely(end == nullptr))
    {
        end = str + strlen(str);
    }

    const char *p = str;
    int fac = 1;

    while (p != end)
    {
        char c = *p;
        if (c >= '0' && c <= '9')
        {
            result = result * 10 + (c - '0');
        }
        else if ((c == '-' || c == '+') && p == str)
        {
            if (c == '-')
            {
                fac = -1;
            }
        }
        else
        {
            return p;
        }
        p++;
    }
    result *= fac;
    return p;
}

const char *str2uint(const char *str, const char *end, u64 &result)
{
    result = 0;
    if (unlikely(end == nullptr))
    {
        end = str + strlen(str);
    }
    const char *p = str;

    while (p != end)
    {
        char c = *p;
        if (c >= '0' && c <= '9')
        {
            result = result * 10 + (c - '0');
        }
        else
        {
            return p;
        }
        p++;
    }
    return p;
}
} // namespace util::formatter
