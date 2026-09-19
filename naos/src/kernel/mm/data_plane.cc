#include "kernel/mm/data_plane.hpp"
#include "kernel/arch/cpu.hpp"
#include "kernel/ipc/invocation.hpp"

#include "kernel/arch/klib.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/mm/vm.hpp"
#include "kernel/task.hpp"
#include "kernel/terminal.hpp"
#include "kernel/ucontext.hpp"
#include "naos/generated/system/File.hpp"
#include "naos/generated/system/MemoryObject.hpp"
#include <limits>

KLOG_MODULE(mm);
namespace naos::data_plane
{
namespace
{
bool valid_extent(u64 offset, u64 size, u64 limit) { return offset <= limit && size <= limit - offset; }

constexpr u8 page_unloaded = 0;
constexpr u8 page_loading = 1;
constexpr u8 page_ready = 2;
constexpr u8 page_failed = 3;
constexpr u64 maximum_fault_window_pages = (1 * 1024 * 1024) / memory::page_size;
constexpr u64 pager_io_window_pages = 4;
static_assert(maximum_fault_window_pages <= 0xffff);

struct pager_fault_job
{
    handle_t<memory_object> object;
    handle_t<task::process_object> process;
    u64 first_page;
    u64 page_count;
    pager_fault_job *next = nullptr;
};

struct pager_fault_queue
{
    lock::spinlock_t lock;
    task::wait_queue_t wait;
    pager_fault_job *head = nullptr;
    pager_fault_job *tail = nullptr;
    std::atomic_uint64_t pending{0};

    void enqueue(pager_fault_job *job)
    {
        {
            uctx::RawSpinLockUninterruptibleContext guard(lock);
            if (tail != nullptr)
                tail->next = job;
            else
                head = job;
            tail = job;
            pending.fetch_add(1, std::memory_order_release);
        }
        wait.do_wake_up(1);
    }

