#pragma once

#include "kernel/irq.hpp"
#include "kernel/kobject.hpp"
#include "kernel/lock.hpp"
#include "kernel/pci/discovery_fabric.hpp"
#include "kernel/pci/interrupt_event.hpp"
#include "kernel/pci/msi_config.hpp"
#include "kernel/pci/msi_route_registry.hpp"
#include "kernel/pci/platform.hpp"
#include "kernel/pci/topology_lifecycle.hpp"
#include "naos/abi.h"
#include <atomic>
#include <utility>

namespace pci
{
struct interrupt_source_operation_result
{
    na_status_t status = NA_STATUS_OK;
    bool outcome_unknown = false;
};

enum class interrupt_source_state : u8
{
    uninitialized,
    prepared,
    bound,
    masked,
    uncertain,
    closed,
};

/// Owner-bound, readiness-signaled MSI route. The IRQ handler only updates a
/// coalesced sequence and notifies capability waiters; config-space access,
/// route enable/mask, IEC invalidation, and teardown remain typed kernel ops.
class interrupt_source final : public kobject
{
  public:
    static constexpr type_e type_of() { return type_e::interrupt_source; }

    interrupt_source(process_id owner, function_identity identity);
    ~interrupt_source() override;

    bool capability_is_unique() const override { return true; }
    na_signal_t capability_signals() const override;
    u64 capability_state() const override;
    const function_identity &identity() const { return identity_; }

    interrupt_source_operation_result bind(process_id caller);
    interrupt_source_operation_result set_masked(process_id caller, bool masked);
    na_status_t wait(process_id caller, u64 &sequence, u32 &flags) const;
    na_status_t acknowledge(process_id caller, u64 sequence, u64 &current_sequence, bool &pending, u32 &flags);

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

    na_status_t initialize(const function &pci_function, bool &outcome_unknown);
    irq::request_result on_interrupt(const irq::interrupt_info *, u64) noexcept;
    void quarantine_resources() noexcept;
    void quarantine_route_identity() noexcept;
    void teardown_noexcept() noexcept;
    void update_state(interrupt_source_state state);

    process_id owner_;
    function_identity identity_;
    irq::msi_vector_lease vector_;
    irq::registration registration_;
    msi_route route_{};
    interrupt_route_config_snapshot previous_{};
    mutable lock::spinlock_t state_lock_;
    interrupt_event_state event_{};
    interrupt_source_state state_ = interrupt_source_state::uninitialized;
    std::atomic_flag operation_in_progress_ = ATOMIC_FLAG_INIT;
    bool route_prepared_ = false;
    bool registry_reserved_ = false;
    bool quarantine_recorded_ = false;

    friend na_status_t create_interrupt_source(const function &pci_function, handle_t<interrupt_source> &result,
                                               bool &outcome_unknown);
};

/// Mint and configure one disabled MSI route for a function. The caller must
/// register the returned capability before invoking bind().
na_status_t create_interrupt_source(const function &pci_function, handle_t<interrupt_source> &result,
                                    bool &outcome_unknown);

} // namespace pci
