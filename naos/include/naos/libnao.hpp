#pragma once

#include <cstddef>
#include <cstdint>
#include <time.h>
#include <utility>

#include <naos/syscall.h>

namespace nao
{

/// Small, allocation-free epoll primitives for freestanding userland.
/// The kernel owns the readiness queue; this wrapper only keeps the syscall
/// contract typed and makes invocation waits one-shot.
class event_loop
{
  public:
    event_loop() = default;
    event_loop(const event_loop &) = delete;
    event_loop &operator=(const event_loop &) = delete;

    static na_status_t create(na_handle_t &epoll)
    {
        epoll = NA_HANDLE_INVALID;
        return static_cast<na_status_t>(_na_epoll_create(&epoll));
    }

    static na_status_t control(na_handle_t epoll, std::uint32_t operation, na_handle_t target,
                               const na_epoll_event_t *event)
    {
        if (epoll == NA_HANDLE_INVALID || target == NA_HANDLE_INVALID)
            return NA_STATUS_INVALID_HANDLE;
        return static_cast<na_status_t>(_na_epoll_ctl(epoll, operation, target, event));
    }

    static na_status_t wait(na_handle_t epoll, na_epoll_event_t *events, std::uint64_t capacity, std::uint64_t &actual,
                            const struct timespec *deadline = nullptr)
    {
        actual = 0;
        return static_cast<na_status_t>(_na_epoll_wait(epoll, events, capacity, &actual, deadline));
    }

    static na_status_t cancel(na_handle_t invocation)
    {
        if (invocation == NA_HANDLE_INVALID)
            return NA_STATUS_INVALID_HANDLE;
        return static_cast<na_status_t>(_na_invocation_cancel(invocation));
    }

    static na_status_t submit(na_handle_t target, const na_submit_frame_t &frame, na_handle_t &invocation)
    {
        invocation = NA_HANDLE_INVALID;
        if (target == NA_HANDLE_INVALID)
            return NA_STATUS_INVALID_HANDLE;
        return static_cast<na_status_t>(_na_invoke_submit(target, &frame, &invocation));
    }

    static na_status_t take(na_handle_t invocation, na_result_frame_t &frame)
    {
        if (invocation == NA_HANDLE_INVALID)
            return NA_STATUS_INVALID_HANDLE;
        return static_cast<na_status_t>(_na_invocation_take_result(invocation, &frame));
    }

    static na_status_t wait_invocation(na_handle_t invocation, const struct timespec *deadline = nullptr)
    {
        if (invocation == NA_HANDLE_INVALID)
            return NA_STATUS_INVALID_HANDLE;
        na_handle_t epoll = NA_HANDLE_INVALID;
        auto status = create(epoll);
        if (status != NA_STATUS_OK)
            return status;
        const na_epoll_event_t event{NA_EPOLL_EVENT_READABLE, 0};
        status = control(epoll, NA_EPOLL_CTL_ADD, invocation, &event);
        if (status == NA_STATUS_OK)
        {
            na_epoll_event_t returned{};
            std::uint64_t actual = 0;
            status = wait(epoll, &returned, 1, actual, deadline);
            if (status == NA_STATUS_OK && (actual == 0 || (returned.events & NA_EPOLL_EVENT_READABLE) == 0))
                status = NA_STATUS_WOULD_BLOCK;
        }
        (void)_na_handle_close(epoll);
        return status;
    }
};

/// A move-only handle owner for resources returned by an invocation or
/// received from a channel.  Ownership is explicit at API boundaries and
/// never depends on a hidden global resource list.
class handle
{
  public:
    handle() = default;
    explicit handle(na_handle_t value)
        : value_(value)
    {
    }
    handle(const handle &) = delete;
    handle &operator=(const handle &) = delete;
    handle(handle &&other) noexcept
        : value_(other.release())
    {
    }
    handle &operator=(handle &&other) noexcept
    {
        if (this != &other)
        {
            reset();
            value_ = other.release();
        }
        return *this;
    }
    ~handle() { reset(); }

    na_handle_t get() const { return value_; }
    bool valid() const { return value_ != NA_HANDLE_INVALID; }
    na_handle_t release()
    {
        const auto value = value_;
        value_ = NA_HANDLE_INVALID;
        return value;
    }
    void reset(na_handle_t value = NA_HANDLE_INVALID)
    {
        if (value_ != NA_HANDLE_INVALID)
            (void)_na_handle_close(value_);
        value_ = value;
    }

