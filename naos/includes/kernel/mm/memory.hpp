#pragma once
#include "../kernel.hpp"
#include "allocator.hpp"
#include "common.hpp"
#include "mm.hpp"
#include <utility>

namespace memory
{
namespace vm
{
struct info_t;
} // namespace vm

extern vm::info_t *kernel_vm_info;

void tag_zone_buddy_memory(void *start_addr, void *end_addr);

void init(const kernel_start_args *args, u64 fix_memory_limit);
void paging();
u64 get_max_available_memory();
u64 get_max_maped_memory();

struct zone_t
{
    void *start;
    void *end;
    u64 page_count;
    /// buddy pointer
    void *buddy_impl;
    /// tell buddy system memory needs to be reserved
    void tag_used(u64 offset_start, u64 offset_end);
};

struct zones_t
{
    /// zones array
    zone_t *zones;
    /// zone element count
    int count;
};

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
    /// address to start allocation
    static void *base_ptr;
    static void *current_ptr;
    /// is enable boot allocator
    static bool available;

  public:
    static void init(void *base_ptr)
    {
        PhyBootAllocator::base_ptr = base_ptr;
        current_ptr = base_ptr;
        available = true;
    }

    static void discard() { available = false; }

    void *allocate(u64 size, u64 align) override
    {

        if (unlikely(!available))
            return nullptr;

        char *start = (char *)(((u64)current_ptr + align - 1) & ~(align - 1));
        current_ptr = start + size;

        return start;
    }
    /// do nothing
    void deallocate(void *) override {}

    static void *current_ptr_address() { return current_ptr; }
};

class VirtBootAllocator : public PhyBootAllocator
{
  public:
    void *allocate(u64 size, u64 align) override
    {
        return kernel_phyaddr_to_virtaddr(PhyBootAllocator::allocate(size, align));
    }
    /// do nothing. Same as PhyBootAllocator
    void deallocate(void *) override {}

    static void *current_ptr_address() { return kernel_phyaddr_to_virtaddr(PhyBootAllocator::current_ptr_address()); }
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

} // namespace memory
