#pragma once

#include "kernel/common.hpp"
#include <utility>

namespace naos::ipc
{

/// A fixed-storage FIFO with an explicit queue-head claim.
///
/// The channel implementation owns the storage and performs synchronization
/// around this class. Keeping the claim state here makes it impossible for a
/// receive and a discard operation to consume the same message.
template <typename T> class bounded_queue
{
  public:
    bounded_queue(T *storage, u64 capacity)
        : storage_(storage)
        , capacity_(capacity)
        , head_(0)
        , count_(0)
        , claimed_(false)
    {
    }

    bounded_queue(const bounded_queue &) = delete;
    bounded_queue &operator=(const bounded_queue &) = delete;

    bool try_push(T value)
    {
        if (storage_ == nullptr || capacity_ == 0 || count_ == capacity_)
            return false;

        storage_[(head_ + count_) % capacity_] = std::move(value);
        count_++;
        return true;
    }

    bool claim_front()
    {
        if (count_ == 0 || claimed_)
            return false;

        claimed_ = true;
        return true;
    }

    /// Remove an unclaimed entry while preserving the FIFO order.
    ///
    /// Channel cancellation uses this to discard a queued request before it
    /// becomes visible to the receiver.  A claimed head belongs to a receive
    /// transaction and must not be removed by a concurrent cancellation;
    /// unclaimed entries behind it may still be removed safely.
    bool remove(T value)
    {
        if (count_ == 0)
            return false;

        u64 offset = 0;
        while (offset < count_ && storage_[(head_ + offset) % capacity_] != value)
            offset++;
        if (offset == count_ || (claimed_ && offset == 0))
            return false;

        for (u64 current = offset; current + 1 < count_; current++)
            storage_[(head_ + current) % capacity_] = std::move(storage_[(head_ + current + 1) % capacity_]);
        storage_[(head_ + count_ - 1) % capacity_] = T{};
        count_--;
        return true;
    }

    T &front() { return storage_[head_]; }
    const T &front() const { return storage_[head_]; }

    T &at(u64 offset) { return storage_[(head_ + offset) % capacity_]; }
    const T &at(u64 offset) const { return storage_[(head_ + offset) % capacity_]; }

    void cancel_claim() { claimed_ = false; }

    void commit_claim()
    {
        if (!claimed_ || count_ == 0)
            return;

        head_ = (head_ + 1) % capacity_;
        count_--;
        claimed_ = false;
    }

    bool is_claimed() const { return claimed_; }
    bool empty() const { return count_ == 0; }
    bool full() const { return capacity_ != 0 && count_ == capacity_; }
    u64 size() const { return count_; }
    u64 capacity() const { return capacity_; }

  private:
    T *storage_;
    u64 capacity_;
    u64 head_;
    u64 count_;
    bool claimed_;
};

} // namespace naos::ipc