    pager_fault_job *pop()
    {
        uctx::RawSpinLockUninterruptibleContext guard(lock);
        auto *job = head;
        if (job == nullptr)
            return nullptr;
        head = job->next;
        if (head == nullptr)
            tail = nullptr;
        job->next = nullptr;
        pending.fetch_sub(1, std::memory_order_release);
        return job;
    }
};

pager_fault_queue *pager_queue = nullptr;

void pager_worker(task::thread_start_info_t *)
{
    for (;;)
    {
        pager_queue->wait.do_wait([] { return pager_queue->pending.load(std::memory_order_acquire) != 0; });
        auto *job = pager_queue->pop();
        if (job == nullptr)
            continue;
        job->object->service_fault_pages(job->first_page, job->page_count,
                                         job->process ? job->process->process() : nullptr);
        memory::Delete<>(memory::KernelCommonAllocatorV, job);
    }
}

void resize_bytes(freelibcxx::vector<byte> &bytes, u64 size) { bytes.resize(size, byte(0)); }

void copy_bytes(byte *destination, const byte *source, u64 size)
{
    for (u64 i = 0; i < size; i++)
        destination[i] = source[i];
}

void free_pages(freelibcxx::vector<byte *> &pages)
{
    for (auto *page : pages)
    {
        if (page != nullptr)
            memory::free_page(page);
    }
    pages.clear();
}
} // namespace

memory_object::memory_object(u64 size, u32 flags)
    : kobject(type_of())
    , bytes_(memory::MemoryAllocatorV)
    , flags_(flags)
    , pages_(memory::MemoryAllocatorV)
    , page_states_(memory::MemoryAllocatorV)
{
    if (size <= NA_MEMORY_OBJECT_MAX_BYTES)
    {
        resize_bytes(bytes_, size);
        if (size == 0 || bytes_.data() != nullptr)
            return;
    }
    bytes_.clear();
}

memory_object::memory_object(byte *const *pages, u64 page_count, u64 size, page_alias_tag)
    : kobject(type_of())
    , bytes_(memory::MemoryAllocatorV)
    , flags_(0)
    , pages_(memory::MemoryAllocatorV)
    , pages_size_(size)
    , pages_owned_(false)
    , page_states_(memory::MemoryAllocatorV)
{
    if (pages == nullptr || page_count == 0 || size == 0)
        return;
    pages_.ensure(page_count);
    page_states_.ensure(page_count);
    if (pages_.capacity() < page_count || page_states_.capacity() < page_count)
        return;
    for (u64 index = 0; index < page_count; index++)
    {
        if (pages[index] == nullptr)
        {
            pages_.clear();
            page_states_.clear();
            return;
        }
        pages_.push_back(pages[index]);
        page_states_.push_back(page_ready);
    }
}

na_status_t memory_object::attach_pager(khandle pager)
{
    if (!pager)
        return NA_STATUS_INVALID_HANDLE;

    u64 size = 0;
    {
        uctx::RawSpinLockUninterruptibleContext context(lock_);
        if (view_ != nullptr || physical_.get() != nullptr || !pages_.empty())
            return NA_STATUS_INVALID_ARGUMENT;
        if (pager_)
            return pager_.get_control() == pager.get_control() ? NA_STATUS_OK : NA_STATUS_INVALID_ARGUMENT;
        size = bytes_.size();
        if (size == 0)
            return NA_STATUS_INVALID_ARGUMENT;
    }

    freelibcxx::vector<byte> old_bytes(memory::MemoryAllocatorV);
    freelibcxx::vector<byte *> pages(memory::MemoryAllocatorV);
    freelibcxx::vector<u8> states(memory::MemoryAllocatorV);
    const u64 count = (size + memory::page_size - 1) / memory::page_size;
    pages.resize(count, nullptr);
    states.resize(count, u8{page_unloaded});
    if (pages.size() != count || states.size() != count)
        return NA_STATUS_RESOURCE_EXHAUSTED;
    {
        uctx::RawSpinLockUninterruptibleContext context(lock_);
        if (view_ != nullptr || physical_.get() != nullptr || !pages_.empty() || pager_ || bytes_.size() != size)
            return NA_STATUS_INVALID_ARGUMENT;
        if (pager->get_ktype() != ipc::protocol_endpoint::type_of(ipc::endpoint_role::client))
            return NA_STATUS_WRONG_BINDING;
        auto *endpoint = pager->get_unsafe<ipc::protocol_endpoint>();
        pager_ = std::move(pager);
        pager_endpoint_ = endpoint;
        pager_endpoint_->acquire_kernel_reference();
        flags_ |= NA_MEMORY_FLAG_READ_ONLY;
        pages_size_ = size;
        old_bytes = std::move(bytes_);
        pages_ = std::move(pages);
        page_states_ = std::move(states);
    }
    return NA_STATUS_OK;
}

memory_object::memory_object(byte *storage, u64 size)
    : kobject(type_of())
    , bytes_(memory::MemoryAllocatorV)
    , flags_(NA_MEMORY_FLAG_READ_ONLY)
    , view_(storage)
    , view_size_(size)
    , pages_(memory::MemoryAllocatorV)
    , page_states_(memory::MemoryAllocatorV)
{
}

memory_object::memory_object(phy_addr_t physical, byte *kernel_view, u64 size, release_callback on_release)
    : kobject(type_of())
    , bytes_(memory::MemoryAllocatorV)
    , flags_(0)
    , view_(kernel_view)
    , view_size_(size)
    , pages_(memory::MemoryAllocatorV)
    , page_states_(memory::MemoryAllocatorV)
    , physical_(physical)
    , on_release_(on_release)
{
}

memory_object::~memory_object()
{
    freelibcxx::vector<byte *> pages(memory::MemoryAllocatorV);
    freelibcxx::vector<byte> bytes(memory::MemoryAllocatorV);
    khandle pager;
    ipc::protocol_endpoint *pager_endpoint = nullptr;
    bool pages_owned = true;
    release_callback on_release = nullptr;
    {
        uctx::RawSpinLockUninterruptibleContext context(lock_);
        // Move storage out while protected, then release it after dropping the
        // spinlock.  Page freeing and allocator deallocation are not
        // interrupt-safe operations.
        pages = std::move(pages_);
        pages_size_ = 0;
        bytes = std::move(bytes_);
        pager = std::move(pager_);
        pager_endpoint = pager_endpoint_;
        pager_endpoint_ = nullptr;
        pages_owned = pages_owned_;
        on_release = on_release_;
        on_release_ = nullptr;
    }
    if (pages_owned)
        free_pages(pages);
    else
        pages.clear();
    if (pager_endpoint != nullptr)
        pager_endpoint->release_kernel_reference();
    if (on_release != nullptr)
        on_release();
}

u64 memory_object::owned_size_locked() const
{
    if (view_ != nullptr)
        return view_size_;
    if (pager_backed_locked() || page_count() != 0)
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
        if (pager_backed_locked())
            return NA_STATUS_OK;
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
    if (pager_backed_locked() && page_states_.data()[object_offset / memory::page_size] != page_ready)
        return nullptr;
    auto *page = page_at(object_offset / memory::page_size);
    return page == nullptr ? nullptr : memory::va2pa(page);
}

bool memory_object::page_backed() const
{
    uctx::RawSpinLockUninterruptibleContext context(lock_);
    return pager_backed_locked() || page_count() != 0;
}

bool memory_object::pager_backed() const
{
    uctx::RawSpinLockUninterruptibleContext context(lock_);
    return pager_backed_locked();
}

bool memory_object::service_fault_pages(u64 first_page, u64 requested_page_count,
                                         task::process_t *accounting_process) const
{
    auto *self = const_cast<memory_object *>(this);
    khandle pager;
    u64 offset = 0;
    u64 length = 0;
    {
        uctx::RawSpinLockUninterruptibleContext context(self->lock_);
        if (!self->pager_backed_locked() || requested_page_count == 0 || first_page >= self->page_count() ||
            requested_page_count > self->page_count() - first_page)
        {
            return false;
        }
        for (u64 index = 0; index < requested_page_count; index++)
        {
            const u64 page_index = first_page + index;
            // A page is installed before the request so the user-space pager
            // can map the final frame directly. It is still unavailable until
            // the request completes, so a duplicate job must not report
            // success.
            if (self->page_states_[page_index] != page_loading || self->pages_[page_index] != nullptr)
                return false;
        }
        pager = self->pager_;
        offset = first_page * memory::page_size;
        const u64 requested_bytes = requested_page_count * memory::page_size;
        length = self->pages_size_ - offset < requested_bytes ? self->pages_size_ - offset : requested_bytes;
    }

    auto fail_loading_pages = [&] {
        {
            uctx::RawSpinLockUninterruptibleContext context(self->lock_);
            for (u64 index = 0; index < requested_page_count; index++)
            {
                const u64 page_index = first_page + index;
                if (self->page_states_[page_index] == page_loading && self->pages_[page_index] == nullptr)
                    self->page_states_[page_index] = page_failed;
            }
        }
        self->pager_wait_queue_.do_wake_up();
    };

    freelibcxx::vector<byte *> pages(memory::MemoryAllocatorV);
    pages.resize(requested_page_count, nullptr);
    if (pages.size() != requested_page_count)
    {
        fail_loading_pages();
        return false;
    }
    for (u64 index = 0; index < requested_page_count; index++)
    {
        pages[index] = reinterpret_cast<byte *>(memory::malloc_page());
        if (pages[index] == nullptr)
        {
            free_pages(pages);
            fail_loading_pages();
            return false;
        }
    }

    bool can_install = true;
    {
        uctx::RawSpinLockUninterruptibleContext context(self->lock_);
        for (u64 index = 0; index < requested_page_count; index++)
        {
            const u64 page_index = first_page + index;
            if (self->page_states_[page_index] != page_loading || self->pages_[page_index] != nullptr)
            {
                can_install = false;
                break;
            }
        }
        if (can_install)
        {
            for (u64 index = 0; index < requested_page_count; index++)
                self->pages_[first_page + index] = pages[index];
        }
    }
    if (!can_install)
    {
        free_pages(pages);
        return false;
    }

    bool loaded = true;
    for (u64 chunk_first = 0; chunk_first < requested_page_count && loaded;)
    {
        const u64 chunk_pages = requested_page_count - chunk_first < pager_io_window_pages
                                    ? requested_page_count - chunk_first
                                    : pager_io_window_pages;
        const u64 chunk_offset = offset + chunk_first * memory::page_size;
        const u64 chunk_bytes = self->pages_size_ - chunk_offset < chunk_pages * memory::page_size
                                    ? self->pages_size_ - chunk_offset
                                    : chunk_pages * memory::page_size;
        auto buffer = handle_t<memory_object>::make(pages.data() + chunk_first, chunk_pages, chunk_bytes,
                                                     memory_object::page_alias_tag{});
        bool chunk_loaded = false;
        if (buffer && buffer->page_backed())
        {
            freelibcxx::vector<byte> request(memory::MemoryAllocatorV);
            request.resize(naos::system::File::pread_request_header_bytes, byte{});
            naos::system::File::pread_request value{};
            value.offset = static_cast<i64>(chunk_offset);
            value.size = chunk_bytes;
            value.flags = 0;
            value.buffer.value = 0;
            u64 written = 0;
            if (naos::system::File::encode_pread_request(
                    reinterpret_cast<std::uint8_t *>(request.data()), request.size(), value, written))
            {
                request.resize(written, byte{});
                capability::metadata buffer_metadata;
                buffer_metadata.binding = NA_BINDING_MEMORY_OBJECT;
                buffer_metadata.protocol_uuid = naos::system::MemoryObject::protocol_uuid;
                buffer_metadata.scope = NA_SCOPE_MEMORY_OBJECT;
                buffer_metadata.revision = naos::system::MemoryObject::revision;
                buffer_metadata.meta_rights = NA_RIGHT_TRANSFER | NA_RIGHT_INSPECT | NA_RIGHT_WAIT;
                buffer_metadata.protocol_rights =
                    NA_MEMORY_RIGHT_READ | NA_MEMORY_RIGHT_WRITE | NA_MEMORY_RIGHT_MAP | NA_MEMORY_RIGHT_INFO;
                buffer_metadata.view_length = chunk_bytes;
                capability::transfer_record_list resources(memory::KernelCommonAllocatorV);
                resources.push_back(capability::transfer_record(
                    NA_HANDLE_INVALID, false,
                    capability::transferred_resource(khandle(buffer.get_control()), buffer_metadata)));
                auto invocation = ipc::submit_kernel_invocation(pager, NA_METHOD_FILE_PREAD, std::move(request),
                                                                std::move(resources));
                if (invocation)
                {
                    invocation->wait_queue().do_wait(
                        [&invocation] { return (invocation->signals() & NA_SIGNAL_COMPLETED) != 0; });
                    na_result_frame_t result{};
                    result.struct_size = sizeof(result);
                    result.byte_capacity = 64;
                    result.resource_capacity = 0;
                    freelibcxx::vector<byte> response(memory::MemoryAllocatorV);
                    capability::transfer_record_list response_resources(memory::KernelCommonAllocatorV);
                    const auto claim = invocation->claim_result(result, response, response_resources);
                    if (claim == NA_STATUS_OK)
                    {
                        naos::system::File::pread_response decoded{};
                        const bool decoded_ok = naos::system::File::decode_pread_response(
                            reinterpret_cast<const std::uint8_t *>(response.data()), response.size(), decoded);
                        chunk_loaded = result.protocol_error == 0 && result.actual_resources == 0 && decoded_ok &&
                                       decoded.count == chunk_bytes;
                        response_resources.clear();
                        (void)invocation->commit_result();
                    }
                }
            }
        }
        if (!chunk_loaded)
            loaded = false;
        else
            chunk_first += chunk_pages;
    }

    if (loaded)
    {
        const u64 tail = length % memory::page_size;
        if (tail != 0)
            memset(pages[requested_page_count - 1] + tail, 0, memory::page_size - tail);
        if (accounting_process != nullptr)
            accounting_process->file_inputs.fetch_add((length + 511) / 512, std::memory_order_relaxed);
    }
    {
        uctx::RawSpinLockUninterruptibleContext context(self->lock_);
        if (loaded)
        {
            for (u64 index = 0; index < requested_page_count; index++)
                self->page_states_[first_page + index] = page_ready;
        }
        else
        {
            for (u64 index = 0; index < requested_page_count; index++)
            {
                self->pages_[first_page + index] = nullptr;
                self->page_states_[first_page + index] = page_failed;
            }
        }
    }
    if (!loaded)
    {
        free_pages(pages);
    }
    self->pager_wait_queue_.do_wake_up();
    return loaded;
}

page_fault_result memory_object::fault_page(u64 object_offset, const khandle &backing,
                                            memory::vm::fault_history_t *history) const
{
    auto *self = const_cast<memory_object *>(this);
    const u64 page_index = object_offset / memory::page_size;
    for (;;)
    {
        bool loading = false;
        u64 request_first_page = page_index;
        u64 request_page_count = 1;
        {
            uctx::RawSpinLockUninterruptibleContext context(self->lock_);
            if (!self->pager_backed_locked() || page_index >= self->page_count())
            {
                return page_fault_result::failed;
            }
            if (self->page_states_[page_index] == page_ready)
            {
                self->pager_wait_queue_.remove(task::current());
                return page_fault_result::ready;
            }
            if (self->page_states_[page_index] == page_failed)
            {
                self->pager_wait_queue_.remove(task::current());
                return page_fault_result::failed;
            }
            loading = self->page_states_[page_index] == page_loading;
            if (!loading)
            {
                constexpr u64 first_fault_window_pages = 4;
                const u64 current_offset = page_index * memory::page_size;
                u64 window_pages = first_fault_window_pages;
                if (history != nullptr)
                {
                    if (history->last_offset == ~u64(0))
                    {
                        history->window_pages = first_fault_window_pages;
                        history->sequential_count = 0;
                    }
                    else if (history->last_offset == current_offset)
                    {
                        const u64 next_window = static_cast<u64>(history->window_pages) * 2;
                        history->window_pages = static_cast<u16>(
                            next_window > maximum_fault_window_pages ? maximum_fault_window_pages : next_window);
                        if (history->sequential_count != ~u32(0))
                            history->sequential_count++;
                    }
                    else
                    {
                        history->window_pages = 1;
                        history->sequential_count = 0;
                    }
                    window_pages = history->window_pages;
                }

                request_page_count = self->page_count() - request_first_page < window_pages
                                         ? self->page_count() - request_first_page
                                         : window_pages;
                for (u64 index = 0; index < request_page_count; index++)
                {
                    if (self->page_states_[request_first_page + index] != page_unloaded)
                    {
                        request_page_count = index;
                        break;
                    }
                }
                for (u64 index = 0; index < request_page_count; index++)
                    self->page_states_[request_first_page + index] = page_loading;
                if (history != nullptr)
                    history->last_offset = (request_first_page + request_page_count) * memory::page_size;
            }
        }

        const bool fault_context =
            arch::cpu::current().is_in_interrupt_context() || arch::cpu::current().is_in_exception_context();
        if (!fault_context)
        {
            if (loading)
            {
                self->pager_wait_queue_.do_wait([self, page_index] {
                    uctx::RawSpinLockUninterruptibleContext context(self->lock_);
                    // The pager installs the final frame before issuing the
                    // IPC so the transferred MemoryObject can alias it.  A
                    // non-null page therefore means "loading", not that a
                    // fault waiter may resume yet.
                    return self->page_states_[page_index] == page_ready ||
                           self->page_states_[page_index] == page_failed;
                });
                continue;
            }
            return self->service_fault_pages(request_first_page, request_page_count, task::current_process())
                       ? page_fault_result::ready
                       : page_fault_result::failed;
        }

        if (loading)
        {
            const bool blocked = self->pager_wait_queue_.block_current([self, page_index] {
                uctx::RawSpinLockUninterruptibleContext context(self->lock_);
                return self->page_states_[page_index] == page_ready || self->page_states_[page_index] == page_failed;
            });
            if (blocked)
                return page_fault_result::blocked;
            continue;
        }

        if (pager_queue == nullptr)
        {
            uctx::RawSpinLockUninterruptibleContext context(self->lock_);
            for (u64 index = 0; index < request_page_count; index++)
                self->page_states_[request_first_page + index] = page_failed;
            KLOG_WARN("pager queue unavailable object {} page {}", self->object_id(), page_index);
            return page_fault_result::failed;
        }
        if (!backing)
        {
            uctx::RawSpinLockUninterruptibleContext context(self->lock_);
            for (u64 index = 0; index < request_page_count; index++)
                self->page_states_[request_first_page + index] = page_failed;
            KLOG_WARN("pager backing unavailable object {} page {}", self->object_id(), page_index);
            return page_fault_result::failed;
        }
        auto *job = memory::New<pager_fault_job>(
            memory::KernelCommonAllocatorV, handle_t<memory_object>(backing.get_control()),
            handle_t<task::process_object>::make(task::current_process()), request_first_page, request_page_count);
        if (job == nullptr)
        {
            uctx::RawSpinLockUninterruptibleContext context(self->lock_);
            for (u64 index = 0; index < request_page_count; index++)
                self->page_states_[request_first_page + index] = page_failed;
            KLOG_WARN("pager job allocation failed object {} page {}", self->object_id(), page_index);
            return page_fault_result::failed;
        }
        const bool blocked = self->pager_wait_queue_.block_current([self, page_index] {
            uctx::RawSpinLockUninterruptibleContext context(self->lock_);
            return self->page_states_[page_index] == page_ready || self->page_states_[page_index] == page_failed;
        });
        if (!blocked)
        {
            memory::Delete<>(memory::KernelCommonAllocatorV, job);
            continue;
        }
        pager_queue->enqueue(job);
        return page_fault_result::blocked;
    }
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
    bool pager_backed = false;
    {
        uctx::RawSpinLockUninterruptibleContext context(lock_);
        if (!readable_locked(offset, size))
            return NA_STATUS_INVALID_ARGUMENT;
        pager_backed = pager_backed_locked();
    }
    for (u64 done = 0; pager_backed && done < size;)
    {
        const u64 page_offset = (offset + done) & ~(memory::page_size - 1);
        if (fault_page(page_offset) != page_fault_result::ready)
            return NA_STATUS_IO_ERROR;
        const u64 chunk = memory::page_size - ((offset + done) & (memory::page_size - 1)) < size - done
                              ? memory::page_size - ((offset + done) & (memory::page_size - 1))
                              : size - done;
        done += chunk;
    }
    uctx::RawSpinLockUninterruptibleContext context(lock_);
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

void init_pager_worker()
{
    if (pager_queue != nullptr)
        return;
    pager_queue = memory::New<pager_fault_queue>(memory::KernelCommonAllocatorV);
    if (pager_queue == nullptr)
        KLOG_PANIC("Unable to allocate pager fault queue");
    if (task::create_kernel_process(pager_worker, nullptr, 0) == nullptr)
        KLOG_PANIC("Unable to create pager fault worker");
}

} // namespace naos::data_plane
