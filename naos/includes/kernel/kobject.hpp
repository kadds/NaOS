#pragma once
#include "freelibcxx/vector.hpp"
#include "handle.hpp"
#include "kernel/common.hpp"
#include "kernel/lock.hpp"
#include "naos/abi.h"
#include <atomic>

namespace capability
{
enum class location : u8;
}

// vm.hpp is included transitively by mutex.hpp.  Publish this alias before
// that include is expanded so the recursive kobject.hpp include still gives
// vm.hpp a complete handle_t declaration.
class kobject;
using khandle = handle_t<kobject>;

namespace kobject_detail
{
inline std::atomic_uint64_t next_object_id{1};

/// Sleepable serialization for readiness-vector preparation.  The mutex
/// implementation lives in kobject.cc so this header can still be included by
/// vm.hpp without recursively requiring wait.hpp.
class readiness_allocation_lock
{
  public:
    void lock();
    void unlock();
};
}

/// Receives readiness transitions from a kobject it is watching.
///
/// A watcher registers a (sink, token) pair so a notifying object can name the
/// exact registration that may have changed.  The sink owns whatever state the
/// token identifies; kobject never dereferences it or allocates on the notify
/// path.  The callback runs with the object's readiness lock held, so it must
/// not call back into the notifying object.
class readiness_sink
{
  public:
    virtual ~readiness_sink() = default;
    virtual void on_readiness_change(u64 token) = 0;
};

class kobject
{
  public:
    enum class type_e : u32
    {
        unknown = 0,
        file = 1,
        semaphore,
        raw_channel_end,
        protocol_descriptor,
        protocol_client_end,
        protocol_server_end,
        invocation,
        responder,
        directory,
        memory_object,
        process,
        service_directory,
        input_event_source,
        terminal_identity,
        terminal_job_control,
        terminal_driver_control,
        terminal_driver_factory,
        console_stream,
        klog_stream,
        framebuffer,
        epoll,
        system_status,
    };

    explicit kobject(type_e ty);

    virtual ~kobject() {}

    type_e get_ktype() const { return ty; }

    virtual bool capability_is_unique() const { return false; }
    virtual void on_capability_acquire(capability::location) {}
    virtual void on_capability_release(capability::location) {}
    virtual void on_capability_handoff(capability::location, capability::location) {}
    virtual na_signal_t capability_signals() const { return 0; }
    virtual u64 capability_state() const { return 0; }
    bool add_readiness_watcher(readiness_sink *sink, u64 token);
    void remove_readiness_watcher(readiness_sink *sink, u64 token);
    void notify_readiness();
    u64 readiness_epoch() const { return readiness_epoch_.load(std::memory_order_acquire); }
    u64 object_id() const { return object_id_; }

    template <typename T> T *get()
    {
        if (likely(T::type_of() == this->ty))
            return (T *)this;
        return nullptr;
    }
    template <typename T> const T *get() const
    {
        if (likely(T::type_of() == this->ty))
            return (const T *)this;
        return nullptr;
    }

    template <typename T> bool is() const { return T::type_of() == this->ty; }

    template <typename T> T *get_unsafe() { return (T *)this; }
    template <typename T> const T *get_unsafe() const { return (const T *)this; }

  private:
    type_e ty;
    u64 object_id_;
    std::atomic_uint64_t readiness_epoch_{0};
    mutable lock::spinlock_t readiness_lock_;
    // Vector growth is a sleepable operation.  Serialize the prepare phase
    // separately so add_readiness_watcher never allocates while holding the
    // non-sleepable readiness lock.
    mutable kobject_detail::readiness_allocation_lock readiness_allocation_lock_;
    struct readiness_watcher
    {
        readiness_sink *sink;
        u64 token;
    };
    freelibcxx::vector<readiness_watcher> readiness_watchers_;
};
