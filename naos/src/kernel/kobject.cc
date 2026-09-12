#include "kernel/kobject.hpp"

#include "kernel/mm/memory.hpp"
#include "kernel/mutex.hpp"
#include "kernel/ucontext.hpp"

namespace
{
lock::mutex_t *global_readiness_allocation_lock()
{
    // Static storage avoids both a per-kobject allocation and a freestanding
    // static destructor.  The mutex is constructed on first use, after the
    // kernel allocator and wait-queue implementation are available.
    alignas(lock::mutex_t) static unsigned char storage[sizeof(lock::mutex_t)];
    static std::atomic<lock::mutex_t *> value{nullptr};
    static std::atomic_flag initializing = ATOMIC_FLAG_INIT;
    auto *result = value.load(std::memory_order_acquire);
    if (result != nullptr)
        return result;
    while (initializing.test_and_set(std::memory_order_acquire))
    {
        result = value.load(std::memory_order_acquire);
        if (result != nullptr)
            return result;
    }
    result = value.load(std::memory_order_relaxed);
    if (result == nullptr)
    {
        result = new (storage) lock::mutex_t();
        value.store(result, std::memory_order_release);
    }
    initializing.clear(std::memory_order_release);
    return result;
}
} // namespace

namespace kobject_detail
{
void readiness_allocation_lock::lock() { global_readiness_allocation_lock()->lock(); }

void readiness_allocation_lock::unlock() { global_readiness_allocation_lock()->unlock(); }
} // namespace kobject_detail

kobject::kobject(type_e ty)
    : ty(ty)
    , object_id_(kobject_detail::next_object_id.fetch_add(1, std::memory_order_relaxed))
    , readiness_watchers_(memory::KernelCommonAllocatorV)
{
}

bool kobject::add_readiness_watcher(readiness_sink *sink, u64 token)
{
    if (sink == nullptr)
        return false;
    {
        uctx::RawSpinLockUninterruptibleContext guard(readiness_lock_);
        for (auto &current : readiness_watchers_)
        {
            if (current.sink == sink && current.token == token)
                return true;
        }
    }
    readiness_allocation_lock_.lock();
    u64 required_capacity = 0;
    {
        uctx::RawSpinLockUninterruptibleContext guard(readiness_lock_);
        for (auto &current : readiness_watchers_)
        {
            if (current.sink == sink && current.token == token)
            {
                readiness_allocation_lock_.unlock();
                return true;
            }
        }
        required_capacity = readiness_watchers_.size() + 1;
    }
    readiness_watchers_.ensure(required_capacity);
    const bool capacity_ready = readiness_watchers_.capacity() >= required_capacity;
    {
        uctx::RawSpinLockUninterruptibleContext guard(readiness_lock_);
        // A concurrent add may have installed the same watcher while this
        // thread was preparing capacity.  Do not duplicate it.
        for (auto &current : readiness_watchers_)
        {
            if (current.sink == sink && current.token == token)
            {
                readiness_allocation_lock_.unlock();
                return true;
            }
        }
        if (!capacity_ready)
        {
            readiness_allocation_lock_.unlock();
            return false;
        }
        readiness_watchers_.push_back(readiness_watcher{sink, token});
    }
    readiness_allocation_lock_.unlock();
    return true;
}

void kobject::remove_readiness_watcher(readiness_sink *sink, u64 token)
{
    uctx::RawSpinLockUninterruptibleContext guard(readiness_lock_);
    for (u64 index = 0; index < readiness_watchers_.size(); index++)
    {
        if (readiness_watchers_[index].sink == sink && readiness_watchers_[index].token == token)
        {
            readiness_watchers_.remove(readiness_watchers_.begin() + index);
            return;
        }
    }
}

void kobject::notify_readiness()
{
    // Notifications are on the hot path.  Keep the watcher list locked while
    // dispatching it so no temporary kernel allocation is needed for every
    // state transition.  A sink only records the token and wakes its own wait
    // queue; it does not call back into this object.
    readiness_epoch_.fetch_add(1, std::memory_order_release);
    uctx::RawSpinLockUninterruptibleContext guard(readiness_lock_);
    for (auto &watcher : readiness_watchers_)
    {
        if (watcher.sink != nullptr)
            watcher.sink->on_readiness_change(watcher.token);
    }
}