    na_status_t duplicate(na_meta_rights_t rights, handle &result) const
    {
        result.reset();
        if (!valid())
            return NA_STATUS_INVALID_HANDLE;
        na_handle_t duplicate_handle = NA_HANDLE_INVALID;
        const auto status = _na_handle_duplicate(value_, rights, &duplicate_handle);
        if (status == NA_STATUS_OK)
            result = handle(duplicate_handle);
        return status;
    }

    na_status_t restrict(const na_handle_restriction_t &restriction, handle &result) const
    {
        result.reset();
        if (!valid())
            return NA_STATUS_INVALID_HANDLE;
        na_handle_t restricted_handle = NA_HANDLE_INVALID;
        const auto status = _na_handle_restrict(value_, &restriction, &restricted_handle);
        if (status == NA_STATUS_OK)
            result = handle(restricted_handle);
        return status;
    }

  private:
    na_handle_t value_ = NA_HANDLE_INVALID;
};

/// A user-space mapping of a NaOS MemoryObject. The mapping owns only the
/// virtual address; the MemoryObject handle remains owned by memory_object.
class mapped_memory
{
  public:
    mapped_memory() = default;
    mapped_memory(const mapped_memory &) = delete;
    mapped_memory &operator=(const mapped_memory &) = delete;
    mapped_memory(mapped_memory &&other) noexcept
        : address_(other.address_)
        , map_address_(other.map_address_)
        , length_(other.length_)
        , map_length_(other.map_length_)
    {
        other.address_ = nullptr;
        other.map_address_ = nullptr;
        other.length_ = 0;
        other.map_length_ = 0;
    }
    mapped_memory &operator=(mapped_memory &&other) noexcept
    {
        if (this != &other)
        {
            reset();
            address_ = other.address_;
            map_address_ = other.map_address_;
            length_ = other.length_;
            map_length_ = other.map_length_;
            other.address_ = nullptr;
            other.map_address_ = nullptr;
            other.length_ = 0;
            other.map_length_ = 0;
        }
        return *this;
    }
    ~mapped_memory() { reset(); }

    static na_status_t map(na_handle_t object, std::uint64_t length, std::uint32_t flags, mapped_memory &result)
    {
        return map(object, 0, length, flags, result);
    }

    static na_status_t map(na_handle_t object, std::uint64_t offset, std::uint64_t length, std::uint32_t flags,
                           mapped_memory &result)
    {
        result.reset();
        if (object == NA_HANDLE_INVALID || length == 0)
            return NA_STATUS_INVALID_ARGUMENT;
        na_memory_map_frame_t frame{};
        frame.struct_size = sizeof(frame);
        frame.flags = flags;
        frame.object = object;
        frame.offset = offset;
        frame.length = length;
        const auto status = _na_memory_map(&frame);
        if (status != NA_STATUS_OK)
            return status;
        constexpr std::uint64_t page_size = 4096;
        const auto delta = frame.data_offset;
        if (delta >= page_size || length > UINT64_MAX - delta)
        {
            na_memory_unmap_frame_t unmap{};
            unmap.struct_size = sizeof(unmap);
            unmap.address = frame.address;
            unmap.length = (length + delta + page_size - 1) & ~(page_size - 1);
            (void)_na_memory_unmap(&unmap);
            return NA_STATUS_IO_ERROR;
        }
        result.address_ = reinterpret_cast<void *>(reinterpret_cast<std::uintptr_t>(frame.address) + delta);
        result.map_address_ = reinterpret_cast<void *>(frame.address);
        result.length_ = length;
        result.map_length_ = (length + delta + page_size - 1) & ~(page_size - 1);
        return NA_STATUS_OK;
    }

    void *data() const { return address_; }
    std::uint64_t size() const { return length_; }
    explicit operator bool() const { return address_ != nullptr; }

    void reset()
    {
        if (address_ != nullptr)
        {
            na_memory_unmap_frame_t frame{};
            frame.struct_size = sizeof(frame);
            frame.address = reinterpret_cast<std::uint64_t>(map_address_);
            frame.length = map_length_;
            (void)_na_memory_unmap(&frame);
        }
        address_ = nullptr;
        map_address_ = nullptr;
        length_ = 0;
        map_length_ = 0;
    }

  private:
    void *address_ = nullptr;
    void *map_address_ = nullptr;
    std::uint64_t length_ = 0;
    std::uint64_t map_length_ = 0;
};

/// A bounded MemoryObject capability. The range is capability metadata over
/// an existing storage identity; constructing one does not allocate pages or
/// create another kernel object.
class memory_view
{
  public:
    memory_view() = default;
    memory_view(handle &&object, std::uint64_t length)
        : object_(std::move(object))
        , length_(length)
    {
    }
    memory_view(const memory_view &) = delete;
    memory_view &operator=(const memory_view &) = delete;
    memory_view(memory_view &&) noexcept = default;
    memory_view &operator=(memory_view &&) noexcept = default;

