#pragma once

#include "freelibcxx/vector.hpp"
#include "kernel/kobject.hpp"
#include "kernel/lock.hpp"
#include "kernel/mm/new.hpp"
#include "naos/abi.h"

namespace naos::data_plane
{

class memory_object final : public kobject
{
  public:
    using release_callback = void (*)();

    memory_object(u64 size, u32 flags);
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
    /// Physical base of direct-mapped device memory; null for heap objects
    /// and immutable boot-archive views.
    phy_addr_t physical_{nullptr};
    release_callback on_release_ = nullptr;
};

} // namespace naos::data_plane
