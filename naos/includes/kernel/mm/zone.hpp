#pragma once
#include "common.hpp"
#include "freelibcxx/allocator.hpp"
#include "kernel/lock.hpp"
#include "kernel/mm/page.hpp"
#include "kernel/types.hpp"
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

    u64 free_pages() const;
    u64 total_pages() const { return page_count; }

    phy_addr_t range_beg() const { return start; }
    phy_addr_t range_end() const { return end; }

    phy_addr_t allocate_range_beg() const { return start; }
    phy_addr_t allocate_range_end() const { return allocate_end; }

    page *page_beg() const { return address_to_page(start); }
    page *page_end() const;

    phy_addr_t malloc(u64 pages);
    void page_add_reference(phy_addr_t ptr);
    void free(phy_addr_t ptr);

    page *address_to_page(phy_addr_t ptr) const;
    phy_addr_t page_to_address(page *p) const;

  private:
    phy_addr_t start;
    phy_addr_t allocate_end;
    phy_addr_t end;

    u64 page_count;
    page *page_array;
    void *impl;

    lock::spinlock_t spin;

    u64 impl_ptr_[64 / sizeof(u64)];
};

class zones : public freelibcxx::Allocator
{
  public:
    zones(int count)
        : last_report_(0)
        , count_(count)
    {
    }
    zones(const zones &) = delete;
    zones(zones &&) = delete;
    zones &operator=(const zones &) = delete;
    zones &operator=(zones &&) = delete;
    zones() {}

    zone *at(int idx) { return &zone_array_[idx]; }

    int zone_count() const { return count_; }

    zone *which(phy_addr_t ptr);

    void tag_alloc(phy_addr_t start, phy_addr_t end);

    // virtual address
    void *allocate(size_t size, size_t align) noexcept override;
    // virtual address
    void deallocate(void *ptr) noexcept override;

    void page_add_reference(void *ptr);

    page *get_page(void *ptr);

    u64 total_pages() const;

    u64 free_pages() const;

  private:
    timeclock::microsecond_t last_report_;
    /// zone element count
    int count_;
    /// zones array
    zone zone_array_[0];
};

extern zones *global_zones;
} // namespace memory