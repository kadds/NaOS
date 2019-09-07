#pragma once
#include "../kernel.hpp"
#include "../trace.hpp"
#include "allocator.hpp"
#include "common.hpp"
#include <new>
#include <utility>
namespace memory
{

void init(const kernel_start_args *args, u64 fix_memory_limit);
u64 get_max_available_memory();
u64 get_max_maped_memory();

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

struct zone_t
{
    void *start;
    void *end;
    u64 props;
    u64 page_count;
    void *buddy_impl;
    void tag_used(u64 offset_start, u64 offset_end);
    struct prop
    {
        enum
        {
            present = 1,
            fixed = 2,
        };
    };
};

struct zones_t
{
    zone_t *zones;
    int count;
};

extern const u64 linear_addr_offset;
extern const u64 page_size;
extern zones_t global_zones;

template <typename Tptr> inline Tptr kernel_virtaddr_to_phyaddr(Tptr virt_addr)
{
    return (Tptr)((char *)virt_addr - linear_addr_offset);
}

template <typename Tptr> inline Tptr kernel_phyaddr_to_virtaddr(Tptr phy_addr)
{
    return (Tptr)((char *)phy_addr + linear_addr_offset);
}

template <typename Tptr> inline Unpaged_Text_Section Tptr unpaged_kernel_virtaddr_to_phyaddr(Tptr virt_addr)
{
    return (Tptr)((char *)virt_addr - linear_addr_offset);
}

template <typename Tptr> inline Unpaged_Text_Section Tptr unpaged_kernel_phyaddr_to_virtaddr(Tptr phy_addr)
{
    return (Tptr)((char *)phy_addr + linear_addr_offset);
}

class PhyBootAllocator : public IAllocator
{
  private:
    static void *base_ptr;
    static void *current_ptr;

  public:
    static void init(void *base_ptr)
    {
        PhyBootAllocator::base_ptr = base_ptr;
        current_ptr = base_ptr;
    }
    PhyBootAllocator(){};
    ~PhyBootAllocator(){};
    void *allocate(u64 size, u64 align) override
    {
        char *start = (char *)(((u64)current_ptr + align - 1) & ~(align - 1));
        current_ptr = start + size;

        return start;
    }
    void deallocate(void *) override {}
    static void *current_ptr_address() { return current_ptr; }
};

class VirtBootAllocator : public PhyBootAllocator
{
  private:
  public:
    VirtBootAllocator(){};
    ~VirtBootAllocator(){};
    void *allocate(u64 size, u64 align) override
    {
        return kernel_phyaddr_to_virtaddr(PhyBootAllocator::allocate(size, align));
    }
    void deallocate(void *) override {}
    static void *current_ptr_address() { return kernel_phyaddr_to_virtaddr(PhyBootAllocator::current_ptr_address()); }
};

void *kmalloc(u64 size, u64 align);
void kfree(void *addr);

// It uses kmalloc and kfree
class KernelCommonAllocator : public IAllocator
{
  private:
  public:
    KernelCommonAllocator(){};
    ~KernelCommonAllocator(){};
    void *allocate(u64 size, u64 align) override { return kmalloc(size, align); }
    void deallocate(void *p) override { kfree(p); }
    static void *current_ptr_address() { return kernel_phyaddr_to_virtaddr(PhyBootAllocator::current_ptr_address()); }
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
extern VirtBootAllocator *VirtBootAllocatorV;
extern PhyBootAllocator *PhyBootAllocatorV;
extern KernelCommonAllocator *KernelCommonAllocatorV;

} // namespace memory