    bool valid() const { return object_.valid(); }
    na_handle_t get() const { return object_.get(); }
    std::uint64_t size() const { return length_; }

    mapped_memory map(std::uint64_t offset, std::uint64_t length,
                      std::uint32_t flags = NA_MEMORY_MAP_READ | NA_MEMORY_MAP_WRITE | NA_MEMORY_MAP_SHARED) const
    {
        mapped_memory result;
        if (offset > length_ || length == 0 || length > length_ - offset ||
            mapped_memory::map(object_.get(), offset, length, flags, result) != NA_STATUS_OK)
            return {};
        return result;
    }

    mapped_memory map(std::uint32_t flags = NA_MEMORY_MAP_READ | NA_MEMORY_MAP_WRITE | NA_MEMORY_MAP_SHARED) const
    {
        return map(0, length_, flags);
    }

    memory_view subspan(std::uint64_t offset, std::uint64_t length) const
    {
        if (!object_.valid() || length == 0 || offset > length_ || length > length_ - offset)
            return {};
        handle duplicate;
        if (object_.duplicate(0, duplicate) != NA_STATUS_OK)
            return {};
        na_handle_restriction_t restriction{};
        restriction.struct_size = sizeof(restriction);
        restriction.flags = NA_RESTRICTION_RANGE;
        restriction.view_offset = offset;
        restriction.view_length = length;
        handle restricted;
        if (duplicate.restrict(restriction, restricted) != NA_STATUS_OK)
            return {};
        return memory_view(std::move(restricted), length);
    }

    memory_view slice(std::uint64_t offset, std::uint64_t length) const { return subspan(offset, length); }

  private:
    handle object_;
    std::uint64_t length_ = 0;
};

/// Typed ownership for a MemoryObject capability. Data-plane users map the
/// object and access the returned address directly instead of issuing a
/// buffer read/write RPC for every operation.
class memory_object
{
  public:
    memory_object() = default;
    explicit memory_object(handle &&object)
        : object_(std::move(object))
    {
    }
    memory_object(const memory_object &) = delete;
    memory_object &operator=(const memory_object &) = delete;
    memory_object(memory_object &&) noexcept = default;
    memory_object &operator=(memory_object &&) noexcept = default;

    bool valid() const { return object_.valid(); }
    na_handle_t get() const { return object_.get(); }
    memory_view subspan(std::uint64_t offset, std::uint64_t length) const
    {
        if (!object_.valid())
            return {};
        if (length == 0)
            return {};
        handle duplicate;
        if (object_.duplicate(0, duplicate) != NA_STATUS_OK)
            return {};
        na_handle_restriction_t restriction{};
        restriction.struct_size = sizeof(restriction);
        restriction.flags = NA_RESTRICTION_RANGE;
        restriction.view_offset = offset;
        restriction.view_length = length;
        handle restricted;
        if (duplicate.restrict(restriction, restricted) != NA_STATUS_OK)
            return {};
        return memory_view(std::move(restricted), length);
    }
    memory_view slice(std::uint64_t offset, std::uint64_t length) const { return subspan(offset, length); }

    mapped_memory map(std::uint64_t length,
                      std::uint32_t flags = NA_MEMORY_MAP_READ | NA_MEMORY_MAP_WRITE | NA_MEMORY_MAP_SHARED) const
    {
        mapped_memory result;
        if (mapped_memory::map(object_.get(), length, flags, result) != NA_STATUS_OK)
            return {};
        return result;
    }

    mapped_memory map(std::uint64_t offset, std::uint64_t length,
                      std::uint32_t flags = NA_MEMORY_MAP_READ | NA_MEMORY_MAP_WRITE | NA_MEMORY_MAP_SHARED) const
    {
        mapped_memory result;
        if (mapped_memory::map(object_.get(), offset, length, flags, result) != NA_STATUS_OK)
            return {};
        return result;
    }

  private:
    handle object_;
};

/// Thin channel transport used by event-loop continuations. It deliberately
/// exposes the native frames so generated IDL remains the type-safe layer.
class channel
{
  public:
    channel() = default;
    explicit channel(na_handle_t endpoint)
        : endpoint_(endpoint)
    {
    }

