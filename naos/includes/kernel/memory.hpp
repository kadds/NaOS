#pragma once
#include "common.hpp"
#include <utility>
namespace memory
{
extern void *base_ptr;
extern void *current_ptr;
extern u64 limit;

void *alloc(u64 size, u64 align);
void init(u64 data_base, u64 data_limit);

template <typename T, typename... Args> T *New(Args &&... args)
{
    T *ptr = (T *)alloc(sizeof(T), alignof(T));
    new (ptr) T(std::forward<Args>(args)...);
    return ptr;
}
template <typename T, typename... Args> T *NewArray(u64 count, Args &&... args)
{
    T *ptr = (T *)alloc(sizeof(T) * count, alignof(T));
    for (u64 i = 0; i < count; i++)
    {
        new (ptr + i) T(std::forward<Args>(args)...);
    }
    return ptr;
}

} // namespace memory
