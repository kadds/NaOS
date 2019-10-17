#pragma once
#include "../util/bit_set.hpp"
#include "../util/linked_list.hpp"
#include "buddy.hpp"
#include "common.hpp"
#include "list_node_cache.hpp"

#define NewSlabGroup(domain, struct, align, flags) domain->create_new_slab_group(sizeof(struct), #struct, align, flags)

namespace memory
{

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
using slab_list_t = util::linked_list<slab *>;
using slab_list_node_allocator_t = memory::list_node_cache_allocator<slab_list_t>;
/// A same slab list set
class slab_group
{
  private:
    lock::spinlock_t slab_lock;
    const u64 obj_align_size;
    const u64 size;
    const char *name;
    slab_list_t list_empty, list_partial, list_full;
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

using slab_group_list_t = util::linked_list<slab_group>;
using slab_group_list_node_allocator = memory::list_node_cache_allocator<slab_group_list_t>;

/// The slab_group collection of the specified domain
struct slab_cache_pool
{
  private:
    lock::spinlock_t group_lock;

    slab_group_list_t slab_groups;
    slab_list_node_allocator_t slab_list_node_allocator;
    slab_group_list_t::iterator find_slab_group_node(const char *name);

  public:
    slab_group *find_slab_group(const char *name);
    slab_group *find_slab_group(u64 size);

    slab_group_list_t &get_slab_groups() { return slab_groups; }

    slab_group *create_new_slab_group(u64 size, const char *name, u64 align, u64 flags);
    void remove_slab_group(slab_group *group);

    slab_cache_pool();
};

extern slab_cache_pool *global_kmalloc_slab_domain;
extern slab_cache_pool *global_dma_slab_domain;
extern slab_cache_pool *global_object_slab_domain;

// A object allocator,
struct SlabObjectAllocator : IAllocator
{
  private:
    slab_group *slab_obj;

  public:
    SlabObjectAllocator(slab_group *obj)
        : slab_obj(obj){};

    void *allocate(u64 size, u64 align) override;
    void deallocate(void *ptr) override;
};

// A allocator get memory for slab
struct SlabSizeAllocator : IAllocator
{
  private:
    slab_cache_pool *pool;

  public:
    SlabSizeAllocator(slab_cache_pool *pool)
        : pool(pool){

          };
    void *allocate(u64 size, u64 align) override;

    void deallocate(void *ptr) override;
};
} // namespace memory
