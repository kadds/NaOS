#pragma once

#include <cstddef>
#include <cstdint>
#include <time.h>

#include <naos/syscall.h>

namespace nao
{

/// Small, allocation-free event-loop primitives for freestanding userland.
/// The loop deliberately delegates readiness and deadlines to the native
/// handle wait operation; it does not add a second poll/watch protocol.
class event_loop
{
  public:
    event_loop() = default;
    event_loop(const event_loop &) = delete;
    event_loop &operator=(const event_loop &) = delete;

    static na_status_t wait(na_wait_item_t *items, std::uint64_t count, const struct timespec *deadline = nullptr)
    {
        if (count != 0 && items == nullptr)
            return NA_STATUS_INVALID_ARGUMENT;
        return static_cast<na_status_t>(_na_handle_wait_many(items, count, deadline));
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
        na_wait_item_t item{invocation, NA_SIGNAL_COMPLETED | NA_SIGNAL_PEER_CLOSED, 0};
        return wait(&item, 1, deadline);
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

  private:
    na_handle_t value_ = NA_HANDLE_INVALID;
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
