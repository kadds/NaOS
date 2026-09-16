#pragma once

#include "freelibcxx/formatter.hpp"
#include "kernel/common.hpp"

namespace log::format
{
/// Append a log message while preserving line boundaries and making the
/// remaining control bytes visible in text sinks.
inline void append_escaped(freelibcxx::format_detail::writer &output, const char *message, u64 length)
{
    static constexpr char hex_digits[] = "0123456789abcdef";
    for (u64 index = 0; index < length; index++)
    {
        const u8 value = static_cast<u8>(message[index]);
        if (value == '\n')
            output.put('\n');
        else if (value == '\r')
            output.put("\\r");
        else if (value == '\t')
            output.put("\\t");
        else if (value == 0x1b)
            output.put("\\x1b");
        else if (value < 0x20 || value == 0x7f)
        {
            output.put("\\x");
            output.put(hex_digits[value >> 4]);
            output.put(hex_digits[value & 0xf]);
        }
        else
            output.put(message[index]);
    }
}
} // namespace log::format
