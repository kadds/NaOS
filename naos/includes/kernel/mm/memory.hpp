#pragma once
#include "../kernel.hpp"
#include "allocator.hpp"
#include "common.hpp"
#include "mm.hpp"
#include "zone.hpp"
#include <utility>

namespace memory
{
namespace vm
{
class info_t;
} // namespace vm

extern vm::info_t *kernel_vm_info;

void init(kernel_start_args *args, u64 fix_memory_limit);
void paging();
u64 get_max_available_memory();
u64 get_max_maped_memory();

template <typename T> inline phy_addr_t va2pa(T virt_addr)
{
    return phy_addr_t::from(reinterpret_cast<byte *>(virt_addr) - linear_addr_offset);
}

template <typename T = void *> inline T pa2va(phy_addr_t phy_addr)
{
    return reinterpret_cast<T>((u64)phy_addr() + linear_addr_offset);
}

template <typename T> inline Unpaged_Text_Section phy_addr_t unpaged_va2pa(T virt_addr)
{
    return reinterpret_cast<T>(reinterpret_cast<byte*>(virt_addr) - linear_addr_offset);
}

template <typename T> inline Unpaged_Text_Section T unpaged_pa2va(phy_addr_t phy_addr)
{
    return reinterpret_cast<T>((u64)phy_addr() + linear_addr_offset);
}

inline phy_addr_t align_down(phy_addr_t v, u64 m)
{
    return phy_addr_t::from(reinterpret_cast<u64>(v()) & ~(m - 1));
}

inline phy_addr_t align_up(phy_addr_t v, u64 m)
{
    return phy_addr_t::from((reinterpret_cast<u64>(v()) + (m - 1)) & ~(m - 1));
}

template <typename T> inline T *align_down(T *v, u64 m)
{
    return reinterpret_cast<T *>(reinterpret_cast<u64>(v) & ~(m - 1));
}
template <typename T> inline T *align_up(T *v, u64 m)
{
    return reinterpret_cast<T *>((reinterpret_cast<u64>(v) + (m - 1)) & ~(m - 1));
}

class PhyBootAllocator : public IAllocator
{
  private:
    /// address to start allocation
    static phy_addr_t base_ptr;
    static phy_addr_t current_ptr;
    /// boot allocator is available
    static bool available;

  public:
    static inline void init(phy_addr_t base_ptr)
    {
        PhyBootAllocator::base_ptr = base_ptr;
        current_ptr = base_ptr;
        available = true;
    }

    static inline void discard() { available = false; }

    void *allocate(u64 size, u64 align) override
    {
        if (unlikely(!available))
            return nullptr;
        byte *start = align_up(current_ptr(), align);

        current_ptr = phy_addr_t::from(start + size);
        return start;
    }
    /// do nothing
    void deallocate(void *) override {}

    static void *current_ptr_address() { return current_ptr(); }
};

class VirtBootAllocator : public PhyBootAllocator
{
  public:
    void *allocate(u64 size, u64 align) override
    {
        return pa2va(phy_addr_t::from(PhyBootAllocator::allocate(size, align)));
    }
    /// do nothing. Same as PhyBootAllocator
    void deallocate(void *) override {}

    static void *current_ptr_address() { return pa2va(phy_addr_t::from(PhyBootAllocator::current_ptr_address())); }
};

void *kmalloc(u64 size, u64 align);
void kfree(void *addr);

void *vmalloc(u64 size, u64 align);
void vfree(void *addr);

void *malloc_page();
void free_page(void *addr);

void listen_page_fault();

extern VirtBootAllocator *VirtBootAllocatorV;
extern PhyBootAllocator *PhyBootAllocatorV;
extern zones *KernelBuddyAllocatorV;

} // namespace memory
