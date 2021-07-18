#pragma once
#include "allocator.hpp"
#include <new>
#include <utility>

namespace memory
{

template <typename T, typename I, int align = alignof(T), typename... Args> T *New(I allocator, Args &&...args)
{
    T *ptr = (T *)allocator->allocate(sizeof(T), align);
    return new (ptr) T(std::forward<Args>(args)...);
}

template <typename T, typename I> void Delete(I allocator, T *t)
{
    t->~T();
    allocator->deallocate(t);
}

template <typename T, typename I, int align = alignof(T), typename... Args>
T *NewArray(I allocator, u64 count, Args &&...args)
{
    T *ptr = (T *)allocator->allocate(sizeof(T) * count, align);
    for (u64 i = 0; i < count; i++)
    {
        new (ptr + i) T(std::forward<Args>(args)...);
    }
    return ptr;
}

template <typename T, typename I> void DeleteArray(I allocator, T *t, u64 count)
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
class MemoryAllocator : public IAllocator
{
  public:
    void *allocate(u64 size, u64 align) override;
    void deallocate(void *p) override;
};

extern KernelCommonAllocator *KernelCommonAllocatorV;
extern KernelVirtualAllocator *KernelVirtualAllocatorV;
extern MemoryAllocator *MemoryAllocatorV;

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
