#include "kernel/mm/slab.hpp"

slab_cache_pool *global_kmalloc_slab_cache_pool, *global_dma_slab_cache_pool, *global_object_slab_cache_pool;
void slab_group::new_memory_node()
{
    memory::BuddyAllocator allocator(memory::zone_t::prop::present);
    slab *s = (slab *)allocator.allocate(page_pre_slab * memory::page_size, 8);
    s = new (s) slab(node_pre_slab, 0);

    s->data_ptr = (char *)s + sizeof(slab);
    s->data_ptr = (char *)(((u64)s->data_ptr + align - 1) & ~(align - 1));
    s->bitmap.clean_all();
    list_empty.push_back(s);
    all_obj_count += node_pre_slab;
}
void slab_group::delete_memory_node(slab *s)
{
    kassert(s->rest == node_pre_slab, "slab error");
    all_obj_count -= node_pre_slab;
    s->~slab();
    memory::BuddyAllocator allocator(memory::zone_t::prop::present);
    allocator.deallocate(s);
}

slab_group::slab_group(memory::IAllocator *allocator, u64 size, const char *name, u64 align, u64 flags)
    : obj_align_size((size + align - 1) & ~(align - 1))
    , size(size)
    , name(name)
    , list_empty(allocator)
    , list_partial(allocator)
    , list_full(allocator)
    , flags(flags)
    , align(align)
    , all_obj_count(0)
    , all_obj_used(0)
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
                    node_pre_slab = restsize / obj_align_size + memory::page_size * 15 / obj_align_size;
                }
                else
                {
                    page_pre_slab = 8;
                    node_pre_slab = restsize / obj_align_size + memory::page_size * 7 / obj_align_size;
                }
            }
            else
            {
                page_pre_slab = 4;
                node_pre_slab = restsize / obj_align_size + memory::page_size * 2 / obj_align_size;
            }
        }
        else
        {
            // 63 - 251
            page_pre_slab = 2;
            node_pre_slab = restsize / obj_align_size + memory::page_size / obj_align_size;
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
    if (list_partial.empty())
    {
        if (list_empty.empty())
        {
            new_memory_node();
        }
        list_partial.push_back(list_empty.pop_back());
    }
    slab *slab = list_partial.back();
    auto &bitmap = slab->bitmap;
    for (int i = 0; i < bitmap.count(); i++)
    {
        if (bitmap.get(i) == 0)
        {
            slab->rest--;
            all_obj_used++;
            bitmap.set(i, 1);
            if (slab->rest == 0)
            {
                list_full.push_back(list_partial.pop_back());
            }
            return slab->data_ptr + i * obj_align_size;
        }
    }
    trace::panic("Slab group should not panic here!");
}
void slab_group::free(void *ptr)
{
    char *page_addr = (char *)((u64)ptr & ~(memory::page_size - 1));

    for (auto it = list_partial.begin(); it != list_partial.end(); it = list_partial.next(it))
    {
        if (page_addr == (char *)it->element)
        {
            it->element->bitmap.set(((char *)ptr - it->element->data_ptr) / obj_align_size, 0);
            it->element->rest++;
            all_obj_used--;
            if (it->element->rest == node_pre_slab)
            {
                list_empty.push_back(it->element);
                list_partial.remove(it);

                if (all_obj_used * 2 < all_obj_count)
                {
                    delete_memory_node(it->element);
                    list_empty.pop_back();
                }
            }

            return;
        }
    }
    for (auto it = list_full.begin(); it != list_full.end(); it = list_full.next(it))
    {
        if (page_addr == (char *)it->element)
        {
            it->element->bitmap.set(((char *)ptr - it->element->data_ptr) / obj_align_size, 0);
            it->element->rest++;
            all_obj_used--;
            list_partial.push_back(it->element);
            list_full.remove(it);
            return;
        }
    }
}
bool slab_group::include_address(void *ptr)
{
    char *page_addr = (char *)((u64)ptr & ~(memory::page_size - 1));
    for (auto it = list_full.begin(); it != list_full.end(); it = list_full.next(it))
    {
        if (page_addr == (char *)it->element)
            return true;
    }
    for (auto it = list_partial.begin(); it != list_partial.end(); it = list_partial.next(it))
    {
        if (page_addr == (char *)it->element)
            return true;
    }
    for (auto it = list_empty.begin(); it != list_empty.end(); it = list_empty.next(it))
    {
        if (page_addr == (char *)it->element)
            return true;
    }
    return false;
}
// slab pool

slab_group_list::list_node *slab_cache_pool::find_slab_group_node(const char *name)
{
    auto group = slab_groups.begin();
    while (group != slab_groups.end())
    {
        if (util::strcmp(group->element.get_name(), name) == 0)
        {
            return group;
        }
        group = group->next;
    }
    return nullptr;
}

slab_group *slab_cache_pool::find_slab_group(const char *name)
{
    auto group = slab_groups.begin();
    while (group != slab_groups.end())
    {
        if (util::strcmp(group->element.get_name(), name) == 0)
        {
            return &group->element;
        }
        group = group->next;
    }
    return nullptr;
}
slab_group *slab_cache_pool::find_slab_group(u64 size)
{
    auto group = slab_groups.begin();
    while (group != slab_groups.end())
    {
        if (group->element.get_size() == size)
        {
            return &group->element;
        }
        group = group->next;
    }
    return nullptr;
}
void slab_cache_pool::create_new_slab_group(u64 size, const char *name, u64 align, u64 flags)
{
    slab_group group(&slab_node_list_node_allocator, size, name, align, flags);
    slab_groups.push_back(group);
}
void slab_cache_pool::remove_slab_group(const char *name)
{
    auto group_node = find_slab_group_node(name);
    if (group_node)
    {
        slab_groups.remove(group_node);
    }
}
slab_cache_pool::slab_cache_pool()
    : buddyAllocator(memory::zone_t::prop::present)
    , slab_node_list_node_allocator(&slab_list_node_cache)
    , slab_group_list_node_allocator(&slab_group_list_node_cache)
    , slab_list_node_cache(&buddyAllocator)
    , slab_group_list_node_cache(&buddyAllocator)
    , slab_groups(&slab_group_list_node_allocator)
{
}