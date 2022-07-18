#pragma once
#include "allocator.hpp"
#include "mm.hpp"

namespace memory
{

class page;
class zone;
constexpr int buddy_max_order = 10;
constexpr int buddy_max_page = 1 << buddy_max_order; // 1024 page
constexpr int buddy_size = buddy_max_page * 2 - 1;
constexpr int buddy_max_alloc_size = buddy_max_page * memory::page_size;

class buddy
{
  private:
    page *orders[buddy_max_order + 1];
    zone *z;

    u64 next_fit_size(u64 size);

  public:
    buddy();
    i64 init(zone *z);
    ~buddy() = default;
    buddy(const buddy &) = delete;
    buddy(buddy &&) = delete;
    buddy &operator=(const buddy &) = delete;
    buddy &operator=(buddy &&) = delete;

    page *alloc(u64 size);
    void free(page *p);

    bool mark_unavailable(page *s, page *e);
    u64 scan_free() const;

  private:
    void merge(u8 order, bool fast);
    page *get_bro(page *p);
    page *split_buddy(page *p);
    void remove(page *p);
};

} // namespace memory
