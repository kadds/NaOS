#pragma once
#include "allocator.hpp"
#include <new>
namespace memory
{

template <typename T, int align = alignof(T), typename... Args> T *New(IAllocator *allocator, Args &&... args)
{
    T *ptr = (T *)allocator->allocate(sizeof(T), align);
    return new (ptr) T(std::forward<Args>(args)...);
}

template <typename T> void Delete(IAllocator *allocator, T *t)
{
    t->~T();
    allocator->deallocate(t);
}

template <typename T, int align = alignof(T), typename... Args>
T *NewArray(IAllocator *allocator, u64 count, Args &&... args)
{
    T *ptr = (T *)allocator->allocate(sizeof(T) * count, align);
    for (u64 i = 0; i < count; i++)
    {
        new (ptr + i) T(std::forward<Args>(args)...);
    }
    return ptr;
}

template <typename T> void DeleteArray(IAllocator *allocator, T *t, u64 count)
{
    T *addr = t;
    for (u64 i = 0; i < count; i++)
    {
        addr->~T();
        addr++;
    }
    allocator->deallocate(t);
}

/// kmalloc and kfree
class KernelCommonAllocator : public IAllocator
{
  private:
  public:
    KernelCommonAllocator(){};
    ~KernelCommonAllocator(){};
    void *allocate(u64 size, u64 align) override;
    void deallocate(void *p) override;
};

/// vmalloc and vfree
class KernelVirtualAllocator : public IAllocator
{
  private:
  public:
    KernelVirtualAllocator(){};
    ~KernelVirtualAllocator(){};
    void *allocate(u64 size, u64 align) override;
    void deallocate(void *p) override;
};

/// Allocate virtual memory or fixed kernel memory depends on allocate size
class KernelMemoryAllocator : public IAllocator
{
  public:
    void *allocate(u64 size, u64 align) override;
    void deallocate(void *p) override;
};

class FixMemoryAllocator : public IAllocator
{
  private:
    void *ptr;

  public:
    FixMemoryAllocator(void *ptr)
        : ptr(ptr){};
    ~FixMemoryAllocator(){};
    void *allocate(u64 size, u64 align) override { return ptr; }
    void deallocate(void *p) override {}
};

extern KernelCommonAllocator *KernelCommonAllocatorV;
extern KernelVirtualAllocator *KernelVirtualAllocatorV;
extern KernelMemoryAllocator *KernelMemoryAllocatorV;

template <typename T> struct MemoryView
{
    T *ptr;
    IAllocator *allocator;
    MemoryView(IAllocator *allocator, u64 size, u64 align)
        : allocator(allocator)
    {
        ptr = (T *)allocator->allocate(size, align);
    }
    ~MemoryView() { allocator->deallocate((void *)ptr); }

    MemoryView(const MemoryView &) = delete;
    MemoryView &operator=(const MemoryView &) = delete;
    T &operator->() { return *ptr; }

    T *get() { return ptr; }
};

} // namespace memory
