#pragma once
#include "common.hpp"
#include <utility>
class memory
{
  private:
    static void *base_ptr;
    static void *current_ptr;
    static u64 limit;

  public:
    template <typename T, typename... Args> static T *New(Args &&... args)
    {
        T *ptr = (T *)alloc(sizeof(T), alignof(T));
        new (ptr) T(std::forward<Args>(args)...);
        return ptr;
    }
    template <typename T, typename... Args> static T *NewArray(u64 count, Args &&... args)
    {
        T *ptr = (T *)alloc(sizeof(T) * count, alignof(T));
        for (u64 i = 0; i < count; i++)
        {
            new (ptr + i) T(std::forward<Args>(args)...);
        }
        return ptr;
    }
    static void *alloc(u64 size, u64 align)
    {
        char *start = (char *)(((u64)current_ptr + align - 1) & ~(align - 1));
        current_ptr = start + size;
        return start;
    }
    static void init(u64 data_base, u64 data_limit)
    {
        limit = data_limit;
        base_ptr = (void *)data_base;
        current_ptr = base_ptr;
    }
};
