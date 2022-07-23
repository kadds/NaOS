#pragma once
#include "../lock.hpp"
#include "../util/skip_list.hpp"
#include "buddy.hpp"
#include "common.hpp"
#include "kernel/mm/allocator.hpp"
#include "kernel/mutex.hpp"
#include "kernel/types.hpp"
#include "kernel/util/array.hpp"
#include "kernel/util/hash_map.hpp"
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
    disable_cache = 1ul << 4,
    expand = 1ul << 11,
    user_mode = 1ul << 12,
    lock = 1ul << 13,
    cow = 1ul << 14,
    populate = 1ul << 15,
    file = 1ul << 16,
    big_page = 1ul << 17,
    huge_page = 1ul << 18,
};
}

void init();
void listen_page_fault();

struct vm_t;
class vm_allocator;

typedef bool (*vm_page_fault_func)(vm_allocator &vma, u64 page_addr, vm_t *vm);
struct vm_t
{
    u64 start;
    u64 end;
    u64 flags;
    vm_page_fault_func handle;
    u64 user_data;
    u64 alloc_times = 0;
    bool operator==(const vm_t &rhs) const { return rhs.start == start && rhs.end == end; }
    bool operator<(const vm_t &rhs) const { return start < rhs.start; }
    vm_t(u64 start, u64 end, u64 flags, vm_page_fault_func handle, u64 user_data)
        : start(start)
        , end(end)
        , flags(flags)
        , handle(handle)
        , user_data(user_data){};
    vm_t(u64 start)
        : start(start)
        , end(start)
        , flags(0)
        , handle(nullptr)
        , user_data(0){};
};

class info_t;

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

    vm_t *get_vm_area(u64 p);

    list_t &get_list() { return list; }
    lock::rw_lock_t &get_lock() { return list_lock; }
    void clone(info_t *info, vm_allocator &to, flag_t flag);

  private:
};

class mmu_paging
{
  private:
    void *base_paging_addr;

  public:
    mmu_paging();
    ~mmu_paging();
    void load_paging();

    void map_area(const vm_t *vm, bool override);
    void map_area_phy(const vm_t *vm, phy_addr_t start, bool override);

    void unmap_area(const vm_t *vm, bool free);

    void *get_base_page();

    void *page_map_vir2phy(void *virtual_addr);

    bool virtual_address_mapped(void *addr);

    void sync_kernel();

    void clone(mmu_paging &to, int deep);
    int remapping(const vm_t *vm, void *vir, phy_addr_t phy);

    bool has_shared(void *vir);
    bool clear_shared(void *vir, bool writeable);
};

struct shared_info_t
{
    lock::spinlock_t spin_lock;
    util::hash_set<process_id> shared_pid;
    shared_info_t(memory::IAllocator *allocator)
        : shared_pid(allocator)
    {
    }
};

/// virtual memory info struct.
///
/// includes head and VMA
class info_t
{
  public:
    memory::vm::vm_allocator vma;
    memory::vm::mmu_paging mmu_paging;
    const vm_t *head_vm;
    shared_info_t *shared_info;
    lock::spinlock_t shared_info_lock;

  private:
    u64 current_head_ptr;

  public:
    info_t();
    ~info_t();
    info_t(const info_t &) = delete;
    info_t &operator=(const info_t &) = delete;
    bool init_brk(u64 start);
    bool set_brk(u64 ptr);
    bool set_brk_now(u64 ptr);
    u64 get_brk();

    const vm_t *map_file(u64 start, fs::vfs::file *file, u64 file_offset, u64 file_length, u64 mmap_length,
                         flag_t page_ext_attr);
    bool umap_file(u64 addr, u64 size);
    void sync_map_file(u64 addr);

    void share_to(pid_t from_id, pid_t to_id, info_t *info);
    bool copy_at(u64 vir);
};

/// map struct
struct map_t
{
    fs::vfs::file *file;
    u64 file_offset;
    u64 file_length;
    u64 mmap_length;
    info_t *vm_info;
    map_t(fs::vfs::file *f, u64 file_offset, u64 file_length, u64 mmap_length, info_t *vmi)
        : file(f)
        , file_offset(file_offset)
        , file_length(file_length)
        , mmap_length(mmap_length)
        , vm_info(vmi){};
};

bool fill_file_vm(vm_allocator &vma, u64 page_addr, vm_t *item);
bool fill_expand_vm(vm_allocator &vma, u64 page_addr, vm_t *item);

void sync_map_file(u64 addr);

} // namespace memory::vm
