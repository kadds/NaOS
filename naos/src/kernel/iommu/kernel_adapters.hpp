#pragma once

#include "kernel/iommu/fault_dispatch.hpp"
#include <atomic>

namespace iommu
{
/// A nonblocking reservation for adapter-owned fault scratch. A concurrent
/// drain returns a retry indication instead of waiting while holding a lock.
class fault_report_slot_lease
{
  public:
    explicit fault_report_slot_lease(std::atomic_bool &busy) noexcept
        : busy_(busy)
    {
        bool expected = false;
        acquired_ = busy_.compare_exchange_strong(expected, true, std::memory_order_acquire, std::memory_order_relaxed);
    }

    ~fault_report_slot_lease()
    {
        if (acquired_)
            busy_.store(false, std::memory_order_release);
    }

    explicit operator bool() const noexcept { return acquired_; }

  private:
    std::atomic_bool &busy_;
    bool acquired_ = false;
};
} // namespace iommu

namespace iommu::intel_vtd
{
bool kernel_drain_fault_events(u32 unit_index, const fault_quiesce_ops &quiesce_ops, bool log_records,
                               fault_drain_report &report);
provider_initialization_report kernel_initialize_provider(const provider_bootstrap_config &configuration);
} // namespace iommu::intel_vtd

namespace iommu::amd_vi
{
bool kernel_drain_fault_events(u32 unit_index, const fault_quiesce_ops &quiesce_ops, bool log_records,
                               fault_drain_report &report);
bool kernel_quarantine_fault_units(const fault_quiesce_ops &quiesce_ops);
provider_initialization_report kernel_initialize_provider(const provider_bootstrap_config &configuration);
} // namespace iommu::amd_vi