    na_handle_t get() const { return endpoint_; }
    na_status_t send(const na_channel_send_frame_t &frame) const
    {
        if (endpoint_ == NA_HANDLE_INVALID)
            return NA_STATUS_INVALID_HANDLE;
        return static_cast<na_status_t>(_na_channel_send(endpoint_, &frame));
    }
    na_status_t receive(na_channel_receive_frame_t &frame) const
    {
        if (endpoint_ == NA_HANDLE_INVALID)
            return NA_STATUS_INVALID_HANDLE;
        return static_cast<na_status_t>(_na_channel_receive(endpoint_, &frame));
    }
    na_status_t discard() const
    {
        if (endpoint_ == NA_HANDLE_INVALID)
            return NA_STATUS_INVALID_HANDLE;
        return static_cast<na_status_t>(_na_channel_discard(endpoint_));
    }

  private:
    na_handle_t endpoint_ = NA_HANDLE_INVALID;
};

template <std::size_t Capacity> class task_queue
{
  public:
    using callback = void (*)(void *);

    bool push(callback function, void *context)
    {
        if (function == nullptr || size_ == Capacity)
            return false;
        tasks_[tail_] = {function, context};
        tail_ = (tail_ + 1) % Capacity;
        size_++;
        return true;
    }

    bool run_one()
    {
        if (size_ == 0)
            return false;
        const auto task = tasks_[head_];
        tasks_[head_] = {};
        head_ = (head_ + 1) % Capacity;
        size_--;
        task.function(task.context);
        return true;
    }

    std::size_t run(std::size_t budget)
    {
        std::size_t ran = 0;
        while (ran < budget && run_one())
            ran++;
        return ran;
    }

    std::size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }

  private:
    struct task
    {
        callback function = nullptr;
        void *context = nullptr;
    };

    task tasks_[Capacity == 0 ? 1 : Capacity]{};
    std::size_t head_ = 0;
    std::size_t tail_ = 0;
    std::size_t size_ = 0;
};

template <std::size_t Capacity> class timer_queue
{
  public:
    bool arm(std::uint64_t id, std::uint64_t deadline_ms)
    {
        for (std::size_t i = 0; i < size_; i++)
        {
            if (timers_[i].id == id)
            {
                timers_[i].deadline_ms = deadline_ms;
                return true;
            }
        }
        if (size_ == Capacity)
            return false;
        timers_[size_++] = {id, deadline_ms};
        return true;
    }

    bool cancel(std::uint64_t id)
    {
        for (std::size_t i = 0; i < size_; i++)
        {
            if (timers_[i].id != id)
                continue;
            timers_[i] = timers_[--size_];
            return true;
        }
        return false;
    }

    bool pop_expired(std::uint64_t now_ms, std::uint64_t &id)
    {
        for (std::size_t i = 0; i < size_; i++)
        {
            if (timers_[i].deadline_ms > now_ms)
                continue;
            id = timers_[i].id;
            timers_[i] = timers_[--size_];
            return true;
        }
        return false;
    }

    bool next_deadline(std::uint64_t &deadline_ms) const
    {
        if (size_ == 0)
            return false;
        deadline_ms = timers_[0].deadline_ms;
        for (std::size_t i = 1; i < size_; i++)
            if (timers_[i].deadline_ms < deadline_ms)
                deadline_ms = timers_[i].deadline_ms;
        return true;
    }

  private:
    struct timer
    {
        std::uint64_t id = 0;
        std::uint64_t deadline_ms = 0;
    };

    timer timers_[Capacity == 0 ? 1 : Capacity]{};
    std::size_t size_ = 0;
};

/// Owns one invocation handle and makes cancellation-before-close explicit.
class invocation
{
  public:
    invocation() = default;
    explicit invocation(na_handle_t handle)
        : handle_(handle)
    {
    }
    invocation(const invocation &) = delete;
    invocation &operator=(const invocation &) = delete;
    invocation(invocation &&other) noexcept
        : handle_(other.release())
    {
    }
    invocation &operator=(invocation &&other) noexcept
    {
        if (this != &other)
        {
            reset();
            handle_ = other.release();
        }
        return *this;
    }
    ~invocation() { reset(); }

    na_handle_t get() const { return handle_; }
    bool valid() const { return handle_ != NA_HANDLE_INVALID; }
    na_status_t cancel() { return event_loop::cancel(handle_); }
    na_handle_t release()
    {
        const auto result = handle_;
        handle_ = NA_HANDLE_INVALID;
        return result;
    }
    void reset()
    {
        if (handle_ == NA_HANDLE_INVALID)
            return;
        (void)event_loop::cancel(handle_);
        (void)_na_handle_close(handle_);
        handle_ = NA_HANDLE_INVALID;
    }

  private:
    na_handle_t handle_ = NA_HANDLE_INVALID;
};

} // namespace nao
