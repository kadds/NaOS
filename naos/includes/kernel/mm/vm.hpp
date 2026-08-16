#pragma once
#include "../lock.hpp"
#include "freelibcxx/hash_map.hpp"
#include "freelibcxx/skip_list.hpp"
#include "freelibcxx/vector.hpp"
#include "kernel/arch/paging.hpp"
#include "kernel/common.hpp"
#include "kernel/handle.hpp"
#include "kernel/kobject.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/types.hpp"
#include "list_node_cache.hpp"
#include "vm.hpp"

namespace fs::vfs
{
class file;
class pseudo_t;
} // namespace fs::vfs

namespace naos::data_plane
{
class memory_object;
} // namespace naos::data_plane

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
    shared = 1ul << 19,
};
}

void init();
void listen_page_fault();

struct vm_t;
class vm_allocator;
class info_t;

enum class page_fault_method
{
    none,
    heap_break,
    common,
    common_with_bss,
    file,
    memory_object,
    physical,
};

class vm_allocator
{
  public:
    using list_t = freelibcxx::skip_list<vm_t>;

  private:
    list_t list;
    lock::rw_lock_t list_lock;
    u64 range_top, range_bottom;

  public:
    vm_allocator(u64 top, u64 bottom)
        : list(memory::KernelCommonAllocatorV)
        , range_top(top)
        , range_bottom(bottom)
    {
    }

    ~vm_allocator();

    void set_range(u64 top, u64 bottom)
    {
        range_top = top;
        range_bottom = bottom;
    }

    const vm_t *allocate_map(u64 size, u64 flags, page_fault_method method, u64 user_data);
    void deallocate_map(const vm_t *vm);
    bool deallocate_map(u64 p);

    const vm_t *add_map(u64 start, u64 end, u64 flags, page_fault_method method, u64 user_data);

    vm_t *get_vm_area(u64 p);

    list_t &get_list() { return list; }
    lock::rw_lock_t &get_lock() { return list_lock; }
    void clone(info_t *info, vm_allocator &to, flag_t flag);

  private:
};

struct shared_info_t
{
    lock::spinlock_t spin_lock;
    freelibcxx::hash_set<process_id> shared_pid;
    shared_info_t(freelibcxx::Allocator *allocator)
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
    info_t();
    info_t(arch::paging::page_table_t paging);

    ~info_t();
    info_t(const info_t &) = delete;
    info_t &operator=(const info_t &) = delete;

    // break offset
    bool init_brk(u64 start);
    bool set_brk(u64 ptr);
    bool set_brk_now(u64 ptr);
    u64 get_brk();

    // mmap virtual address
    const vm_t *map_file(u64 start, fs::vfs::file *file, u64 file_offset, u64 file_length, u64 mmap_length,
                         flag_t page_ext_attr);
    const vm_t *map_memory_object(u64 start, khandle backing, naos::data_plane::memory_object *object,
                                  u64 object_offset, u64 length, flag_t page_ext_attr);

    bool umap_file(u64 addr, u64 size);
    void sync_map_file(u64 addr);

    void share_to(process_id from_id, process_id to_id, info_t *info);
    void remove_fork_disallowed_mappings();
    bool copy_at(u64 vir);

    void load() { paging_.load(); }

    arch::paging::page_table_t &paging() { return paging_; }
    vm_allocator &vma() { return vma_; }

    bool expand(page_fault_method method, u64 alignment_page, u64 access_address, vm_t *item);

  private:
    bool expand_brk(u64 alignment_page, u64 access_address, vm_t *item);
    bool expand_vm(u64 alignment_page, u64 access_address, vm_t *item);
    bool expand_bss(u64 alignment_page, u64 access_address, vm_t *item);
    bool expand_file(u64 alignment_page, u64 access_address, vm_t *item);
    bool expand_memory_object(u64 alignment_page, u64 access_address, vm_t *item);
    bool expand_physical(u64 alignment_page, u64 access_address, vm_t *item);
    void restore_fork_disallowed_mappings();

  private:
    memory::vm::vm_allocator vma_;
    arch::paging::page_table_t paging_;

    const vm_t *heap_vm_;
    u64 heap_top_;
    lock::spinlock_t paging_spin_;
};

/// map struct
struct map_t
{
    fs::vfs::file *file;
    naos::data_plane::memory_object *memory_object;
    khandle backing;
    phy_addr_t physical_address;
    fs::vfs::pseudo_t *pseudo;
    u64 file_offset;
    u64 file_length;
    u64 mmap_length;
    info_t *vm_info;
    map_t(fs::vfs::file *f, u64 file_offset, u64 file_length, u64 mmap_length, info_t *vmi)
        : file(f)
        , memory_object(nullptr)
        , backing()
        , physical_address(nullptr)
        , pseudo(nullptr)
        , file_offset(file_offset)
        , file_length(file_length)
        , mmap_length(mmap_length)
        , vm_info(vmi) {};
    map_t(khandle backing, naos::data_plane::memory_object *object, u64 object_offset, u64 length, info_t *vmi)
        : file(nullptr)
        , memory_object(object)
        , backing(std::move(backing))
        , physical_address(nullptr)
        , pseudo(nullptr)
        , file_offset(object_offset)
        , file_length(length)
        , mmap_length(length)
        , vm_info(vmi) {};
    map_t(phy_addr_t physical_address, u64 file_offset, u64 file_length, u64 mmap_length, info_t *vmi)
        : file(nullptr)
        , memory_object(nullptr)
        , backing()
        , physical_address(physical_address)
        , pseudo(nullptr)
        , file_offset(file_offset)
        , file_length(file_length)
        , mmap_length(mmap_length)
        , vm_info(vmi) {};
    map_t(const map_t &rhs, info_t *vmi)
        : file(rhs.file)
        , memory_object(rhs.memory_object)
        , backing(rhs.backing)
        , physical_address(rhs.physical_address)
        , pseudo(rhs.pseudo)
        , file_offset(rhs.file_offset)
        , file_length(rhs.file_length)
        , mmap_length(rhs.mmap_length)
        , vm_info(vmi) {};
};

void sync_map_file(u64 addr);

struct vm_t
{
    u64 start;
    u64 end;
    u64 flags;
    page_fault_method method;
    u64 user_data;
    u64 alloc_times = 0;
    bool operator==(const vm_t &rhs) const { return rhs.end == end; }
    bool operator<(const vm_t &rhs) const { return end < rhs.end; }
    bool operator<=(const vm_t &rhs) const { return end <= rhs.end; }
    bool operator>(const vm_t &rhs) const { return end > rhs.end; }
    vm_t(u64 start, u64 end, u64 flags, page_fault_method method, u64 user_data)
        : start(start)
        , end(end)
        , flags(flags)
        , method(method)
        , user_data(user_data) {};
    vm_t(u64 start, u64 end, u64 flags)
        : start(start)
        , end(end)
        , flags(flags)
        , method(page_fault_method::none)
        , user_data(0) {};
};

} // namespace memory::vm
