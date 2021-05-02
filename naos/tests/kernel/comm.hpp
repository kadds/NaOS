#pragma once
#include "kernel/mm/allocator.hpp"
#include <cstring>

class LibAllocator : public memory::IAllocator
{
  public:
    void *allocate(u64 size, u64 align)
    {
        void *p = new char[size];
        memset(p, 1, size);
        return p;
    }
    void deallocate(void *p) { delete[](char *) p; }
};
extern LibAllocator LibAllocatorV;
namespace util
{
u64 murmur_hash2_64(const void *addr, u64 len, u64 s);
}