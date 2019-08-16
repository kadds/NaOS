#pragma once
#include "../util/bit_set.hpp"
#include "../util/linked_list.hpp"
#include "../util/str.hpp"
#include "buddy.hpp"
#include "common.hpp"
#include "list_node_cache.hpp"
struct slab
{
    using bitmap_t = util::bit_set_fixed<u64, 1, 512>;
    bitmap_t bitmap; // max 512 element
    u32 rest;
    u32 color_offset;
    char *data_ptr;

    slab(slab &) = delete;
    slab(const slab &) = delete;
    slab &operator=(const slab &) = delete;

    slab(u32 element_count, u32 color)
        : rest(element_count)
        , color_offset(color){

          };
};
typedef util::linked_list<slab *> slab_list;

class slab_group
{
  private:
    const u64 obj_align_size;
    const u64 size;
    const char *name;
    slab_list list_empty, list_partial, list_full;
    u32 color_offset;
    u64 flags;
    u64 align;
    u32 node_pre_slab;
    u32 page_pre_slab;
    u64 all_obj_count;
    u64 all_obj_used;

  private:
    void new_memory_node();
    void delete_memory_node(slab *s);

  public:
    slab_group(memory::IAllocator *allocator, u64 size, const char *name, u64 align, u64 flags);

    const char *get_name() const { return name; }
    u64 get_size() const { return size; }
    void *alloc();
    void free(void *ptr);
    bool include_address(void *ptr);
    int shrink();
};

typedef util::linked_list<slab_group> slab_group_list;

struct slab_cache_pool
{
  private:
    memory::BuddyAllocator buddyAllocator;
    memory::ListNodeCacheAllocator<list_node_cache<slab_list>> slab_node_list_node_allocator;
    memory::ListNodeCacheAllocator<list_node_cache<slab_group_list>> slab_group_list_node_allocator;

    list_node_cache<slab_list> slab_list_node_cache;
    list_node_cache<slab_group_list> slab_group_list_node_cache;

    slab_group_list slab_groups;
    slab_group_list::list_node *find_slab_group_node(const char *name);

  public:
    slab_group *find_slab_group(const char *name);
    slab_group *find_slab_group(u64 size);
    slab_group_list *get_slab_groups() { return &slab_groups; }
    void create_new_slab_group(u64 size, const char *name, u64 align, u64 flags);

    void remove_slabe_group(const char *name);
    slab_cache_pool();
};

extern slab_cache_pool *global_kmalloc_slab_cache_pool, *global_dma_slab_cache_pool, *global_object_slab_cache_pool;

namespace memory
{
struct SlabObjectAllocator : IAllocator
{
  private:
    slab_cache_pool *pool;
    const char *name;

  public:
    SlabObjectAllocator(slab_cache_pool *pool, const char *name)
        : pool(pool)
        , name(name){};
    void *allocate(u64 size, u64 align) override
    {
        auto group = pool->find_slab_group(name);
        if (size > group->get_size())
            trace::panic("slab allocator can not alloc a larger size");
        return group->alloc();
    };
    void deallocate(void *ptr) override
    {
        auto group = pool->find_slab_group(name);
        group->free(ptr);
    };
};
struct SlabSizeAllocator : IAllocator
{
  private:
    slab_cache_pool *pool;

  public:
    SlabSizeAllocator(slab_cache_pool *pool)
        : pool(pool){

          };
    void *allocate(u64 size, u64 align) override
    {
        auto groups = pool->get_slab_groups();
        if (groups->empty())
            return nullptr;
        slab_group *last = nullptr;
        for (auto it = groups->begin(); it != groups->end(); it = groups->next(it))
        {
            if (it->element.get_size() >= size)
            {
                if (last == nullptr)
                    last = &it->element;
                else if (it->element.get_size() < last->get_size())
                    last = &it->element;
            }
        }
        if (last != nullptr)
            return last->alloc();
        return nullptr;
    };
    void deallocate(void *ptr) override
    {
        auto groups = pool->get_slab_groups();
        for (auto it = groups->begin(); it != groups->end(); it = groups->next(it))
        {
            if (it->element.include_address(ptr))
            {
                it->element.free(ptr);
                return;
            }
        }
    };
};
} // namespace memory
