#include "kernel/mm/data_plane.hpp"

#include "kernel/ipc/channel.hpp"
#include "kernel/mm/memory.hpp"

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
} // namespace

memory_object::memory_object(u64 size, u32 flags)
    : kobject(type_of())
    , bytes_(memory::MemoryAllocatorV)
    , flags_(flags)
{
    if (size <= NA_MEMORY_OBJECT_MAX_BYTES)
    {
        resize_bytes(bytes_, size);
        if (size == 0 || bytes_.data() != nullptr)
            return;
    }
    bytes_.clear();
}

bool memory_object::readable(u64 offset, u64 size) const { return valid_extent(offset, size, bytes_.size()); }

bool memory_object::writable(u64 offset, u64 size) const
{
    return (flags_ & NA_MEMORY_FLAG_READ_ONLY) == 0 && valid_extent(offset, size, bytes_.size());
}

na_status_t memory_object::read(u64 offset, byte *destination, u64 size, u64 &actual) const
{
    actual = 0;
    if (size != 0 && destination == nullptr)
        return NA_STATUS_INVALID_ARGUMENT;
    uctx::RawSpinLockUninterruptibleContext context(lock_);
    if (!valid_extent(offset, size, bytes_.size()))
        return NA_STATUS_INVALID_ARGUMENT;
    copy_bytes(destination, bytes_.data() + offset, size);
    actual = size;
    return NA_STATUS_OK;
}

na_status_t memory_object::write(u64 offset, const byte *source, u64 size, u64 &actual)
{
    actual = 0;
    if (size != 0 && source == nullptr)
        return NA_STATUS_INVALID_ARGUMENT;
    uctx::RawSpinLockUninterruptibleContext context(lock_);
    if (!writable(offset, size))
        return (flags_ & NA_MEMORY_FLAG_READ_ONLY) != 0 ? NA_STATUS_ACCESS_DENIED : NA_STATUS_INVALID_ARGUMENT;
    copy_bytes(bytes_.data() + offset, source, size);
    actual = size;
    return NA_STATUS_OK;
}

u64 memory_object::size() const
{
    uctx::RawSpinLockUninterruptibleContext context(lock_);
    return bytes_.size();
}

shared_ring::shared_ring(u64 slots, u64 slot_bytes, u32 flags)
    : kobject(type_of())
    , slots_(memory::KernelCommonAllocatorV)
    , slot_bytes_(slot_bytes)
    , head_(0)
    , tail_(0)
    , count_(0)
    , pop_claimed_(false)
    , valid_(false)
    , flags_(flags)
{
    if (slots == 0 || slots > NA_SHARED_RING_MAX_SLOTS || slot_bytes == 0 ||
        slot_bytes > NA_SHARED_RING_MAX_SLOT_BYTES || slots > NA_SHARED_RING_MAX_BYTES / slot_bytes)
        return;
    slots_.ensure(slots);
    if (slots_.capacity() < slots)
        return;
    for (u64 i = 0; i < slots; i++)
    {
        slots_.push_back(memory::MemoryAllocatorV);
        auto &slot = slots_.back();
        resize_bytes(slot.bytes, slot_bytes);
        if (slot.bytes.data() == nullptr)
        {
            slots_.clear();
            return;
        }
    }
    valid_ = true;
}

u64 shared_ring::queued() const
{
    uctx::RawSpinLockUninterruptibleContext context(lock_);
    return count_;
}

na_signal_t shared_ring::capability_signals() const
{
    uctx::RawSpinLockUninterruptibleContext context(lock_);
    na_signal_t signals = 0;
    if (!valid_)
        return signals;
    if (count_ != 0)
        signals |= NA_SIGNAL_READABLE;
    if (count_ < slots_.size() && !pop_claimed_)
        signals |= NA_SIGNAL_WRITABLE;
    return signals;
}

na_status_t shared_ring::push(freelibcxx::vector<byte> &&bytes)
{
    if (!valid_ || bytes.size() > slot_bytes_)
        return NA_STATUS_INVALID_ARGUMENT;
    na_status_t result = NA_STATUS_OK;
    {
        uctx::RawSpinLockUninterruptibleContext context(lock_);
        if (count_ == slots_.size())
            result = NA_STATUS_WOULD_BLOCK;
        else
        {
            auto &destination = slots_[tail_].bytes;
            destination.clear();
            resize_bytes(destination, bytes.size());
            if (bytes.size() != 0 && destination.data() == nullptr)
                result = NA_STATUS_RESOURCE_EXHAUSTED;
            else
            {
                copy_bytes(destination.data(), bytes.data(), bytes.size());
                tail_ = (tail_ + 1) % slots_.size();
                count_++;
            }
        }
    }
    if (result == NA_STATUS_OK)
        ipc::notify_channel_waiters();
    return result;
}

na_status_t shared_ring::claim_pop(freelibcxx::vector<byte> &snapshot)
{
    snapshot.clear();
    uctx::RawSpinLockUninterruptibleContext context(lock_);
    if (!valid_ || count_ == 0)
        return NA_STATUS_WOULD_BLOCK;
    if (pop_claimed_)
        return NA_STATUS_WOULD_BLOCK;
    auto &source = slots_[head_].bytes;
    resize_bytes(snapshot, source.size());
    if (source.size() != 0 && snapshot.data() == nullptr)
        return NA_STATUS_RESOURCE_EXHAUSTED;
    copy_bytes(snapshot.data(), source.data(), source.size());
    pop_claimed_ = true;
    return NA_STATUS_OK;
}

void shared_ring::cancel_pop()
{
    {
        uctx::RawSpinLockUninterruptibleContext context(lock_);
        pop_claimed_ = false;
    }
    ipc::notify_channel_waiters();
}

na_status_t shared_ring::commit_pop()
{
    {
        uctx::RawSpinLockUninterruptibleContext context(lock_);
        if (!pop_claimed_ || count_ == 0)
            return NA_STATUS_INVALID_ARGUMENT;
        slots_[head_].bytes.clear();
        head_ = (head_ + 1) % slots_.size();
        count_--;
        pop_claimed_ = false;
    }
    ipc::notify_channel_waiters();
    return NA_STATUS_OK;
}

} // namespace naos::data_plane
