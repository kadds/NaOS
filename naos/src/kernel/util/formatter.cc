#include "kernel/util/formatter.hpp"

namespace util::formatter
{

void int2str(i64 in, char *buffer, int buffer_len)
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
    } while (intger != 0);

    if (in < 0)
    {
        buffer[index++] = '-';
    }
    int c = index / 2;
    for (int i = 0; i < c; i++)
    {
        char t = buffer[i];
        buffer[i] = buffer[index - i - 1];
        buffer[index - i - 1] = t;
    }
    buffer[index] = 0;
    if (index >= buffer_len)
    {
        ; // TODO:: err
    }
}

void uint2str(u64 in, char *buffer, int buffer_len)
{
    int index = 0;
    u64 intger = in;
    do
    {
        int rest = intger % 10;
        intger = intger / 10;
        buffer[index++] = '0' + rest;
    } while (intger != 0);

    int c = index / 2;
    for (int i = 0; i < c; i++)
    {
        char t = buffer[i];
        buffer[i] = buffer[index - i - 1];
        buffer[index - i - 1] = t;
    }
    buffer[index] = 0;
    if (index >= buffer_len)
    {
        ; // TODO:: err
    }
}

void pointer2str(const void *in, char *buffer, int buffer_len)
{
    int index = 0;
    u64 intger = (u64)in;
    do
    {
        int rest = intger % 16;
        intger = intger / 16;
        buffer[index++] = rest < 10 ? ('0' + rest) : ('a' + rest - 10);
    } while (intger != 0);

    int c = index / 2;
    for (int i = 0; i < c; i++)
    {
        char t = buffer[i];
        buffer[i] = buffer[index - i - 1];
        buffer[index - i - 1] = t;
    }
    buffer[index] = 0;
    if (index >= buffer_len)
    {
        ; // TODO:: err
    }
}
} // namespace util::formatter
