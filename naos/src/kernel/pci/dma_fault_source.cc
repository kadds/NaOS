#include "kernel/pci/dma_fault_source.hpp"

#include "kernel/ucontext.hpp"

namespace pci
{
dma_fault_source::dma_fault_source(process_id owner, function_identity identity)
    : kobject(type_of())
    , owner_(owner)
    , identity_(identity)
{
}

u16 dma_fault_source::requester_id(const function_identity &identity) noexcept
{
    return static_cast<u16>((static_cast<u16>(identity.bus) << 8) | (static_cast<u16>(identity.device) << 3) |
                            static_cast<u16>(identity.function));
}

bool dma_fault_source::readable_locked() const noexcept
{
    return count_ != 0 || dropped_records_ != 0 || unit_overflow_;
}

bool dma_fault_source::push_fault(const dma_fault_record &record) noexcept
{
    if (record.requester_id != requester_id(identity_))
        return false;

    bool became_readable = false;
    bool recorded = true;
    {
        uctx::RawSpinLockUninterruptibleContext guard(state_lock_);
        if (closed_)
            return false;

        const bool was_readable = readable_locked();
        if (sequence_ != ~u64{0})
            sequence_++;
        if (count_ == max_dma_fault_queue_records)
        {
            if (dropped_records_ != ~u32{0})
                dropped_records_++;
            recorded = false;
        }
        else
        {
            const u32 tail = (head_ + count_) % max_dma_fault_queue_records;
            records_[tail] = record;
            count_++;
        }
        became_readable = !was_readable && readable_locked();
    }

    if (became_readable)
        notify_readiness();
    return recorded;
}

bool dma_fault_source::note_unit_overflow() noexcept
{
    bool became_readable = false;
    {
        uctx::RawSpinLockUninterruptibleContext guard(state_lock_);
        if (closed_)
            return false;

        const bool was_readable = readable_locked();
        if (!unit_overflow_)
        {
            unit_overflow_ = true;
            if (sequence_ != ~u64{0})
                sequence_++;
        }
        became_readable = !was_readable && readable_locked();
    }

    if (became_readable)
        notify_readiness();
    return true;
}

na_status_t dma_fault_source::take_faults(process_id caller, u32 limit, dma_fault_batch &batch) noexcept
{
    batch = {};
    if (caller != owner_)
        return NA_STATUS_ACCESS_DENIED;
    if (limit == 0 || limit > max_dma_fault_batch_records)
        return NA_STATUS_INVALID_ARGUMENT;
    batch.boot_epoch = identity_.boot_epoch;
    batch.firmware_generation = identity_.firmware_generation;
    batch.aml_namespace_generation = identity_.aml_namespace_generation;
    batch.platform_topology_generation = identity_.platform_topology_generation;
    batch.pci_fabric_generation = identity_.pci_fabric_generation;

    bool readiness_changed = false;
    na_status_t status = NA_STATUS_OK;
    {
        uctx::RawSpinLockUninterruptibleContext guard(state_lock_);
        const bool was_readable = readable_locked();
        // A closed source may still own queued events. Return those before
        // reporting revocation; the IPC adapter otherwise drops the batch on
        // any non-OK status.
        const bool had_pending_data = was_readable;
        const u32 count = count_ < limit ? count_ : limit;
        batch.sequence = sequence_;
        batch.count = count;
        batch.dropped_records = dropped_records_;
        batch.unit_overflow = unit_overflow_;
        dropped_records_ = 0;
        unit_overflow_ = false;
        for (u32 index = 0; index < count; ++index)
        {
            batch.records[index] = records_[head_];
            head_ = (head_ + 1) % max_dma_fault_queue_records;
        }
        count_ -= count;
        if (count_ == 0)
            head_ = 0;

        const bool is_readable = readable_locked();
        readiness_changed = was_readable != is_readable;
        if (closed_ && !had_pending_data)
            status = NA_STATUS_OBJECT_REVOKED;
    }

    if (readiness_changed)
        notify_readiness();
    return status;
}

void dma_fault_source::close() noexcept
{
    bool changed = false;
    {
        uctx::RawSpinLockUninterruptibleContext guard(state_lock_);
        changed = !closed_;
        closed_ = true;
    }
    if (changed)
        notify_readiness();
}

na_signal_t dma_fault_source::capability_signals() const
{
    uctx::RawSpinLockUninterruptibleContext guard(state_lock_);
    na_signal_t signals = readable_locked() ? NA_SIGNAL_READABLE : 0;
    if (closed_)
        signals |= NA_SIGNAL_OBJECT_REVOKED;
    return signals;
}

u64 dma_fault_source::capability_state() const
{
    uctx::RawSpinLockUninterruptibleContext guard(state_lock_);
    return sequence_;
}
} // namespace pci
