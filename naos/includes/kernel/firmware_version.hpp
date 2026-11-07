#pragma once

#include "kernel/types.hpp"
#include <array>

namespace firmware
{
// Opaque on the wire: 32 lowercase hex characters encode the low eight bytes
// of boot_epoch followed by the low eight bytes of firmware snapshot generation,
// both little-endian. The IDL string remains bounded only by the message size.
using version_storage = std::array<char, 32>;

constexpr version_storage encode_version(u64 boot_epoch, u64 firmware_generation) noexcept
{
    constexpr char hex[] = "0123456789abcdef";
    version_storage result{};
    for (u32 index = 0; index < 8; ++index)
    {
        const auto boot_byte = static_cast<u8>(boot_epoch >> (index * 8));
        const auto generation_byte = static_cast<u8>(firmware_generation >> (index * 8));
        result[index * 2] = hex[boot_byte >> 4];
        result[index * 2 + 1] = hex[boot_byte & 0xf];
        result[(index + 8) * 2] = hex[generation_byte >> 4];
        result[(index + 8) * 2 + 1] = hex[generation_byte & 0xf];
    }
    return result;
}

constexpr int hex_value(char value) noexcept
{
    if (value >= '0' && value <= '9')
        return value - '0';
    if (value >= 'a' && value <= 'f')
        return value - 'a' + 10;
    return -1;
}

template <typename String> constexpr bool decode_version(const String &token, u64 &boot_epoch, u64 &firmware_generation)
{
    boot_epoch = 0;
    firmware_generation = 0;
    if (token.data == nullptr || token.size != 32)
        return false;
    for (u32 index = 0; index < 8; ++index)
    {
        const int boot_high = hex_value(token.data[index * 2]);
        const int boot_low = hex_value(token.data[index * 2 + 1]);
        const int generation_high = hex_value(token.data[(index + 8) * 2]);
        const int generation_low = hex_value(token.data[(index + 8) * 2 + 1]);
        if (boot_high < 0 || boot_low < 0 || generation_high < 0 || generation_low < 0)
            return false;
        boot_epoch |= static_cast<u64>((boot_high << 4) | boot_low) << (index * 8);
        firmware_generation |= static_cast<u64>((generation_high << 4) | generation_low) << (index * 8);
    }
    return boot_epoch != 0 && firmware_generation != 0;
}

template <typename String>
constexpr void write_version(String &destination, version_storage &storage, u64 boot_epoch,
                             u64 firmware_generation) noexcept
{
    storage = encode_version(boot_epoch, firmware_generation);
    destination.data = storage.data();
    destination.size = static_cast<u32>(storage.size());
}

} // namespace firmware
