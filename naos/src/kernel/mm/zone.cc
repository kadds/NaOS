#include "kernel/mm/zone.hpp"
#include "common.hpp"
#include "freelibcxx/buddy.hpp"
#include "kernel/arch/mm.hpp"
#include "kernel/clock.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/mm/page.hpp"
#include "kernel/trace.hpp"
#include "kernel/ucontext.hpp"

namespace memory
{

struct buddy_operator
{
  public:
    buddy_operator(page *ptr)
        : ptr_(ptr)
    {
    }
    void set(u32 index, u32 prev, u32 next, int order, bool used, bool merged)
    {
        ptr_[index].set_buddy_prev(prev);
        ptr_[index].set_buddy_next(next);
        ptr_[index].set_buddy_order(order);
        auto flags = ptr_[index].get_flags();
        flags &= ~(page::buddy_merged | page::buddy_used);
        if (used)
        {
            flags |= page::buddy_used;
        }
        if (merged)
        {
            flags |= page::buddy_merged;
        }
        ptr_[index].set_flags(flags);
    }

    void set_next(u32 index, u32 next) { ptr_[index].set_buddy_next(next); }
    void set_prev(u32 index, u32 prev) { ptr_[index].set_buddy_prev(prev); }
    void set_order(u32 index, int order) { ptr_[index].set_buddy_order(order); }
    void set_merged(u32 index) { ptr_[index].add_flags(page::buddy_merged); }
    void clear_merged(u32 index) { ptr_[index].remove_flags(page::buddy_merged); }
    void set_used(u32 index) { ptr_[index].add_flags(page::buddy_used); }
    void clear_used(u32 index) { ptr_[index].remove_flags(page::buddy_used); }

    u32 get_next(u32 index) const { return ptr_[index].get_buddy_next(); }
    u32 get_prev(u32 index) const { return ptr_[index].get_buddy_prev(); }
    int get_order(u32 index) const { return ptr_[index].get_buddy_order(); }
    bool get_merged(u32 index) const { return ptr_[index].has_flags(page::buddy_merged); }
    bool get_used(u32 index) const { return ptr_[index].has_flags(page::buddy_used); }

    freelibcxx::tuple<u32, u32, int, bool, bool> get(size_t index) const
    {
        auto &p = ptr_[index];
        return freelibcxx::make_tuple(p.get_buddy_prev(), p.get_buddy_next(), p.get_buddy_order(),
                                      p.has_flags(page::buddy_used), p.has_flags(page::buddy_merged));
    }

  private:
    page *ptr_;
};

using buddy_t = freelibcxx::buddy<buddy_operator, 11, u32>;
static_assert(sizeof(buddy_t) == 64);

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

u32 zones::get_page_reference(void *ptr)
{
    auto page = get_page(ptr);
    if (page != nullptr)
    {
        return page->get_ref_count();
    }
    return 0;
}

u64 zones::total_pages() const
{
    u64 ret = 0;
    for (int i = 0; i < count_; i++)
    {
        ret += zone_array_[i].total_pages();
    }
    return ret;
}

u64 zones::free_pages() const
{
    u64 ret = 0;
    for (int i = 0; i < count_; i++)
    {
        ret += zone_array_[i].free_pages();
    }
    return ret;
}

zone *zones::which(phy_addr_t ptr)
{
    int i = 0, j = count_;
    while (i < j)
    {
        int mid = i + (j - i) / 2;
        phy_addr_t start = zone_array_[mid].range_beg();
        phy_addr_t end = zone_array_[mid].range_end();
        if (ptr >= start && ptr < end)
        {
            return &zone_array_[mid];
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

void *zones::allocate(size_t size, size_t align) noexcept
{
    // ignore align
    size = align_up(size, memory::page_size);
    u64 pages = size / memory::page_size;
    // select one zone
    for (int i = 0; i < count_; i++)
    {
        phy_addr_t p = zone_array_[i].malloc(pages);
        if (p != nullptr)
        {
            auto ptr = pa2va(p);
#ifdef _DEBUG
            memset(ptr, 0xFAFF'FEF0, size);
#endif
            // if (timeclock::get_current_clock() - last_report_ > 1000'000)
            // {
            //     last_report_ = timeclock::get_current_clock();
            //     trace::info("free ", free_pages(), "/", total_pages());
            // }
            return ptr;
        }
    }
    trace::panic("Kernel OOM allocate pages ", pages);
}

void zones::deallocate(void *ptr) noexcept
{
    phy_addr_t p = va2pa(ptr);
    zone *z = which(p);
    kassert(z != nullptr, "Not found this zone at ", trace::hex(p()));
    z->free(p);
}

void zones::page_add_reference(void *ptr)
{
    phy_addr_t p = va2pa(ptr);
    zone *z = which(p);
    kassert(z != nullptr, "Not found this zone at ", trace::hex(p()));
    z->page_add_reference(p);
}

void zones::tag_alloc(phy_addr_t start, phy_addr_t end)
{
    kassert(start <= end, "address check fail");

    for (int i = 0; i < count_; i++)
    {
        auto &z = zone_array_[i];

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
    auto s = address_to_page(start) - page_array;
    auto e = address_to_page(end) - page_array;
    auto impl = reinterpret_cast<buddy_t *>(impl_ptr_);
    impl->alloc_at(s, e - s);
}

page *zone::page_end() const { return page_beg() + page_count; }

phy_addr_t zone::malloc(u64 pages)
{
    auto impl = reinterpret_cast<buddy_t *>(impl_ptr_);
    auto index = impl->alloc(pages);
    if (likely(index.has_value()))
    {
        uctx::RawSpinLockUninterruptibleContext ctx(spin);
        page *p = page_array + index.value();
        if (p->get_ref_count() != 0)
        {
            trace::panic("ref count == 0 malloc ", pages);
        }
        p->add_ref_count();
        return page_to_address(p);
    }
    return nullptr;
}

void zone::page_add_reference(phy_addr_t ptr)
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
        auto impl = reinterpret_cast<buddy_t *>(impl_ptr_);
        [[maybe_unused]] bool ok = impl->free(p - page_array);
        kassert(ok, "fail ", p - page_array);
    }
}

u64 zone::free_pages() const
{
    auto impl = reinterpret_cast<const buddy_t *>(impl_ptr_);
    return impl->free_pages();
}

page *zone::address_to_page(phy_addr_t ptr) const
{
    i64 off = ptr - start;
    kassert(off >= 0, "address is not this zone");
    kassert(off % memory::page_size == 0, "pointer is not a alignment pointer");
    i64 page_index = off / memory::page_size;
    return page_array + page_index;
}

phy_addr_t zone::page_to_address(page *p) const
{
    i64 index = p - page_array;
    if (unlikely(index < 0))
    {
        return nullptr;
    }
    kassert((u64)index < page_count, "invalid index");
    auto ptr = start + memory::page_size * index;
    kassert(ptr < va2pa(page_array), "invalid offset");
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

    page_array = memory::pa2va<page *>(end - page_bytes);
    memset(page_array, 0, page_bytes);

    if (page_addr >= danger_beg && page_addr < danger_end)
    {
        trace::panic("Zone page/buddy allocate failed at ", trace::hex(start()), "-", trace::hex(end()), ", required ",
                     page_bytes, '.');
    }

    impl = new (impl_ptr_) buddy_t(page_count, buddy_operator(page_array));
}

} // namespace memory