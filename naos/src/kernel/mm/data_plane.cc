#include "kernel/mm/data_plane.hpp"

#include "kernel/arch/klib.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/terminal.hpp"
#include "kernel/ucontext.hpp"

namespace naos::data_plane
{
namespace
{
bool valid_extent(u64 offset, u64 size, u64 limit) { return offset <= limit && size <= limit - offset; }

void resize_bytes(freelibcxx::vector<byte> &bytes, u64 size) { bytes.resize(size, byte(0)); }

void copy_bytes(byte *destination, const byte *source, u64 size)
{
    for (u64 i = 0; i < size; i++)
        destination[i] = source[i];
}

void free_pages(freelibcxx::vector<byte *> &pages)
{
    for (auto *page : pages)
        memory::free_page(page);
    pages.clear();
}
} // namespace

memory_object::memory_object(u64 size, u32 flags)
    : kobject(type_of())
    , bytes_(memory::MemoryAllocatorV)
    , flags_(flags)
    , pages_(memory::MemoryAllocatorV)
{
    if (size <= NA_MEMORY_OBJECT_MAX_BYTES)
    {
        resize_bytes(bytes_, size);
        if (size == 0 || bytes_.data() != nullptr)
            return;
    }
    bytes_.clear();
}

memory_object::memory_object(byte *storage, u64 size)
    : kobject(type_of())
    , bytes_(memory::MemoryAllocatorV)
    , flags_(NA_MEMORY_FLAG_READ_ONLY)
    , view_(storage)
    , view_size_(size)
    , pages_(memory::MemoryAllocatorV)
{
}

memory_object::memory_object(phy_addr_t physical, byte *kernel_view, u64 size, release_callback on_release)
    : kobject(type_of())
    , bytes_(memory::MemoryAllocatorV)
    , flags_(0)
    , view_(kernel_view)
    , view_size_(size)
    , pages_(memory::MemoryAllocatorV)
    , physical_(physical)
    , on_release_(on_release)
{
}

memory_object::~memory_object()
{
    freelibcxx::vector<byte *> pages(memory::MemoryAllocatorV);
    freelibcxx::vector<byte> bytes(memory::MemoryAllocatorV);
    release_callback on_release = nullptr;
    {
        uctx::RawSpinLockUninterruptibleContext context(lock_);
        // Move storage out while protected, then release it after dropping the
        // spinlock.  Page freeing and allocator deallocation are not
        // interrupt-safe operations.
        pages = std::move(pages_);
        pages_size_ = 0;
        bytes = std::move(bytes_);
        on_release = on_release_;
        on_release_ = nullptr;
    }
    free_pages(pages);
    if (on_release != nullptr)
        on_release();
}

u64 memory_object::owned_size_locked() const
{
    if (view_ != nullptr)
        return view_size_;
    if (page_count() != 0)
        return pages_size_;
    return bytes_.size();
}

bool memory_object::readable_locked(u64 offset, u64 size) const
{
    return valid_extent(offset, size, owned_size_locked());
}

bool memory_object::writable_locked(u64 offset, u64 size) const
{
    if ((flags_ & NA_MEMORY_FLAG_READ_ONLY) != 0 || !valid_extent(offset, size, owned_size_locked()))
        return false;
    // Direct-mapped device memory is written through its kernel view; heap
    // objects write into the owned byte vector or its published page frames.
    // Immutable external views (view_ without physical_) stay read-only.
    return physical_.get() != nullptr || view_ == nullptr;
}

void memory_object::read_locked(u64 offset, byte *destination, u64 size) const
{
    if (view_ != nullptr)
    {
        copy_bytes(destination, view_ + offset, size);
        return;
    }
    if (page_count() != 0)
    {
        for (u64 done = 0; done < size;)
        {
            const u64 index = (offset + done) / memory::page_size;
            const u64 in_page = (offset + done) % memory::page_size;
            const u64 available = memory::page_size - in_page;
            const u64 chunk = available < (size - done) ? available : (size - done);
            copy_bytes(destination + done, page_at(index) + in_page, chunk);
            done += chunk;
        }
        return;
    }
    copy_bytes(destination, bytes_.data() + offset, size);
}

void memory_object::write_locked(u64 offset, const byte *source, u64 size)
{
    if (view_ == nullptr && page_count() != 0)
    {
        for (u64 done = 0; done < size;)
        {
            const u64 index = (offset + done) / memory::page_size;
            const u64 in_page = (offset + done) % memory::page_size;
            const u64 available = memory::page_size - in_page;
            const u64 chunk = available < (size - done) ? available : (size - done);
            copy_bytes(page_at(index) + in_page, source + done, chunk);
            done += chunk;
        }
        return;
    }
    copy_bytes((view_ != nullptr ? view_ : bytes_.data()) + offset, source, size);
}

na_status_t memory_object::publish_shared_pages()
{
    u64 size = 0;
    u64 count = 0;
    {
        uctx::RawSpinLockUninterruptibleContext context(lock_);
        if (page_count() != 0)
            return NA_STATUS_OK;
        // Immutable external views and direct-mapped device memory are
        // published through their own storage; an empty object has nothing to
        // publish.
        if (view_ != nullptr || physical_.get() != nullptr || bytes_.size() == 0)
            return NA_STATUS_NOT_SUPPORTED;
        size = bytes_.size();
        count = (size + memory::page_size - 1) / memory::page_size;
    }

    // Allocate all page storage before taking the object spinlock.  A second
    // publisher may win the race; that case discards this private batch after
    // the lock is released.
    freelibcxx::vector<byte *> allocated(memory::MemoryAllocatorV);
    allocated.ensure(count);
    if (allocated.capacity() < count)
        return NA_STATUS_RESOURCE_EXHAUSTED;
    for (u64 index = 0; index < count; index++)
    {
        auto *page = reinterpret_cast<byte *>(memory::malloc_page());
        if (page == nullptr)
        {
            free_pages(allocated);
            return NA_STATUS_RESOURCE_EXHAUSTED;
        }
        // These frames become visible to user mappings, so the whole page is
        // zeroed: the tail of the object must not disclose kernel memory.
        memset(page, 0, memory::page_size);
        allocated.push_back(page);
    }

    freelibcxx::vector<byte> old_bytes(memory::MemoryAllocatorV);
    bool installed = false;
    {
        uctx::RawSpinLockUninterruptibleContext context(lock_);
        if (page_count() == 0 && view_ == nullptr && physical_.get() == nullptr && bytes_.size() != 0)
        {
            for (u64 done = 0; done < size;)
            {
                const u64 index = done / memory::page_size;
                const u64 in_page = done % memory::page_size;
                const u64 available = memory::page_size - in_page;
                const u64 chunk = available < (size - done) ? available : (size - done);
                copy_bytes(allocated[index] + in_page, bytes_.data() + done, chunk);
                done += chunk;
            }
            pages_ = std::move(allocated);
            pages_size_ = size;
            old_bytes = std::move(bytes_);
            installed = true;
        }
    }
    if (!installed)
    {
        free_pages(allocated);
        return NA_STATUS_OK;
    }
    // `old_bytes` is deliberately destroyed after the lock scope.  It owns the
    // former kernel byte vector and therefore performs the allocator call only
    // after the spinlock has been released.
    return NA_STATUS_OK;
}

phy_addr_t memory_object::page_frame(u64 object_offset) const
{
    uctx::RawSpinLockUninterruptibleContext context(lock_);
    if (page_count() == 0 || object_offset >= pages_size_)
        return nullptr;
    return memory::va2pa(page_at(object_offset / memory::page_size));
}

bool memory_object::page_backed() const
{
    uctx::RawSpinLockUninterruptibleContext context(lock_);
    return page_count() != 0;
}

bool memory_object::readable(u64 offset, u64 size) const
{
    uctx::RawSpinLockUninterruptibleContext context(lock_);
    return readable_locked(offset, size);
}

bool memory_object::writable(u64 offset, u64 size) const
{
    uctx::RawSpinLockUninterruptibleContext context(lock_);
    return writable_locked(offset, size);
}

na_status_t memory_object::read(u64 offset, byte *destination, u64 size, u64 &actual) const
{
    actual = 0;
    if (size != 0 && destination == nullptr)
        return NA_STATUS_INVALID_ARGUMENT;
    uctx::RawSpinLockUninterruptibleContext context(lock_);
    if (!readable_locked(offset, size))
        return NA_STATUS_INVALID_ARGUMENT;
    read_locked(offset, destination, size);
    actual = size;
    return NA_STATUS_OK;
}

na_status_t memory_object::write(u64 offset, const byte *source, u64 size, u64 &actual)
{
    actual = 0;
    if (size != 0 && source == nullptr)
        return NA_STATUS_INVALID_ARGUMENT;
    uctx::RawSpinLockUninterruptibleContext context(lock_);
    if (!writable_locked(offset, size))
        return (flags_ & NA_MEMORY_FLAG_READ_ONLY) != 0 ? NA_STATUS_ACCESS_DENIED : NA_STATUS_INVALID_ARGUMENT;
    write_locked(offset, source, size);
    actual = size;
    return NA_STATUS_OK;
}

u64 memory_object::size() const
{
    uctx::RawSpinLockUninterruptibleContext context(lock_);
    return owned_size_locked();
}

} // namespace naos::data_plane
