#pragma once

#include "kernel/common.hpp"

namespace timeclock::unsigned_math
{

struct wide_u128
{
    u64 low = 0;
    u64 high = 0;
};

constexpr wide_u128 multiply(u64 lhs, u64 rhs) noexcept
{
    const u64 lhs0 = static_cast<u32>(lhs);
    const u64 lhs1 = lhs >> 32;
    const u64 rhs0 = static_cast<u32>(rhs);
    const u64 rhs1 = rhs >> 32;
    const u64 product0 = lhs0 * rhs0;
    const u64 product1 = lhs0 * rhs1;
    const u64 product2 = lhs1 * rhs0;
    const u64 product3 = lhs1 * rhs1;

    const u64 limb0 = static_cast<u32>(product0);
    const u64 middle = (product0 >> 32) + static_cast<u32>(product1) + static_cast<u32>(product2);
    const u64 limb1 = static_cast<u32>(middle);
    const u64 upper = (product1 >> 32) + (product2 >> 32) + static_cast<u32>(product3) + (middle >> 32);
    const u64 limb2 = static_cast<u32>(upper);
    const u64 limb3 = (product3 >> 32) + (upper >> 32);
    return {limb0 | (limb1 << 32), limb2 | (limb3 << 32)};
}

/// Divide a 128-bit unsigned value by a non-zero 64-bit value without using
/// a compiler-provided __udivti3 helper unavailable to the freestanding link.
constexpr bool try_divide(wide_u128 numerator, u64 denominator, u64 &quotient, u64 &remainder) noexcept
{
    if (denominator == 0)
        return false;
    quotient = 0;
    remainder = 0;
    bool overflow = false;
    for (i32 bit = 127; bit >= 0; bit--)
    {
        const bool numerator_bit =
            bit >= 64 ? ((numerator.high >> (bit - 64)) & 1u) != 0 : ((numerator.low >> bit) & 1u) != 0;
        bool quotient_bit = false;
        const u64 threshold = denominator - remainder;
        if (remainder >= threshold)
        {
            remainder = remainder - threshold;
            if (numerator_bit)
                remainder++;
            quotient_bit = true;
        }
        else if (numerator_bit && remainder + 1 >= threshold)
        {
            // The appended bit can make 2*remainder+bit reach the
            // denominator even when 2*remainder alone does not.
            remainder = remainder + 1 - threshold;
            quotient_bit = true;
        }
        else
        {
            remainder = remainder + remainder + (numerator_bit ? 1u : 0u);
        }
        if (quotient_bit)
        {
            if (bit >= 64)
                overflow = true;
            else
                quotient |= 1ULL << bit;
        }
    }
    return !overflow;
}

constexpr bool try_mul_div_floor(u64 lhs, u64 rhs, u64 denominator, u64 &result) noexcept
{
    u64 remainder = 0;
    return try_divide(multiply(lhs, rhs), denominator, result, remainder);
}

constexpr bool try_mul_div_ceil(u64 lhs, u64 rhs, u64 denominator, u64 &result) noexcept
{
    u64 remainder = 0;
    if (!try_divide(multiply(lhs, rhs), denominator, result, remainder))
        return false;
    if (remainder != 0)
    {
        if (result == static_cast<u64>(-1))
            return false;
        result++;
    }
    return true;
}

} // namespace timeclock::unsigned_math
