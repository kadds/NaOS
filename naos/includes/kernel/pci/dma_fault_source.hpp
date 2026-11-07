#pragma once

#include "kernel/kobject.hpp"
#include "kernel/pci/discovery_fabric.hpp"
#include "kernel/types.hpp"
#include "naos/abi.h"

namespace pci
{
inline constexpr u32 max_dma_fault_batch_records = 16;
inline constexpr u32 max_dma_fault_queue_records = 32;

enum class dma_fault_direction : u8
{
    device_read = 0,
    device_write = 1,
    unknown = 2,
};

struct dma_fault_record
{
    u64 iova = 0;
    u16 requester_id = 0;
    // Raw backend-specific event reason; it is not a cross-vendor enum.
    u8 reason = 0;
    dma_fault_direction direction = dma_fault_direction::unknown;
};

struct dma_fault_batch
{
    u64 sequence = 0;
    u32 count = 0;
    u32 dropped_records = 0;
    bool unit_overflow = false;
    u64 boot_epoch = 0;
    u64 firmware_generation = 0;
    u64 aml_namespace_generation = 0;
    u64 platform_topology_generation = 0;
    u64 pci_fabric_generation = 0;
    dma_fault_record records[max_dma_fault_batch_records]{};
};

/// Move-only, function-scoped DMA fault stream. Records are published by the
/// selected IOMMU backend after its hardware event log is drained; readiness
/// is exposed through the ordinary capability/epoll contract.
class dma_fault_source final : public kobject
{
  public:
    static constexpr type_e type_of() { return type_e::pci_dma_fault_source; }

    dma_fault_source(process_id owner, function_identity identity);
    process_id owner() const noexcept { return owner_; }

    bool capability_is_unique() const override { return true; }
    na_signal_t capability_signals() const override;
    u64 capability_state() const override;

    /// Kernel-backend entry point. Records for other requesters and records
    /// dropped by the bounded queue are reported as false.
    bool push_fault(const dma_fault_record &record) noexcept;
    /// Report the IOMMU unit's requester-agnostic event-log overflow.
    bool note_unit_overflow() noexcept;
    na_status_t take_faults(process_id caller, u32 limit, dma_fault_batch &batch) noexcept;
    void close() noexcept;

  private:
    static u16 requester_id(const function_identity &identity) noexcept;
    bool readable_locked() const noexcept;

    process_id owner_;
    function_identity identity_;
    mutable lock::spinlock_t state_lock_;
    dma_fault_record records_[max_dma_fault_queue_records]{};
    u32 head_ = 0;
    u32 count_ = 0;
    u32 dropped_records_ = 0;
    u64 sequence_ = 0;
    bool unit_overflow_ = false;
    bool closed_ = false;
};
} // namespace pci
