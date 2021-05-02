#include "kernel/mm/zone.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/mm/page.hpp"
#include "kernel/trace.hpp"
#include "kernel/util/memory.hpp"

namespace memory
{

page *zones::get_page(void *ptr)
{
    phy_addr_t p = va2pa(ptr);
    zone *z = which(p);
    if (unlikely(z == nullptr))
    {
        return nullptr;
    }
    return z->address_to_page(p);
}

zone *zones::which(phy_addr_t ptr)
{
    int i = 0, j = count;
    while (i < j)
    {
        int mid = i + (j - i) / 2;
        phy_addr_t start = zone_array[mid].range_beg();
        phy_addr_t end = zone_array[mid].range_end();
        if (ptr >= start && ptr < end)
        {
            return &zone_array[mid];
        }
        else if (ptr < start)
        {
            j = mid;
        }
        else
        {
            i = mid + 1;
        }
    }
    return nullptr;
}

void *zones::allocate(u64 size, u64 align)
{
    // select one zone
    for (int i = 0; i < count; i++)
    {
        phy_addr_t p = zone_array[i].alloc(size);
        if (p != nullptr)
        {
            return pa2va(p);
        }
    }
    trace::panic("Kernel OOM allocate size ", size);
}

void zones::deallocate(void *ptr)
{
    phy_addr_t p = va2pa(ptr);
    zone *z = which(p);
    kassert(z != nullptr, "Not found this zone at ", reinterpret_cast<addr_t>(p()));
    z->free(p);
}

void zones::tag_alloc(phy_addr_t start, phy_addr_t end)
{
    kassert(start <= end, "address check fail");

    for (int i = 0; i < count; i++)
    {
        auto &z = zone_array[i];

        phy_addr_t s0 = start > z.range_beg() ? start : z.range_beg();
        phy_addr_t e0 = end < z.range_end() ? end : z.range_end();
        if (s0 < e0)
        {
            z.tag_alloc(s0, e0);
        }
    }
}

void zone::tag_alloc(phy_addr_t start, phy_addr_t end)
{
    kassert(start <= end, "address check fail");
    page *pstart = address_to_page(start);
    page *pend = address_to_page(end);
    buddy_impl.mark_unavailable(pstart, pend);
}

phy_addr_t zone::alloc(u64 size)
{
    page *p = buddy_impl.alloc((size + memory::page_size - 1) / memory::page_size);
    if (likely(p != nullptr))
    {
        return page_to_address(p);
    }
    return nullptr;
}

void zone::free(phy_addr_t ptr)
{
    page *p = address_to_page(ptr);
    buddy_impl.free(p);
}

page *zone::address_to_page(phy_addr_t ptr) const
{
    i64 off = ptr - start;
    kassert(off >= 0, "address is not this zone");
    kassert(off % memory::page_size == 0, "pointer is not a alignment pointer");
    i64 page_index = off / memory::page_size;
    return pages + page_index;
}

phy_addr_t zone::page_to_address(page *p) const
{
    i64 index = p - pages;
    if (unlikely(index < 0))
    {
        return nullptr;
    }
    return start + memory::page_size * index;
}

zone::zone(phy_addr_t start, phy_addr_t end, phy_addr_t danger_beg, phy_addr_t danger_end)
    : start(start)
    , end(end)
{
    page_count = (end - start) / page_size;
    u64 page_bytes = sizeof(page) * page_count;
    phy_addr_t page_addr = end - page_bytes;

    pages = memory::pa2va<page *>(page_addr);
    util::memzero(pages, page_bytes);

    if (page_addr >= danger_beg && page_addr < danger_end)
    {
        trace::panic("Zone page/buddy allocate failed at ", reinterpret_cast<addr_t>(start()), "-",
                     reinterpret_cast<addr_t>(end()), ", required ", page_bytes, '.');
    }
    buddy_block_count = buddy_impl.init(this);
    page *s = address_to_page(align_down(page_addr, page_size));
    page *e = address_to_page(end);
    buddy_impl.mark_unavailable(s, e);
}

} // namespace memory