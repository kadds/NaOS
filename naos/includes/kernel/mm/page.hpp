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
        buddy_free = 2,
        buddy_unavailable = 4,
        dirty = 8,
    };

  public:
    page()
        : ref_count(0)
        , ref_slab(nullptr)
    {
    }

    page(const page &) = delete;
    page(page &&) = delete;
    page &operator=(const page &) = delete;
    page &operator=(page &&) = delete;

    void set_buddy_next(page *next) { buddy_next = next; }
    page *get_buddy_next() const { return buddy_next; }
    void set_buddy_prev(page *prev) { buddy_prev = prev; }
    page *get_buddy_prev() const { return buddy_prev; }

    void set_ref_slab(slab_group *s) { ref_slab = s; }
    slab_group *get_ref_slab() const { return ref_slab; }

    void set_flag(u32 flag) { flags = flag; }
    void add_flag(u32 flag) { flags |= flag; }
    void rm_flag(u32 flag) { flags &= ~flag; }
    bool has_flag(u32 flag) { return flags & flag; }
    u32 get_ref_count() const { return ref_count; }
    void clear_ref_count() { ref_count = 0; }
    void add_ref_count() { ref_count++; }
    void remove_ref_count() { ref_count--; }

    void set_buddy_order(u8 order) { buddy_order = order; }
    u8 get_buddy_order() { return buddy_order; }

  private:
    u32 flags;
    u32 ref_count;
    union
    {
        slab_group *ref_slab;
        page *buddy_next;
    };
    union
    {
        page *buddy_prev;
    };
    union
    {
        u8 buddy_order;
    };
};

void init_pages();
page *get_page(void *ptr);

} // namespace memory