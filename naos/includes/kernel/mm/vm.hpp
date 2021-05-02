#pragma once
#include "../lock.hpp"
#include "../util/skip_list.hpp"
#include "buddy.hpp"
#include "common.hpp"
#include "list_node_cache.hpp"

namespace fs::vfs
{
class file;
} // namespace fs::vfs

/// virtual memory system
namespace memory::vm
{
/// page flags
namespace flags
{
enum flags : u64
{
    readable = 1ul << 0,
    writeable = 1ul << 1,
    executeable = 1ul << 2,
    expand = 1ul << 11,
    user_mode = 1ul << 12,
    lock = 1ul << 13,
    shared = 1ul << 14,
    populate = 1ul << 15,
    file = 1ul << 16,
    huge_page = 1ul << 17,
};
}

void init();
void listen_page_fault();

struct vm_t;

typedef bool (*vm_page_fault_func)(u64 page_addr, const vm_t *vm);
struct vm_t
{
    u64 start;
    u64 end;
    u64 flags;
    vm_page_fault_func handle;
    u64 user_data;
    bool operator==(const vm_t &rhs) const { return rhs.end == end; }
    bool operator<(const vm_t &rhs) const { return end < rhs.end; }
    vm_t(u64 start, u64 end, u64 flags, vm_page_fault_func handle, u64 user_data)
        : start(start)
        , end(end)
        , flags(flags)
        , handle(handle)
        , user_data(user_data){};
};

class vm_allocator
{
  public:
    using list_t = util::skip_list<vm_t>;

  private:
    list_t list;
    lock::rw_lock_t list_lock;
    u64 range_top, range_bottom;

  public:
    vm_allocator(u64 top, u64 bottom)
        : list(memory::KernelCommonAllocatorV, 2, 32)
        , range_top(top)
        , range_bottom(bottom)
    {
    }

    void set_range(u64 top, u64 bottom)
    {
        range_top = top;
        range_bottom = bottom;
    }

    const vm_t *allocate_map(u64 size, u64 flags, vm_page_fault_func handle, u64 user_data);
    void deallocate_map(const vm_t *vm);
    bool deallocate_map(u64 p);

    const vm_t *add_map(u64 start, u64 end, u64 flags, vm_page_fault_func handle, u64 user_data);
    const vm_t *get_vm_area(u64 p);

    list_t &get_list() { return list; }
    lock::rw_lock_t &get_lock() { return list_lock; }
};

class mmu_paging
{
  private:
    void *base_paging_addr;

  public:
    mmu_paging();
    ~mmu_paging();
    void load_paging();

    void map_area(const vm_t *vm);
    void map_area_phy(const vm_t *vm, phy_addr_t start);

    void unmap_area(const vm_t *vm);

    void *get_base_page();

    void *page_map_vir2phy(void *virtual_addr);

    void sync_kernel();
};

/// virtual memory info struct.
///
/// includes head and VMA
struct info_t
{
  public:
    memory::vm::vm_allocator vma;
    memory::vm::mmu_paging mmu_paging;
    const vm_t *head_vm;

  private:
    u64 current_head_ptr;

  public:
    info_t();
    ~info_t();
    info_t(const info_t &) = delete;
    info_t &operator=(const info_t &) = delete;
    void init_brk(u64 start);
    bool set_brk(u64 ptr);
    bool set_brk_now(u64 ptr);
    u64 get_brk();

    const vm_t *map_file(u64 start, fs::vfs::file *file, u64 file_map_offset, u64 map_length, flag_t page_ext_attr);
    bool umap_file(u64 addr);
    void sync_map_file(u64 addr);
};

/// map struct
struct map_t
{
    fs::vfs::file *file;
    u64 offset;
    u64 length;
    info_t *vm_info;
    map_t(fs::vfs::file *f, u64 offset, u64 length, info_t *vmi)
        : file(f)
        , offset(offset)
        , length(length)
        , vm_info(vmi){};
};

bool fill_file_vm(u64 page_addr, const vm_t *item);
bool fill_expand_vm(u64 page_addr, const vm_t *item);

void sync_map_file(u64 addr);

} // namespace memory::vm
