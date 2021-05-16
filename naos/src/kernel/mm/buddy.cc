#include "kernel/mm/buddy.hpp"
#include "kernel/lock.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/mm/page.hpp"
#include "kernel/ucontext.hpp"
#include "kernel/util/bit_set.hpp"
namespace memory
{

u64 buddy::next_fit_size(u64 size)
{
    size |= size >> 1;
    size |= size >> 2;
    size |= size >> 4;
    size |= size >> 8;
    size |= size >> 16;
    return size + 1;
}

u64 offset(u8 order) { return 1UL << order; }

page *buddy::alloc(u64 pages)
{
    if (pages == 0)
        pages = 1;
    else if ((pages & (pages - 1)) != 0) // not pow of 2
        pages = next_fit_size(pages);

    int order = __builtin_ffsl(pages);
    kassert(order <= buddy_max_order, "order too large");

    int avail_order = order;
    while (orders[avail_order] == nullptr && avail_order <= buddy_max_order)
    {
        avail_order++;
    }
    if (avail_order > buddy_max_order)
    {
        return nullptr;
    }

    for (int o = avail_order; o > order && o > 0; o--)
    {
        page *cur = orders[o];
        page *next = cur->get_buddy_next();
        orders[o] = next;
        if (next != nullptr)
        {
            next->set_buddy_prev(nullptr);
        }
        split_buddy(cur);
    }

    page *p = orders[order];
    page *next = p->get_buddy_next();
    orders[order] = next;
    if (next != nullptr)
    {
        next->set_buddy_prev(nullptr);
    }
    // take it
    p->set_buddy_prev(nullptr);
    p->set_buddy_next(nullptr);
    p->set_flag(0);
    return p;
}

void buddy::free(page *p)
{
    u8 order = p->get_buddy_order();
    page *next = orders[order];
    p->set_buddy_next(next);
    p->set_flag(page::buddy_free);
    p->set_buddy_prev(nullptr);
    if (next != nullptr)
    {
        next->set_buddy_prev(p);
    }
    orders[order] = p;
    // merge levels
    merge(order, true);
}

buddy::buddy() {}

page *buddy::split_buddy(page *p)
{
    u8 order = p->get_buddy_order();
    if (order == 0)
    {
        return nullptr;
    }
    page *end = z->page_end();
    order--;
    page *r = p + offset(order);
    remove(p);
    page *next = orders[order];
    p->set_flag(page::buddy_free);
    p->set_buddy_prev(nullptr);
    p->set_buddy_order(order);

    if (r < end)
    {
        r->set_flag(page::buddy_free);
        r->set_buddy_next(next);
        r->set_buddy_prev(p);
        r->set_buddy_order(order);
        if (next != nullptr)
        {
            next->set_buddy_prev(r);
        }
        p->set_buddy_next(r);
    }
     else
    {
        r = nullptr;
        p->set_buddy_next(next);
        if (next != nullptr)
        {
            next->set_buddy_prev(p);
        }
    }
    orders[order] = p;
    return r;
}

page *buddy::get_bro(page *p)
{
    i64 index = p - z->page_beg();
    u64 count = z->pages_count();
    u8 order = p->get_buddy_order();
    u64 off = offset(order);
    if ((index & (offset(order + 1) - 1)))
    {
        // p is right
        i64 lindex = index - off;
        if (likely(lindex >= 0))
        {
            return p - off;
        }
        else
        {
            return nullptr;
        }
    }
   
    else
    {
        // p is left
        i64 rindex = index + off;
        if (likely(rindex < (i64)count))
        {
            return p + off;
        }
        else
        {
            return nullptr;
        }
    }
}

void buddy::remove(page *p)
{
    page *left = p->get_buddy_prev();
    page *right = p->get_buddy_next();
    page *head = right;
    if (left != nullptr)
    {
        left->set_buddy_next(right);
        head = left;
    }
    if (right != nullptr)
    {
        right->set_buddy_prev(left);
    }
    u8 order = p->get_buddy_order();
    if (p == orders[order]) {
        orders[order] = head;
    }
    p->set_buddy_next(nullptr);
    p->set_buddy_prev(nullptr);
    p->set_flag(0);
}

void buddy::merge(u8 order, bool fast)
{
    for (u8 o = order; o < buddy_max_order; o++)
    {
        page *p = orders[o];
        do
        {
            if (p == nullptr)
            {
                break;
            }
            page *n = p->get_buddy_next();
            page *bro = get_bro(p);
            if (bro != nullptr && bro->has_flag(page::buddy_free) && bro->get_buddy_order() == o)
            {
                if (bro < p)
                {
                    // bro -> p
                    std::swap(p, bro);
                }
                if (bro == n)
                {
                    n = bro->get_buddy_next();
                }
                if (p == n)
                {
                    n = p->get_buddy_next();
                }
                remove(p);
                remove(bro);
                p->set_flag(page::buddy_free);
                p->set_buddy_order(o + 1);
                page *next = orders[o + 1];
                p->set_buddy_next(next);
                p->set_buddy_prev(nullptr);
                if (next != nullptr)
                {
                    next->set_buddy_prev(p);
                }
                orders[o + 1] = p;
            }
            p = n;
        } while  (!fast);
    }
}

i64 buddy::init(zone *z)
{
    for (auto i = 0; i <= buddy_max_order; i++)
    {
        orders[i] = nullptr;
    }

    this->z = z;
    page *bstart = z->page_beg();
    page *bend = z->page_end();
    u64 pages = z->pages_count();
    u64 big_pages = (pages / buddy_max_page) * buddy_max_page;

    page *start = bstart;
    page *end = start + big_pages;

    page *prev = nullptr;
    i64 count = 0;
    while (start < end)
    {
        page *cur = start;
        cur->set_flag(page::buddy_free);
        cur->set_buddy_order(buddy_max_order);
        cur->set_buddy_next(nullptr);
        cur->set_buddy_prev(prev);
        if (unlikely(prev == nullptr))
        {
            orders[buddy_max_order] = cur;
        }
        else
        {
            prev->set_buddy_next(cur);
        }
        prev = cur;
        start += buddy_max_page;
        count++;
    }

    // rest pages
    prev = nullptr;
    if (end < bend) {
        count++;
    }

    while (end < bend)
    {
        page *cur = end;
        cur->set_flag(page::buddy_free);
        cur->set_buddy_order(0);
        cur->set_buddy_next(nullptr);
        cur->set_buddy_prev(prev);
        if (unlikely(prev == nullptr))
        {
            auto next = orders[0];
            cur->set_buddy_next(next);
            orders[0] = cur;
        }
        else
        {
            prev->set_buddy_next(cur);
        }
        prev = cur;
        end++;
    }
    merge(0, false);

    return count;
}

/*
Tag memory used by kernel.
*/
bool buddy::mark_unavailable(page *s, page *e)
{
    for (i16 o = buddy_max_order; o >= 0; o--)
    {
        page *p = orders[o];
        while (p != nullptr)
        {
            page *next = p->get_buddy_next();
            page *pleft = p;
            page *pright = pleft + offset(o);
            page *left, *right;
            if (pleft > s)
            {
                left = pleft;
            }
            else
            {
                left = s;
            }
            if (pright < e)
            {
                right = pright;
            }
            else
            {
                right = e;
            }

            if (left < right)
            {
                // not emtpy
                if (left == pleft && right == pright)
                {
                    remove(p);
                    p->set_flag(page::buddy_unavailable);
                }
                else
                {
                    // split
                    split_buddy(p);
                }
            }
            p = next;
        }
    }
    return true;
}

} // namespace memory
