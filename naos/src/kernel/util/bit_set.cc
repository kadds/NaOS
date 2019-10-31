#include "kernel/util/bit_set.hpp"

namespace util::bit_set_opeartion
{

bool get(byte *data, u64 index)
{
    auto idx = index / (sizeof(BaseType) * 8);
    auto rest = index % (sizeof(BaseType) * 8);
    auto v = ((BaseType *)data)[idx];
    v >>= rest;
    return v & 1;
}

void set(byte *data, u64 index)
{
    auto idx = index / (sizeof(BaseType) * 8);
    auto rest = index % (sizeof(BaseType) * 8);
    ((BaseType *)data)[idx] |= (((BaseType)1 << rest));
}

/// set [0, element_count) to 1
void set_all(byte *data, u64 element_count)
{
    u64 bytes = (element_count + 7) / 8;
    util::memset(data, (BaseType)(-1), bytes);
}
/// set [start, start+element_count) to 1
void set_all(byte *data, u64 start, u64 element_count)
{
    u64 start_idx = start / (sizeof(BaseType) * 8);
    u64 start_rest = start % (sizeof(BaseType) * 8);
    ((BaseType *)data)[start_idx] |= ~(((BaseType)1 << (start_rest)) - 1);
    u64 end = start + element_count - 1;
    u64 end_idx = end / (sizeof(BaseType) * 8);
    u64 end_rest = end % (sizeof(BaseType) * 8);
    ((BaseType *)data)[start_idx] |= (((BaseType)1 << (end_rest)) - 1);

    u64 bytes = (end_idx - start_idx) * 8;
    util::memset(data, (BaseType)(-1), bytes);
}

void clean(byte *data, u64 index)
{
    auto idx = index / (sizeof(BaseType) * 8);
    auto rest = index % (sizeof(BaseType) * 8);
    ((BaseType *)data)[idx] &= ~(((BaseType)1 << rest));
}

/// set [0, element_count) to 0
void clean_all(byte *data, u64 element_count)
{
    u64 bytes = (element_count + 7) / 8;
    util::memzero(data, bytes);
}

/// set [start, start+element_count) to 0
void clean_all(byte *data, u64 start, u64 element_count)
{
    u64 start_idx = start / (sizeof(BaseType) * 8);
    u64 start_rest = start % (sizeof(BaseType) * 8);
    ((BaseType *)data)[start_idx] &= (((BaseType)1 << (start_rest)) - 1);
    u64 end = start + element_count - 1;
    u64 end_idx = end / (sizeof(BaseType) * 8);
    u64 end_rest = end % (sizeof(BaseType) * 8);
    ((BaseType *)data)[start_idx] &= ~(((BaseType)1 << (end_rest)) - 1);

    u64 bytes = (end_idx - start_idx) * 8;
    util::memzero(data, bytes);
}
/// scan [offset, offset+max_len)
u64 scan_zero(byte *data, u64 offset, u64 max_len)
{
    const BaseType fit = (BaseType)(-1);

    u64 start_idx = offset / (sizeof(BaseType) * 8);
    u64 start_rest = offset % (sizeof(BaseType) * 8);
    auto v = ~((BaseType *)data)[start_idx] & ~(((BaseType)1 << start_rest) - 1);
    if (v != 0)
    {
        return __builtin_ffsl(v) - 1 + start_idx * sizeof(BaseType) * 8;
    }

    u64 end = offset + max_len - 1;
    u64 end_idx = end / (sizeof(BaseType) * 8);
    auto *d = ((BaseType *)data);
    for (u64 i = start_idx + 1; i < end_idx; i++)
    {
        if (d[i] != fit)
        {
            return __builtin_ffsl(~d[i]) - 1 + start_rest + i * sizeof(BaseType) * 8;
        }
    }

    auto e = ((BaseType *)data)[end_idx];
    if (e != 0)
    {
        return __builtin_ffsl(~e) - 1 + start_idx * sizeof(BaseType) * 8;
    }
    return (u64)-1;
}

/// scan [offset, offset+max_len)
u64 scan_set(byte *data, u64 offset, u64 max_len)
{
    u64 start_idx = offset / (sizeof(BaseType) * 8);
    u64 start_rest = offset % (sizeof(BaseType) * 8);
    auto v = ((BaseType *)data)[start_idx] & ~(((BaseType)1 << start_rest) - 1);
    if (v != 0)
    {
        return __builtin_ffsl(v) - 1 + start_idx * sizeof(BaseType) * 8;
    }

    u64 end = offset + max_len - 1;
    u64 end_idx = end / (sizeof(BaseType) * 8);
    auto *d = ((BaseType *)data);
    for (u64 i = start_idx + 1; i < end_idx; i++)
    {
        if (d[i] != 0)
        {
            return __builtin_ffsl(d[i]) - 1 + start_rest + i * sizeof(BaseType) * 8;
        }
    }

    auto e = ((BaseType *)data)[end_idx];
    if (e != 0)
    {
        return __builtin_ffsl(e) - 1 + start_idx * sizeof(BaseType) * 8;
    }
    return (u64)-1;
}

} // namespace util::bit_set_opeartion
