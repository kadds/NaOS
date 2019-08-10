#pragma once
#include "common.hpp"
namespace memory
{
class IAllocator
{
  public:
    IAllocator(){};
    virtual ~IAllocator(){};
    virtual void *allocate(u64 size, u64 align) = 0;
    virtual void deallocate(void *ptr) = 0;
};

} // namespace memory
