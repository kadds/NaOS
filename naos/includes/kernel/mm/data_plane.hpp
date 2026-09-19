#pragma once

#include "freelibcxx/vector.hpp"
#include "kernel/kobject.hpp"
#include "kernel/lock.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/wait.hpp"
#include "naos/abi.h"

namespace naos::ipc
{
class protocol_endpoint;
}

namespace task
{
struct process_t;
}

namespace memory::vm
{
struct fault_history_t;
}

namespace naos::data_plane
{

enum class page_fault_result : u8
{
    ready,
    blocked,
    failed,
};

class memory_object final : public kobject
{
  public:
    using release_callback = void (*)();
    struct page_alias_tag
    {
    };

    memory_object(u64 size, u32 flags);
    /// Non-owning page-backed view used as a pager I/O destination. The page
    /// owner keeps the frames alive until the view is destroyed.
    memory_object(byte *const *pages, u64 page_count, u64 size, page_alias_tag);
    /// Immutable zero-copy view over kernel-owned storage (the
    /// immutable-boot-object backing of USERSPACE_FILESYSTEM_ADR §5.1.3).
    /// Never allocates or copies: the size is not limited by
    /// NA_MEMORY_OBJECT_MAX_BYTES and the bytes are never truncated.
    memory_object(byte *storage, u64 size);
    /// Direct-mapped physical display/device memory (framebuffer service).
    /// `kernel_view` is the kernel's cached mapping of the same physical
    /// range (used by read/write IPC); user mappings fault directly onto the
    /// physical pages with write-through/uncached semantics instead of
    /// copying through a shadow buffer.  The physical base must be
    /// page-aligned; the size is not limited by NA_MEMORY_OBJECT_MAX_BYTES.
    memory_object(phy_addr_t physical, byte *kernel_view, u64 size, release_callback on_release = nullptr);
    ~memory_object() override;

    static type_e type_of() { return type_e::memory_object; }

    bool readable(u64 offset, u64 size) const;
    bool writable(u64 offset, u64 size) const;
    na_status_t read(u64 offset, byte *destination, u64 size, u64 &actual) const;
    na_status_t write(u64 offset, const byte *source, u64 size, u64 &actual);

    u64 size() const;
    u32 flags() const { return flags_; }
    /// Non-null physical base when this object is direct-mapped device
    /// memory (framebuffer).  The kernel view remains valid for read/write.
    phy_addr_t physical() const { return physical_; }

    /// Bind the object's owned bytes to page frames and make those frames the
    /// single source of truth for both the kernel view and every
    /// NA_MEMORY_MAP_SHARED mapping.  Idempotent.  Returns
    /// NA_STATUS_NOT_SUPPORTED for immutable external views and direct-mapped
    /// device memory, which publish their own storage instead of a page cache.
    na_status_t publish_shared_pages();
    /// Physical frame backing the page holding `object_offset`.  Null when the
    /// object is not page backed or the offset is outside the object, so a
    /// faulting mapping can fall back to a private page.
    phy_addr_t page_frame(u64 object_offset) const;
    /// True once publish_shared_pages() bound owned bytes to page frames.
    bool page_backed() const;
    /// True when pages are supplied lazily by a user-space File pager.
    bool pager_backed() const;

    /// Attach the File client used by the existing map/exec boundary as this
    /// object's lazy pager. The caller must have validated the capability.
    na_status_t attach_pager(khandle pager);

    /// Resolve one page through the attached user-space pager. In interrupt
    /// context this registers the current thread and returns blocked; the
    /// pager worker performs the actual IPC and wakes the thread.
    page_fault_result fault_page(u64 object_offset, const khandle &backing = {},
                                 memory::vm::fault_history_t *history = nullptr) const;

    /// Service a page request previously marked as loading. The request may
    /// cover a small aligned prefetch window. This is called by the pager
    /// worker, outside interrupt context.
    bool service_fault_pages(u64 first_page, u64 page_count, task::process_t *accounting_process = nullptr) const;

  private:
    /// Logical size of the owned storage, without the page-cache rounding.
    u64 owned_size_locked() const;
    /// Page-cache accessors usable from const helpers.
    u64 page_count() const { return pages_.size(); }
    byte *page_at(u64 index) const { return pages_.data()[index]; }
    bool readable_locked(u64 offset, u64 size) const;
    bool writable_locked(u64 offset, u64 size) const;
    void read_locked(u64 offset, byte *destination, u64 size) const;
    void write_locked(u64 offset, const byte *source, u64 size);
    bool pager_backed_locked() const { return static_cast<bool>(pager_); }

    mutable lock::spinlock_t lock_;
    freelibcxx::vector<byte> bytes_;
    u32 flags_;
    /// Non-null when this object is an external view over kernel-owned
    /// storage instead of an owned byte vector.  For direct-mapped device
    /// memory this is the kernel's cached view of the physical range.
    byte *view_ = nullptr;
    u64 view_size_ = 0;
    /// Page-aligned kernel view of the owned bytes, populated by
    /// publish_shared_pages().  Once non-empty these frames replace `bytes_`
    /// as the object's storage so a shared user mapping cannot diverge from
    /// the kernel view.
    freelibcxx::vector<byte *> pages_;
    u64 pages_size_ = 0;
    bool pages_owned_ = true;
    khandle pager_;
    freelibcxx::vector<u8> page_states_;
    mutable task::wait_queue_t pager_wait_queue_;
    naos::ipc::protocol_endpoint *pager_endpoint_ = nullptr;
    /// Physical base of direct-mapped device memory; null for heap objects
    /// and immutable boot-archive views.
    phy_addr_t physical_{nullptr};
    release_callback on_release_ = nullptr;
};

void init_pager_worker();
} // namespace naos::data_plane
