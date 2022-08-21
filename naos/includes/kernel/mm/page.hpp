#pragma once
#include "common.hpp"

namespace memory
{
class slab_group;

class page
{
  public:
    enum page_flag : u32
    {
        slab_set = 1,
        buddy_used = 2,
        buddy_unavailable = 4,
        buddy_merged = 8,
        dirty = 16,
    };

  public:
    page()
        : ref_count_(0)
        , ref_slab(nullptr)
    {
    }

    page(const page &) = delete;
    page(page &&) = delete;
    page &operator=(const page &) = delete;
    page &operator=(page &&) = delete;

    void set_buddy_next(u32 next) { buddy.next = next; }
    u32 get_buddy_next() const { return buddy.next; }

    void set_buddy_prev(u32 prev) { buddy.prev = prev; }
    u32 get_buddy_prev() const { return buddy.prev; }

    void set_ref_slab(slab_group *s) { ref_slab = s; }
    slab_group *get_ref_slab() const { return ref_slab; }

    void set_flags(u32 flags) { flags_ = flags; }
    void add_flags(u32 flags) { flags_ |= flags; }
    void remove_flags(u32 flags) { flags_ &= ~flags; }
    bool has_flags(u32 flags) { return flags_ & flags; }

    u32 get_ref_count() const { return ref_count_; }
    void clear_ref_count() { ref_count_ = 0; }
    void add_ref_count() { ref_count_++; }
    void remove_ref_count() { ref_count_--; }

    void set_buddy_order(int order) { buddy_order = order; }
    int get_buddy_order() const { return buddy_order; }
    u32 get_flags() const { return flags_; }

    void add_page_table_counter() { page_table_next_entries_++; }
    void sub_page_table_counter() { page_table_next_entries_--; }
    u16 page_table_counter() const { return page_table_next_entries_; }
    void set_page_table_counter(u16 c) { page_table_next_entries_ = c; }

  private:
    u32 flags_;
    u32 ref_count_;
    union
    {
        slab_group *ref_slab;
        u16 page_table_next_entries_;
        struct
        {
            u32 next;
            u32 prev;
        } buddy;
    };

    u8 buddy_order;
    void *mapping_;
};

void init_pages();
page *get_page(void *ptr);

} // namespace memory