#include "kernel/mm/zone.hpp"
#include "common.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/mm/page.hpp"
#include "kernel/trace.hpp"
#include "kernel/ucontext.hpp"
#include "kernel/util/memory.hpp"
#include <sys/ucontext.h>

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

u64 zones::pages_count() const
{
    u64 ret = 0;
    for (int i = 0; i < count; i++)
    {
        ret += zone_array[i].pages_count();
    }
    return ret;
}

u64 zones::free_pages() const
{
    u64 ret = 0;
    for (int i = 0; i < count; i++)
    {
        ret += zone_array[i].free_pages();
    }
    return ret;
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
            auto ptr = pa2va(p);
#ifdef _DEBUG
            util::memset(ptr, 0xFEFF'FEFF'FEFF'FEFF, size);
#endif
            return ptr;
        }
    }
    trace::panic("Kernel OOM allocate size ", size);
}

void zones::deallocate(void *ptr)
{
    phy_addr_t p = va2pa(ptr);
    zone *z = which(p);
    kassert(z != nullptr, "Not found this zone at ", trace::hex(p()));
    z->free(p);
}

void zones::add_reference(void *ptr)
{
    phy_addr_t p = va2pa(ptr);
    zone *z = which(p);
    kassert(z != nullptr, "Not found this zone at ", trace::hex(p()));
    z->add_reference(p);
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

page *zone::page_end() const { return page_beg() + page_count; }

phy_addr_t zone::alloc(u64 size)
{
    page *p = buddy_impl.alloc((size + memory::page_size - 1) / memory::page_size);
    if (likely(p != nullptr))
    {
        uctx::RawSpinLockUninterruptibleContext ctx(spin);
        p->add_ref_count();
        return page_to_address(p);
    }
    return nullptr;
}

void zone::add_reference(phy_addr_t ptr)
{
    page *p = address_to_page(ptr);
    uctx::RawSpinLockUninterruptibleContext ctx(spin);
    if (p->get_ref_count() > 0)
    {
        p->add_ref_count();
    }
    else
    {
        trace::panic("ref count == 0 at ", trace::hex(ptr.get()));
    }
}

void zone::free(phy_addr_t ptr)
{
    page *p = address_to_page(ptr);
    bool free = false;
    {
        uctx::RawSpinLockUninterruptibleContext ctx(spin);
        if (p->get_ref_count() > 0)
        {
            p->remove_ref_count();
            if (p->get_ref_count() == 0)
            {
                free = true;
            }
        }
        else
        {
            trace::panic("ref count == 0 at ", trace::hex(ptr.get()));
        }
    }
    if (free)
    {
        buddy_impl.free(p);
    }
}

u64 zone::free_pages() const { return buddy_impl.scan_free(); }

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
    kassert((u64)index < page_count, "invalid index");
    auto ptr = start + memory::page_size * index;
    kassert(ptr < va2pa(pages), "invalid offset");
    return ptr;
}

zone::zone(phy_addr_t start, phy_addr_t end, phy_addr_t danger_beg, phy_addr_t danger_end)
    : start(start)
    , end(end)
{
    // pages = (end - start - pages * sizeof(pages)) / page_size
    // pages * page_size + pages * sizeof(pages) = (end - start)
    // pages = (end - start) / (page_size + sizeof(pages))

    page_count = (end - start) / (page_size + sizeof(page));
    page_count--;

    u64 page_bytes = sizeof(page) * page_count;
    phy_addr_t page_addr = align_down(end - page_bytes, page_size);
    allocate_end = start + page_count * page_size;

    pages = memory::pa2va<page *>(end - page_bytes);
    util::memzero(pages, page_bytes);

    if (page_addr >= danger_beg && page_addr < danger_end)
    {
        trace::panic("Zone page/buddy allocate failed at ", trace::hex(start()), "-", trace::hex(end()), ", required ",
                     page_bytes, '.');
    }
    buddy_block_count = buddy_impl.init(this);
}

} // namespace memory