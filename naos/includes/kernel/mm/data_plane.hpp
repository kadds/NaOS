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
    memory_object(u64 size, u32 flags);
    ~memory_object() override = default;

    static type_e type_of() { return type_e::memory_object; }

    bool readable(u64 offset, u64 size) const;
    bool writable(u64 offset, u64 size) const;
    na_status_t read(u64 offset, byte *destination, u64 size, u64 &actual) const;
    na_status_t write(u64 offset, const byte *source, u64 size, u64 &actual);

    u64 size() const;
    u32 flags() const { return flags_; }

  private:
    mutable lock::spinlock_t lock_;
    freelibcxx::vector<byte> bytes_;
    u32 flags_;
};

class shared_ring final : public kobject
{
  public:
    shared_ring(u64 slots, u64 slot_bytes, u32 flags);
    ~shared_ring() override = default;

    static type_e type_of() { return type_e::shared_ring; }

    bool valid() const { return valid_; }
    u64 slots() const { return slots_.size(); }
    u64 slot_bytes() const { return slot_bytes_; }
    u64 queued() const;
    na_signal_t capability_signals() const override;

    na_status_t push(freelibcxx::vector<byte> &&bytes);
    na_status_t claim_pop(freelibcxx::vector<byte> &snapshot);
    void cancel_pop();
    na_status_t commit_pop();

  private:
    struct slot
    {
        explicit slot(freelibcxx::Allocator *allocator)
            : bytes(allocator)
        {
        }

        freelibcxx::vector<byte> bytes;
    };

    mutable lock::spinlock_t lock_;
    freelibcxx::vector<slot> slots_;
    u64 slot_bytes_;
    u64 head_;
    u64 tail_;
    u64 count_;
    bool pop_claimed_;
    bool valid_;
    u32 flags_;
};

} // namespace naos::data_plane
