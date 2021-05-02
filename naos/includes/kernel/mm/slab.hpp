#pragma once
#include "../util/bit_set.hpp"
#include "../util/hash_map.hpp"
#include "../util/linked_list.hpp"
#include "../util/str.hpp"
#include "buddy.hpp"
#include "common.hpp"
#include "list_node_cache.hpp"

#define NewSlabGroup(domain, struct, align, flags)                                                                     \
    domain->create_new_slab_group(sizeof(struct), util::string(memory::KernelCommonAllocatorV, #struct), align, flags)

namespace memory
{
struct slab
{
    using bitmap_t = util::bit_set_inplace<512>;
    bitmap_t bitmap; // max 512 element
    u32 rest;
    u32 color_offset;
    slab *prev;
    slab *next;
    char *data_ptr;

    slab(slab &) = delete;
    slab(const slab &) = delete;
    slab(slab &&) = delete;
    slab &operator=(const slab &) = delete;
    slab &operator=(slab &&) = delete;

    slab(u32 element_count, u32 color)
        : rest(element_count)
        , color_offset(color)
        , prev(nullptr)
        , next(nullptr){};
};

/// A same slab list set
class slab_group
{
  private:
    lock::rw_lock_t slab_lock;
    const u64 obj_align_size;
    const u64 size;
    util::string name;
    u32 color_offset;
    u64 flags;
    u64 align;
    u32 node_pre_slab;
    u32 page_pre_slab;
    u64 all_obj_count;
    u64 all_obj_used;
    slab *free_head, *used_head;

  private:
    slab *new_memory_node();
    void delete_memory_node(slab *s);

  public:
    slab_group(u64 size, const char *name, u64 align, u64 flags);

    const util::string get_name() const { return name; }
    u64 get_size() const { return size; }
    void *alloc();
    void free(void *ptr);
    int shrink();
    static slab_group *get_group_from(void *ptr);
};

/// The slab_group collection of the specified domain
struct slab_cache_pool
{
  private:
    lock::rw_lock_t group_lock;
    util::hash_map<util::string, slab_group *> map;

  public:
    slab_group *find_slab_group(const util::string &name);

    slab_group *create_new_slab_group(u64 size, util::string name, u64 align, u64 flags);
    void remove_slab_group(slab_group *group);

    slab_cache_pool();
};

extern slab_cache_pool *global_kmalloc_slab_domain;
extern slab_cache_pool *global_dma_slab_domain;
extern slab_cache_pool *global_object_slab_domain;

/// An object allocator
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

} // namespace memory
