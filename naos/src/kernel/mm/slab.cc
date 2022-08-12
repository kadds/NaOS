#include "kernel/mm/slab.hpp"
#include "freelibcxx/string.hpp"
#include "kernel/lock.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/mm/page.hpp"
#include "kernel/trace.hpp"
#include "kernel/ucontext.hpp"

namespace memory
{

slab_cache_pool *global_kmalloc_slab_domain, *global_dma_slab_domain, *global_object_slab_domain;
slab *slab_group::new_memory_node()
{
    slab *s = (slab *)memory::KernelBuddyAllocatorV->allocate(page_pre_slab * memory::page_size, 8);
    new (s) slab(node_pre_slab, 0);

    s->data_ptr = (char *)s + sizeof(slab);
    s->data_ptr = (char *)(((u64)s->data_ptr + align - 1) & ~(align - 1));
    s->bitmap.reset_all();
    page *p = memory::global_zones->get_page(s);
    for(u32 i = 0; i < page_pre_slab; i++, p++) {
        p->set_ref_slab(this);
    }

    all_obj_count += node_pre_slab;
    return s;
}

void slab_group::delete_memory_node(slab *s)
{
    kassert(s->rest == node_pre_slab, "slab error rest:", s->rest, " target:", node_pre_slab);
    all_obj_count -= node_pre_slab;
    s->~slab();
    // page *p = memory::global_zones->get_page(s);
    // for(int i = 0; i < page_pre_slab; i++, p++) {
    //     p->set_ref_slab(nullptr);
    // }
    memory::KernelBuddyAllocatorV->deallocate(s);
}

slab_group::slab_group(u64 size, const char *name, u64 align, u64 flags)
    : obj_align_size((size + align - 1) & ~(align - 1))
    , size(size)
    , name(freelibcxx::const_string_view(name).to_string(memory::KernelCommonAllocatorV))
    , flags(flags)
    , align(align)
    , all_obj_count(0)
    , all_obj_used(0)
    , free_head(nullptr)
    , used_head(nullptr)
{
    u64 restsize = memory::page_size - ((sizeof(slab) + align - 1) & ~(align - 1));

    if (obj_align_size * 64 > restsize)
    {
        if (obj_align_size * 16 > restsize)
        {
            if (obj_align_size * 4 > restsize)
            {
                if (obj_align_size > restsize)
                {
                    page_pre_slab = 16;
                    node_pre_slab = (restsize + memory::page_size * 15) / obj_align_size;
                }
                else
                {
                    page_pre_slab = 8;
                    node_pre_slab = (restsize + memory::page_size * 7) / obj_align_size;
                }
            }
            else
            {
                page_pre_slab = 4;
                node_pre_slab = (restsize + memory::page_size * 3) / obj_align_size;
            }
        }
        else
        {
            // 63 - 251
            page_pre_slab = 2;
            node_pre_slab = (restsize + memory::page_size) / obj_align_size;
        }
    }
    else
    {
        // first page can save element >= 64
        page_pre_slab = 1;
        node_pre_slab = restsize / obj_align_size;
    }
}

void *slab_group::alloc()
{
    uctx::RawWriteLockUninterruptibleContext ctx(slab_lock);
    if (free_head == nullptr)
    {
        free_head = new_memory_node();
    }
    slab *slab = free_head;
    auto &bitmap = slab->bitmap;
    u64 i = bitmap.scan_zero();
    kassert(i < bitmap.count() && i < node_pre_slab, "Memory corruption in slab");
    bitmap.set_bit(i);
    kassert(bitmap.get_bit(i), "Can't allocate an address. index: ", i);

    slab->rest--;
    all_obj_used++;

    if (slab->rest == 0)
    {
        free_head = free_head->next;
        if (used_head == nullptr)
        {
            slab->next = nullptr;
            slab->prev = nullptr;
        }
        else
        {
            slab->prev = nullptr;
            slab->next = used_head;
            used_head->prev = slab;
        }
        used_head = slab;
        if (free_head)
        {
            free_head->prev = nullptr;
        }
    }
    return slab->data_ptr + i * obj_align_size;
}

void slab_group::free(void *ptr)
{
    uctx::RawWriteLockUninterruptibleContext ctx(slab_lock);

    byte *page_addr = reinterpret_cast<byte*>(reinterpret_cast<u64>(ptr) & ~(memory::page_size * page_pre_slab - 1));
    slab *s = reinterpret_cast<slab *>(page_addr);
    u64 index = ((char *)ptr - s->data_ptr) / obj_align_size;
    kassert(index < s->bitmap.count(), "s->data_ptr=", trace::hex(s->data_ptr));
    kassert(s->bitmap.get_bit(index), "Not an assigned address or double free.");
    s->bitmap.reset_bit(index);
    s->rest++;
    all_obj_used--;
    if (s->rest == 1)
    {
        slab *p = s->prev;
        slab *n = s->next;
        if (n != nullptr)
        {
            n->prev = p;
        }
        if (p == nullptr)
        {
            used_head = n;
        }
        else
        {
            p->next = n;
        }
        s->prev = nullptr;
        s->next = free_head;
        if (free_head)
        {
            free_head->prev = s;
        }
    }
}

slab_group *slab_group::get_group_from(void *ptr) {
    byte *page_addr = reinterpret_cast<byte*>(reinterpret_cast<u64>(ptr) & ~(memory::page_size - 1));
    page *p = memory::global_zones->get_page(page_addr);
    if (likely(p != nullptr)) {
        kassert(!p->has_flag(page::buddy_free), "invalid buddy state, maybe double free");
        return p->get_ref_slab();
    }
    return nullptr;
}

slab_group *slab_cache_pool::find_slab_group(const freelibcxx::string &name)
{
    uctx::RawReadLockUninterruptibleContext ctx(group_lock);
    return map.get(name).value_or(nullptr);
}

slab_group *slab_cache_pool::create_new_slab_group(u64 size, freelibcxx::string name, u64 align, u64 flags)
{
    uctx::RawWriteLockUninterruptibleContext ctx(group_lock);
    slab_group *group = memory::New<slab_group>(memory::KernelCommonAllocatorV, size, name.data(), align, flags);
    map.insert(std::move(name), group);
    return group;
}

void slab_cache_pool::remove_slab_group(slab_group *slab_obj)
{
    uctx::RawWriteLockUninterruptibleContext ctx(group_lock);
    map.remove(slab_obj->get_name());
}

slab_cache_pool::slab_cache_pool()
    : map(memory::KernelCommonAllocatorV)
{
}

void *SlabObjectAllocator::allocate(u64 size, u64 align) noexcept
{
    if (unlikely(size > slab_obj->get_size()))
        trace::panic("slab allocator can not alloc a larger size");

    auto ptr = slab_obj->alloc();
#ifdef _DEBUG
    if (ptr != nullptr)
    {
        memset(ptr, 0xFFFF'FFFF, size);
    }
#endif
    return ptr;
};

void SlabObjectAllocator::deallocate(void *ptr) noexcept
{
#ifdef _DEBUG
    if (ptr != nullptr)
    {
        memset(ptr, 0xFFFF'FFFF, slab_obj->get_size());
    }
#endif
    slab_obj->free(ptr);
};

} // namespace memory