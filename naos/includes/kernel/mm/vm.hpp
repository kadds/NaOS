#pragma once
#include "buddy.hpp"
#include "common.hpp"
#include "kernel/util/linked_list.hpp"
#include "list_node_cache.hpp"

namespace memory::vm
{
namespace flags
{
enum flags : u64
{
    readable = 1ul << 0,
    writeable = 1ul << 1,
    mapped = 1ul << 2,
    io = 1ul << 3,
    exec = 1ul << 4,
    execable = 1ul << 5,
    expand_high = 1ul << 6,
    expand_low = 1ul << 7,
    user_mode = 1ul << 8
};
}
void init();

struct vm_t
{
    void *start;
    void *end;
    u64 flags;
    void *back_file;
    vm_t(void *start, void *end, u64 flags, void *back_file)
        : start(start)
        , end(end)
        , flags(flags)
        , back_file(back_file){};
};
class mmu_paging
{
  private:
    void *base_paging_addr;

    typedef util::linked_list<vm_t> list_t;
    typedef list_node_cache<list_t> list_node_cache_t;
    typedef memory::ListNodeCacheAllocator<list_node_cache_t> list_node_allocator_t;

    list_node_cache_t node_cache;

    list_node_allocator_t node_cache_allocator;
    util::linked_list<vm_t> list;

  public:
    mmu_paging();
    ~mmu_paging();

    void load_paging();
    const vm_t *add_vm_area(void *start, void *end, u64 flags);
    u64 remove_vm_area(void *start, void *end);
    const vm_t *get_vm_area(void *p);
    void map_area(const vm_t *vm);
    void map_area_phy(const vm_t *vm, void *phy_address_start);

    void map_page(void *p);

    void unmap_area(const vm_t *vm);
};
} // namespace memory::vm
