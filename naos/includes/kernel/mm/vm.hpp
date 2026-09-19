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
#include <atomic>

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
    memory_object = 1ul << 20,
    user_stack = 1ul << 21,
};
}

void init();
void listen_page_fault();

struct vm_t;
class vm_allocator;
class info_t;

struct status_t
{
    u64 virtual_pages = 0;
    u64 vma_count = 0;
    u64 mapped_pages = 0;
    u64 rss_pages = 0;
    u64 private_pages = 0;
    u64 shared_pages = 0;
    u64 committed_pages = 0;
    u64 anonymous_pages = 0;
    u64 file_cache_pages = 0;
    u64 page_faults = 0;
    u64 peak_rss_pages = 0;
    u64 user_stack_pages = 0;
    u64 text_pages = 0;
};

enum class page_fault_method
{
    none,
    heap_break,
    common,
    common_with_bss,
    memory_object,
};

struct fault_history_t
{
    u64 last_offset = ~u64(0);
    u32 sequential_count = 0;
    u16 window_pages = 4;
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
    const vm_t *map_memory_object(u64 start, khandle backing, naos::data_plane::memory_object *object,
                                  u64 object_offset, u64 data_offset, u64 data_length, u64 map_length,
                                  flag_t page_ext_attr);

    bool unmap(u64 addr, u64 size);
    /// Release the physical pages in an anonymous VMA without removing the
    /// VMA.  The next access faults in a fresh zero page.
    bool decommit(u64 addr, u64 size);
    /// Validate a previously decommitted anonymous VMA range for recommit.
    /// Pages remain demand-zero and are materialized by the normal fault path.
    bool commit(u64 addr, u64 size);

    void share_to(process_id from_id, process_id to_id, info_t *info);
    void remove_fork_disallowed_mappings();
    /// Re-establish this address space's shared-page memory-object mappings
    /// after fork() made them read-only/COW.  The frames belong to the
    /// memory object, so neither process may privatize them.
    void restore_shared_memory_mappings();
    bool copy_at(u64 vir);

    void load() { paging_.load(); }

    arch::paging::page_table_t &paging() { return paging_; }
    vm_allocator &vma() { return vma_; }

    bool expand(page_fault_method method, u64 alignment_page, u64 access_address, vm_t *item);
    status_t status();
    void record_page_fault() { page_faults_.fetch_add(1, std::memory_order_relaxed); }

  private:
    bool expand_brk(u64 alignment_page, u64 access_address, vm_t *item);
    bool expand_vm(u64 alignment_page, u64 access_address, vm_t *item);
    bool expand_bss(u64 alignment_page, u64 access_address, vm_t *item);
    bool expand_memory_object(u64 alignment_page, u64 access_address, vm_t *item);
    void restore_fork_disallowed_mappings();

  private:
    memory::vm::vm_allocator vma_;
    arch::paging::page_table_t paging_;

    const vm_t *heap_vm_;
    u64 heap_top_;
    lock::spinlock_t paging_spin_;
    std::atomic_uint64_t page_faults_{0};
    std::atomic_uint64_t peak_rss_pages_{0};
};

/// map struct
struct map_t
{
    naos::data_plane::memory_object *memory_object;
    khandle backing;
    u64 file_offset;
    u64 file_length;
    /// Logical bytes begin at this offset within the page-rounded VMA.
    u64 data_offset;
    u64 data_length;
    u64 mmap_length;
    bool shared;
    /// True when this mapping faults onto the object's own page frames instead
    /// of private COW pages.  Such mappings observe each other's writes
    /// directly, so they are never written back and never privatized on fork.
    bool pages_shared;
    fault_history_t fault_history;
    info_t *vm_info;
    map_t(khandle backing, naos::data_plane::memory_object *object, u64 object_offset, u64 data_offset,
          u64 data_length, u64 map_length, bool shared, info_t *vmi)
        : memory_object(object)
        , backing(std::move(backing))
        , file_offset(object_offset)
        , file_length(map_length)
        , data_offset(data_offset)
        , data_length(data_length)
        , mmap_length(map_length)
        , shared(shared)
        , pages_shared(false)
        , fault_history()
        , vm_info(vmi) {};
    map_t(const map_t &rhs, info_t *vmi)
        : memory_object(rhs.memory_object)
        , backing(rhs.backing)
        , file_offset(rhs.file_offset)
        , file_length(rhs.file_length)
        , data_offset(rhs.data_offset)
        , data_length(rhs.data_length)
        , mmap_length(rhs.mmap_length)
        , shared(rhs.shared)
        , pages_shared(rhs.pages_shared)
        , fault_history(rhs.fault_history)
        , vm_info(vmi) {};
};

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
