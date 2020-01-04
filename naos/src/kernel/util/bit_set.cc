#include "kernel/util/bit_set.hpp"

namespace util::bit_set_opeartion
{
inline constexpr u64 bits = sizeof(BaseType) * 8;

bool get(BaseType *data, u64 index)
{
    auto idx = index / bits;
    auto rest = index % bits;
    auto v = data[idx];
    v >>= rest;
    return v & 1;
}

void set(BaseType *data, u64 index)
{
    auto idx = index / bits;
    auto rest = index % bits;
    data[idx] |= ((1ul << rest));
}

/// set [0, element_count) to 1
void set_all(BaseType *data, u64 element_count)
{
    u64 bytes = (element_count + 7) / 8;
    util::memset(data, (BaseType)(-1), bytes);
}

/// set [start, start+element_count) to 1
void set_all(BaseType *data, u64 start, u64 element_count)
{
    u64 start_idx = start / bits;
    u64 start_rest = start % bits;
    data[start_idx] |= ~((1ul << start_rest) - 1);
    u64 end = start + element_count - 1;
    u64 end_idx = end / bits;
    u64 end_rest = end % bits;
    data[start_idx] |= ((1ul << end_rest) - 1);

    u64 bytes = (end_idx - start_idx) * 8;
    util::memset(data, (BaseType)(-1), bytes);
}

void clean(BaseType *data, u64 index)
{
    auto idx = index / bits;
    auto rest = index % bits;
    data[idx] &= ~((1ul << rest));
}

/// set [0, element_count) to 0
void clean_all(BaseType *data, u64 element_count)
{
    u64 bytes = (element_count + 7) / 8;
    util::memzero(data, bytes);
}

/// set [start, start+element_count) to 0
void clean_all(BaseType *data, u64 start, u64 element_count)
{
    u64 start_idx = start / bits;
    u64 start_rest = start % bits;
    data[start_idx] &= ((1ul << (start_rest)) - 1);
    u64 end = start + element_count - 1;
    u64 end_idx = end / bits;
    u64 end_rest = end % bits;
    data[start_idx] &= ~((1ul << (end_rest)) - 1);

    u64 bytes = (end_idx - start_idx) * 8;
    util::memzero(data, bytes);
}
/// scan [offset, offset+max_len)
u64 scan_zero(BaseType *data, u64 offset, u64 max_len)
{
    const BaseType fit = (BaseType)(-1);

    u64 start_idx = offset / bits;
    u64 start_rest = offset % bits;
    auto v = (~data[start_idx]) & ~((1ul << start_rest) - 1);
    if (v != 0)
    {
        return __builtin_ffsl(v) - 1 + start_idx * bits;
    }

    u64 end = offset + max_len - 1;
    u64 end_idx = end / bits;
    auto *d = data;
    for (u64 i = start_idx + 1; i < end_idx; i++)
    {
        if (d[i] != fit)
        {
            return __builtin_ffsl(~d[i]) - 1 + i * bits;
        }
    }

    auto e = data[end_idx];
    if (e != fit)
        return __builtin_ffsl(~e) - 1 + end_idx * bits;

    return fit;
}

/// scan [offset, offset+max_len)
u64 scan_set(BaseType *data, u64 offset, u64 max_len)
{
    u64 start_idx = offset / bits;
    u64 start_rest = offset % bits;
    auto v = data[start_idx] & ~((1ul << start_rest) - 1);
    if (v != 0)
    {
        return __builtin_ffsl(v) - 1 + start_idx * bits;
    }

    u64 end = offset + max_len - 1;
    u64 end_idx = end / bits;
    auto *d = data;
    for (u64 i = start_idx + 1; i < end_idx; i++)
    {
        if (d[i] != 0)
        {
            return __builtin_ffsl(d[i]) - 1 + i * bits;
        }
    }

    auto e = data[end_idx];
    if (e != 0)
    {
        return __builtin_ffsl(e) - 1 + end_idx * bits;
    }
    return (u64)-1;
}

} // namespace util::bit_set_opeartion
