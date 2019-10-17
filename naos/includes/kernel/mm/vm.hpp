#pragma once
#include "buddy.hpp"
#include "common.hpp"
#include "kernel/lock.hpp"
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
void listen_page_fault();

struct vm_t
{
    u64 start;
    u64 end;
    u64 flags;
    void *back_file;
    vm_t(u64 start, u64 end, u64 flags, void *back_file)
        : start(start)
        , end(end)
        , flags(flags)
        , back_file(back_file){};
};

class vm_allocator
{
  public:
    using list_t = util::linked_list<vm_t>;
    using list_node_cache_allocator_t = memory::list_node_cache_allocator<list_t>;
    static list_node_cache_allocator_t *allocator;

  private:
    list_t list;
    lock::spinlock_t list_lock;
    u64 range_top, range_bottom;

  public:
    vm_allocator(u64 top, u64 bottom)
        : list(allocator)
        , range_top(top)
        , range_bottom(bottom)
    {
    }

    void set_range(u64 top, u64 bottom)
    {
        range_top = top;
        range_bottom = bottom;
    }

    const vm_t *allocate_map(u64 size, u64 flags);
    vm_t deallocate_map(void *ptr);

    const vm_t *add_map(u64 start, u64 size, u64 flags);
    const vm_t *get_vm_area(u64 p);

    list_t &get_list() { return list; }
    lock::spinlock_t &get_lock() { return list_lock; }
};

class mmu_paging
{
  private:
    void *base_paging_addr;
    vm_allocator vma;

  public:
    mmu_paging();
    ~mmu_paging();
    void clean_user_mmap();
    void load_paging();

    void map_area(const vm_t *vm);
    void map_area_phy(const vm_t *vm, void *phy_address_start);
    void expand_area(const vm_t *vm, void *pointer);

    void unmap_area(const vm_t *vm);

    vm_allocator &get_vma() { return vma; }
};
} // namespace memory::vm
