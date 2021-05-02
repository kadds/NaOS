#pragma once
#include "common.hpp"
#include <type_traits>

namespace util::formatter
{
///
/// \brief cast intger to string
///
/// \param in the intger to cast
/// \param buffer the string buffer to write
/// \param buffer_len maximum number of characters in the string buffer
/// \note if the buffer is not large enough, the result is unknown
///
void int2str(i64 in, char *buffer, int buffer_len);

///
/// \brief cast unsigned intger to string
///
/// \param in the intger to cast
/// \param buffer the string buffer to write
/// \param buffer_len maximum number of characters in the string buffer
/// \note if the buffer is not large enough, the result is unknown
///
void uint2str(u64 in, char *buffer, int buffer_len);

///
/// \brief cast pointer address to string (hex)
///
/// \param in the intger to cast
/// \param buffer the string buffer to write
/// \param buffer_len maximum number of characters in the string buffer
/// \note if the buffer is not large enough, the result is unknown
///
void pointer2str(const void *in, char *buffer, int buffer_len);

template <typename In> struct format
{
    // The buffer length must >= 32, which can fully contain the maximum value of u64 and the maximum str length of
    // double.
    // FIXME: default operation
    const char *operator()(const In &in, char *buffer, int buffer_len) { return in; }
};

template <> struct format<const char *>
{
    using In = const char *;
    const char *operator()(const In &in, char *buffer, int buffer_len) { return in; }
};

template <> struct format<char *>
{
    using In = char *;
    const char *operator()(const In &in, char *buffer, int buffer_len) { return in; }
};

template <> struct format<i64>
{
    const char *operator()(const i64 &in, char *buffer, int buffer_len)
    {
        int2str(in, buffer, buffer_len);
        return buffer;
    }
};

template <> struct format<i32>
{
    const char *operator()(const i32 &in, char *buffer, int buffer_len)
    {
        int2str(in, buffer, buffer_len);
        return buffer;
    }
};

template <> struct format<i16>
{
    const char *operator()(const i16 &in, char *buffer, int buffer_len)
    {
        int2str(in, buffer, buffer_len);
        return buffer;
    }
};

template <> struct format<i8>
{
    const char *operator()(const i8 &in, char *buffer, int buffer_len)
    {
        int2str(in, buffer, buffer_len);
        return buffer;
    }
};

template <> struct format<u64>
{
    const char *operator()(const u64 &in, char *buffer, int buffer_len)
    {
        uint2str(in, buffer, buffer_len);
        return buffer;
    }
};

template <> struct format<u32>
{
    const char *operator()(const u32 &in, char *buffer, int buffer_len)
    {
        uint2str(in, buffer, buffer_len);
        return buffer;
    }
};

template <> struct format<u16>
{
    const char *operator()(const u16 &in, char *buffer, int buffer_len)
    {
        uint2str(in, buffer, buffer_len);
        return buffer;
    }
};

template <> struct format<u8>
{
    const char *operator()(const u8 &in, char *buffer, int buffer_len)
    {
        uint2str(in, buffer, buffer_len);
        return buffer;
    }
};

template <> struct format<char>
{
    const char *operator()(const char &in, char *buffer, int buffer_len)
    {
        buffer[0] = in;
        buffer[1] = 0;
        return buffer;
    }
};

template <> struct format<void *>
{
    using In = void *;
    const char *operator()(const In &in, char *buffer, int buffer_len)
    {
        buffer[0] = '0';
        buffer[1] = 'x';

        pointer2str(in, buffer + 2, buffer_len - 2);
        return buffer;
    }
};

template <> struct format<const void *>
{
    using In = const void *;
    const char *operator()(const In &in, char *buffer, int buffer_len)
    {
        buffer[0] = '0';
        buffer[1] = 'x';

        pointer2str(in, buffer + 2, buffer_len - 2);
        return buffer;
    }
};
} // namespace util::formatter
