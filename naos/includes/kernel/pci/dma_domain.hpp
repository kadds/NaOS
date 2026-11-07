#pragma once

#include "freelibcxx/extern.hpp"
#include "kernel/mm/data_plane.hpp"
#include "kernel/pci/discovery_fabric.hpp"
#include "kernel/pci/dma_mapping_ledger.hpp"
#include "kernel/pci/dma_operation.hpp"
#include "kernel/pci/dma_quiesce.hpp"
#include "kernel/physical_resource.hpp"
#include "kernel/types.hpp"
#include "naos/abi.h"
#include <atomic>

namespace pci
{
inline constexpr na_meta_rights_t dma_capability_meta_rights = NA_RIGHT_WAIT | NA_RIGHT_INSPECT | NA_RIGHT_TRANSFER;

class dma_domain final : public kobject
{
  public:
    struct configuration
    {
        bool iommu_attached = false;
        bool trusted_driver_override = false;
        bool domain_ready = false;
        bool state_uncertain = false;
        u8 address_width = 0;
    };

    struct backend_ops
    {
        na_status_t (*map_page)(void *context, void *state, u64 iova, u64 physical_address,
                                naos::data_plane::dma_direction direction, bool &outcome_uncertain);
        na_status_t (*unmap_page)(void *context, void *state, u64 iova, bool &outcome_uncertain);
        na_status_t (*set_bus_master)(void *context, void *state, bool enabled,
                                      const bus_master_enable_authority *authority, bool &outcome_uncertain);
        na_status_t (*destroy)(void *context, void *state, bool &outcome_uncertain);
    };

    static constexpr type_e type_of() { return type_e::pci_dma_domain; }

    dma_domain(process_id owner, function_identity identity, backend_ops operations, void *context, void *backend_state,
               configuration config, physical_resource::root *resource_root = nullptr);
    ~dma_domain() override;

    process_id owner() const noexcept { return owner_; }
    dma_operation_result map_page(process_id caller, handle_t<naos::data_plane::memory_object> backing, u64 iova,
                                  u64 object_offset, naos::data_plane::dma_direction direction);
    dma_operation_result unmap_page(process_id caller, u64 iova);
    dma_operation_result set_bus_master(process_id caller, bool enabled);
    /// Explicitly destroy the hardware domain. Requires BME off and no live
    /// mappings; otherwise the capability remains usable for orderly teardown.
    dma_operation_result close(process_id caller);

    bool state_uncertain() const { return state_uncertain_.load(std::memory_order_acquire); }
    bool capability_is_unique() const override { return true; }
    na_signal_t capability_signals() const override { return NA_SIGNAL_WRITABLE; }

  private:
    class operation_guard
    {
      public:
        explicit operation_guard(std::atomic_flag &flag)
            : flag_(flag)
            , acquired_(!flag_.test_and_set(std::memory_order_acquire))
        {
        }
        ~operation_guard()
        {
            if (acquired_)
                flag_.clear(std::memory_order_release);
        }
        explicit operator bool() const { return acquired_; }

      private:
        std::atomic_flag &flag_;
        bool acquired_;
    };

    dma_operation_result set_bus_master_locked(bool enabled);
    dma_operation_result destroy_backend_locked();
    bool reserve_dma_window(u64 physical_address, bool &outcome_uncertain);
    bool revoke_dma_window(u64 physical_address);
    void teardown_noexcept();
    void quarantine_mappings();

    process_id owner_;
    function_identity identity_;
    backend_ops operations_;
    void *context_;
    void *backend_state_;
    configuration configuration_;
    physical_resource::root *resource_root_;
    dma_mapping_ledger<handle_t<naos::data_plane::memory_object>, max_dma_mappings_per_domain> mappings_;
    std::atomic_flag operation_in_progress_ = ATOMIC_FLAG_INIT;
    std::atomic_bool state_uncertain_{false};
    bool bus_master_enabled_ = false;
    bool closed_ = false;
};
} // namespace pci
