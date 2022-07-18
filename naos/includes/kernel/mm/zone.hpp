#pragma once
#include "allocator.hpp"
#include "buddy.hpp"
#include "common.hpp"
#include "kernel/lock.hpp"
#include "ucontext.h"

namespace memory
{

class zone
{
  public:
    /// phy addr
    zone(phy_addr_t start, phy_addr_t end, phy_addr_t danger_beg, phy_addr_t danger_end);
    /// tell buddy system memory needs to be reserved
    void tag_alloc(phy_addr_t start, phy_addr_t end);
    u64 pages_count() const { return page_count; }
    u64 free_pages() const;
    u64 buddy_blocks_count() const { return buddy_block_count; }

    phy_addr_t range_beg() const { return start; }
    phy_addr_t range_end() const { return end; }

    phy_addr_t allocate_range_beg() const { return start; }
    phy_addr_t allocate_range_end() const { return allocate_end; }

    page *page_beg() const { return address_to_page(start); }
    page *page_end() const;

    phy_addr_t alloc(u64 size);
    void add_reference(phy_addr_t ptr);
    void free(phy_addr_t ptr);

    page *address_to_page(phy_addr_t ptr) const;
    phy_addr_t page_to_address(page *p) const;

  private:
    phy_addr_t start;
    phy_addr_t allocate_end;
    phy_addr_t end;
    u64 page_count;
    u64 buddy_block_count;
    /// buddy pointer
    buddy buddy_impl;
    // kernel virtual address
    page *pages;
    lock::spinlock_t spin;
};

class zones : public IAllocator
{
  public:
    zones(int count)
        : count(count)
    {
    }
    zones(const zones &) = delete;
    zones(zones &&) = delete;
    zones &operator=(const zones &) = delete;
    zones &operator=(zones &&) = delete;
    zones() {}

    void set_zone_count(int count) { this->count = count; }

    zone *at(int idx) { return &zone_array[idx]; }

    int zone_count() const { return count; }

    zone *which(phy_addr_t ptr);

    void tag_alloc(phy_addr_t start, phy_addr_t end);

    // virtual address
    void *allocate(u64 size, u64 align) override;
    // virtual address
    void deallocate(void *ptr) override;

    void add_reference(void *ptr);

    page *get_page(void *ptr);

    u64 pages_count() const;

    u64 free_pages() const;

  private:
    /// zone element count
    int count;
    /// zones array
    zone zone_array[0];
};

extern zones *global_zones;
} // namespace memory