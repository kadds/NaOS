#include "kernel/pci/interrupt_source.hpp"

#include "kernel/log.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/ucontext.hpp"

KLOG_MODULE(kernel);

namespace pci
{
namespace
{
constexpr u32 max_live_msi_routes = irq::vector_pool::vector_count;
msi_route_registry<max_live_msi_routes> route_registry;
lock::spinlock_t route_registry_lock;

bool make_route_key(const function_identity &identity, msi_route_function_key &key)
{
    if (identity.segment > 0xffff || identity.bus > 0xff || identity.device >= 32 || identity.function >= 8)
        return false;
    key = {static_cast<u16>(identity.segment), static_cast<u8>(identity.bus), static_cast<u8>(identity.device),
           static_cast<u8>(identity.function)};
    return true;
}

na_status_t config_error_status(msi_config_error failure)
{
    switch (failure)
    {
        case msi_config_error::none:
            return NA_STATUS_OK;
        case msi_config_error::route_unavailable:
        case msi_config_error::invalid_capability:
        case msi_config_error::invalid_message:
        case msi_config_error::msix_already_enabled:
            return NA_STATUS_NOT_SUPPORTED;
        case msi_config_error::stale_identity:
            return NA_STATUS_OBJECT_REVOKED;
        case msi_config_error::resource_unavailable:
            return NA_STATUS_RESOURCE_EXHAUSTED;
        case msi_config_error::invalid_operations:
        case msi_config_error::msi_already_enabled:
        case msi_config_error::msi_still_enabled:
        case msi_config_error::config_read_failed:
        case msi_config_error::hardware_outcome_unknown:
            return NA_STATUS_IO_ERROR;
    }
    return NA_STATUS_IO_ERROR;
}

} // namespace

interrupt_source::interrupt_source(process_id owner, function_identity identity)
    : kobject(type_of())
    , owner_(owner)
    , identity_(identity)
{
}

interrupt_source::~interrupt_source() { teardown_noexcept(); }

na_status_t interrupt_source::initialize(const function &pci_function, bool &outcome_unknown)
{
    outcome_unknown = false;
    if (pci_function.owner() != owner_ || pci_function.identity().platform_root_id != identity_.platform_root_id ||
        pci_function.identity().segment != identity_.segment || pci_function.identity().bus != identity_.bus ||
        pci_function.identity().device != identity_.device || pci_function.identity().function != identity_.function ||
        pci_function.identity().boot_epoch != identity_.boot_epoch ||
        pci_function.identity().firmware_generation != identity_.firmware_generation ||
        pci_function.identity().aml_namespace_generation != identity_.aml_namespace_generation ||
        pci_function.identity().platform_topology_generation != identity_.platform_topology_generation ||
        pci_function.identity().pci_fabric_generation != identity_.pci_fabric_generation)
        return NA_STATUS_OBJECT_REVOKED;
    if (!topology_lifetime().is_current(identity_.boot_epoch, identity_.firmware_generation,
                                        identity_.aml_namespace_generation, identity_.pci_fabric_generation,
                                        identity_.platform_topology_generation))
        return NA_STATUS_OBJECT_REVOKED;
    vector_ = irq::allocate_msi_vector();
    if (!vector_)
        return NA_STATUS_RESOURCE_EXHAUSTED;

    registration_ =
        irq::register_handler(vector_.vector(), irq::hard_handler::bind<&interrupt_source::on_interrupt>(*this));
    if (!registration_)
        return NA_STATUS_RESOURCE_EXHAUSTED;

    msi_config_error failure = msi_config_error::none;
    if (!prepare_interrupt_route(pci_function, vector_.vector(), route_, previous_, failure, outcome_unknown))
    {
        KLOG_ERROR(
            "PCI MSI route preparation failed segment={} bus={} device={} function={} failure={} outcome_unknown={}",
            identity_.segment, identity_.bus, identity_.device, identity_.function, static_cast<u32>(failure),
            outcome_unknown);
        if (outcome_unknown)
        {
            update_state(interrupt_source_state::uncertain);
            // If the provider returned a complete route token, retain the
            // route metadata and let teardown attempt the controller-specific
            // stop/drain/retire sequence before deciding whether to quarantine.
            route_prepared_ = static_cast<bool>(vector_) && route_.token.valid() && route_.vector == vector_.vector();
            if (!route_prepared_)
                quarantine_resources();
            return NA_STATUS_IO_ERROR;
        }
        registration_.reset();
        vector_.reset();
        return config_error_status(failure);
    }

    route_prepared_ = true;
    update_state(interrupt_source_state::prepared);
    return NA_STATUS_OK;
}

irq::request_result interrupt_source::on_interrupt(const irq::interrupt_info *, u64) noexcept
{
    bool notify = false;
    {
        uctx::RawSpinLockUninterruptibleContext guard(state_lock_);
        if (state_ == interrupt_source_state::closed)
            return irq::request_result::ok;
        notify = event_.interrupt();
    }
    if (notify)
        notify_readiness();
    return irq::request_result::ok;
}

void interrupt_source::update_state(interrupt_source_state state)
{
    {
        uctx::RawSpinLockUninterruptibleContext guard(state_lock_);
        state_ = state;
    }
    notify_readiness();
}

interrupt_source_operation_result interrupt_source::bind(process_id caller) { return set_masked(caller, false); }

interrupt_source_operation_result interrupt_source::set_masked(process_id caller, bool masked)
{
    if (caller != owner_)
        return {NA_STATUS_ACCESS_DENIED, false};
    operation_guard operation(operation_in_progress_);
    if (!operation)
        return {NA_STATUS_WOULD_BLOCK, false};

    interrupt_source_state current_state;
    {
        uctx::RawSpinLockUninterruptibleContext guard(state_lock_);
        current_state = state_;
    }
    if (current_state == interrupt_source_state::uncertain)
        return {NA_STATUS_IO_ERROR, false};
    if (current_state == interrupt_source_state::closed || !route_prepared_)
        return {NA_STATUS_OBJECT_REVOKED, false};
    if (!masked && !topology_lifetime().is_current(identity_.boot_epoch, identity_.firmware_generation,
                                                   identity_.aml_namespace_generation, identity_.pci_fabric_generation,
                                                   identity_.platform_topology_generation))
        return {NA_STATUS_OBJECT_REVOKED, false};
    if ((masked && current_state == interrupt_source_state::masked) ||
        (!masked && current_state == interrupt_source_state::bound))
        return {NA_STATUS_OK, false};

    msi_config_error failure = msi_config_error::none;
    bool outcome_unknown = false;
    const bool enabled = !masked;
    if (!set_interrupt_route_enabled(identity_, route_, previous_, enabled, failure, outcome_unknown))
    {
        if (outcome_unknown || failure == msi_config_error::hardware_outcome_unknown)
        {
            update_state(interrupt_source_state::uncertain);
            return {NA_STATUS_IO_ERROR, true};
        }
        return {config_error_status(failure), false};
    }

    if (enabled && !topology_lifetime().is_current(identity_.boot_epoch, identity_.firmware_generation,
                                                   identity_.aml_namespace_generation, identity_.pci_fabric_generation,
                                                   identity_.platform_topology_generation))
    {
        failure = msi_config_error::none;
        bool rollback_outcome_unknown = false;
        if (!set_interrupt_route_enabled(identity_, route_, previous_, false, failure, rollback_outcome_unknown) ||
            rollback_outcome_unknown)
        {
            update_state(interrupt_source_state::uncertain);
            quarantine_resources();
            return {NA_STATUS_IO_ERROR, true};
        }
        update_state(interrupt_source_state::masked);
        return {NA_STATUS_OBJECT_REVOKED, false};
    }

    update_state(masked ? interrupt_source_state::masked : interrupt_source_state::bound);
    return {NA_STATUS_OK, false};
}

na_status_t interrupt_source::wait(process_id caller, u64 &sequence, u32 &flags) const
{
    sequence = 0;
    flags = 0;
    if (caller != owner_)
        return NA_STATUS_ACCESS_DENIED;

    uctx::RawSpinLockUninterruptibleContext guard(state_lock_);
    if (state_ == interrupt_source_state::closed)
        return NA_STATUS_OBJECT_REVOKED;
    sequence = event_.sequence();
    if (event_.readable())
        flags |= 1u;
    if (state_ == interrupt_source_state::masked || state_ == interrupt_source_state::prepared)
        flags |= 1u << 1;
    if (state_ == interrupt_source_state::uncertain)
        flags |= 1u << 2;
    return (event_.readable() || state_ == interrupt_source_state::uncertain) ? NA_STATUS_OK : NA_STATUS_WOULD_BLOCK;
}

na_status_t interrupt_source::acknowledge(process_id caller, u64 sequence, u64 &current_sequence, bool &pending,
                                          u32 &flags)
{
    current_sequence = 0;
    pending = false;
    flags = 0;
    if (caller != owner_)
        return NA_STATUS_ACCESS_DENIED;

    interrupt_event_ack result = interrupt_event_ack::not_pending;
    {
        uctx::RawSpinLockUninterruptibleContext guard(state_lock_);
        if (state_ == interrupt_source_state::uncertain)
            return NA_STATUS_IO_ERROR;
        if (state_ == interrupt_source_state::closed)
            return NA_STATUS_OBJECT_REVOKED;
        result = event_.acknowledge(sequence);
        current_sequence = event_.sequence();
        pending = event_.readable();
        if (pending)
            flags |= 1u;
        if (state_ == interrupt_source_state::masked || state_ == interrupt_source_state::prepared)
            flags |= 1u << 1;
        if (state_ == interrupt_source_state::uncertain)
            flags |= 1u << 2;
    }

    if (result == interrupt_event_ack::acknowledged)
    {
        notify_readiness();
        return NA_STATUS_OK;
    }
    return result == interrupt_event_ack::stale_sequence ? NA_STATUS_ALREADY_CONSUMED : NA_STATUS_WOULD_BLOCK;
}

na_signal_t interrupt_source::capability_signals() const
{
    uctx::RawSpinLockUninterruptibleContext guard(state_lock_);
    na_signal_t signals = event_.readable() ? NA_SIGNAL_READABLE : 0;
    if (state_ == interrupt_source_state::uncertain)
        signals |= NA_SIGNAL_OBJECT_REVOKED;
    return signals;
}

u64 interrupt_source::capability_state() const
{
    uctx::RawSpinLockUninterruptibleContext guard(state_lock_);
    return event_.sequence();
}

void interrupt_source::quarantine_resources() noexcept
{
    if (quarantine_recorded_)
        return;
    if (registration_ && !registration_.quarantine())
        kassert(false, "could not replace uncertain MSI handler with its static sink");
    if (vector_ && !vector_.quarantine())
        kassert(false, "could not quarantine uncertain MSI vector");

    quarantine_route_identity();
    quarantine_recorded_ = true;
}

void interrupt_source::quarantine_route_identity() noexcept
{
    if (registry_reserved_)
    {
        msi_route_function_key key{};
        if (!make_route_key(identity_, key))
            kassert(false, "invalid identity for MSI route quarantine");
        uctx::RawSpinLockUninterruptibleContext guard(route_registry_lock);
        if (route_registry.quarantine(key) != msi_route_registry_error::none)
            kassert(false, "could not quarantine uncertain MSI function route");
        registry_reserved_ = false;
    }
}

void interrupt_source::teardown_noexcept() noexcept
{
    const bool route_was_prepared = route_prepared_;
    const u8 completed_vector = route_prepared_ ? vector_.vector() : 0;
    interrupt_source_state current_state;
    {
        uctx::RawSpinLockUninterruptibleContext guard(state_lock_);
        current_state = state_;
        state_ = interrupt_source_state::closed;
    }
    if (current_state == interrupt_source_state::uncertain && !route_prepared_)
    {
        quarantine_resources();
        KLOG_ERROR("PCI MSI route quarantined after uncertain owner state segment={} bus={} device={} function={}",
                   identity_.segment, identity_.bus, identity_.device, identity_.function);
        return;
    }

    if (route_prepared_)
    {
        const auto teardown = shutdown_interrupt_route(identity_, route_, previous_);
        if (teardown.failure != msi_route_teardown_failure::none || teardown.outcome_uncertain)
        {
            if (teardown.vector_recyclable)
            {
                // The source is disabled, its IRTE retired, and the destination
                // drained on both sides of invalidation. A failed restore keeps
                // this PCI function quarantined, but its vector is safe to reuse.
                registration_.reset();
                vector_.reset();
                quarantine_route_identity();
                quarantine_recorded_ = true;
                KLOG_ERROR("PCI MSI function route quarantined after restore failure; vector {} recycled "
                           "segment={} bus={} device={} function={}",
                           completed_vector, identity_.segment, identity_.bus, identity_.device, identity_.function);
                return;
            }

            quarantine_resources();
            KLOG_ERROR(
                "PCI MSI route teardown quarantined segment={} bus={} device={} function={} failure={} uncertain={}",
                identity_.segment, identity_.bus, identity_.device, identity_.function,
                static_cast<u8>(teardown.failure), teardown.outcome_uncertain);
            return;
        }
        route_prepared_ = false;
    }

    // shutdown_interrupt_route has disabled the source, completed IEC invalidation,
    // and drained the destination. The vector registration lock is the final
    // callback quiescence barrier before its vector can be reused.
    registration_.reset();
    vector_.reset();
    if (registry_reserved_)
    {
        msi_route_function_key key{};
        if (make_route_key(identity_, key))
        {
            uctx::RawSpinLockUninterruptibleContext guard(route_registry_lock);
            const auto released = route_registry.release(key);
            if (released != msi_route_registry_error::none)
                kassert(false, "could not release completed MSI function route");
        }
        registry_reserved_ = false;
    }
    if (route_was_prepared)
        KLOG_INFO("PCI MSI route teardown complete segment={} bus={} device={} function={} vector={}",
                  identity_.segment, identity_.bus, identity_.device, identity_.function, completed_vector);
}

na_status_t create_interrupt_source(const function &pci_function, handle_t<interrupt_source> &result,
                                    bool &outcome_unknown)
{
    result.reset();
    outcome_unknown = false;
    const process_id owner = pci_function.owner();
    const auto &identity = pci_function.identity();
    msi_route_function_key key{};
    if (owner == 0 || !make_route_key(identity, key))
        return NA_STATUS_INVALID_ARGUMENT;

    {
        uctx::RawSpinLockUninterruptibleContext guard(route_registry_lock);
        const auto reserve_result = route_registry.reserve(key);
        if (reserve_result == msi_route_registry_error::duplicate ||
            reserve_result == msi_route_registry_error::already_quarantined)
            return NA_STATUS_ALREADY_CONSUMED;
        if (reserve_result != msi_route_registry_error::none)
            return NA_STATUS_RESOURCE_EXHAUSTED;
    }

    auto source = handle_t<interrupt_source>::make(owner, identity);
    if (!source)
    {
        uctx::RawSpinLockUninterruptibleContext guard(route_registry_lock);
        (void)route_registry.release(key);
        return NA_STATUS_RESOURCE_EXHAUSTED;
    }
    source->registry_reserved_ = true;
    const auto initialized = source->initialize(pci_function, outcome_unknown);
    if (initialized != NA_STATUS_OK)
        return initialized;
    if (!topology_lifetime().is_current(identity.boot_epoch, identity.firmware_generation,
                                        identity.aml_namespace_generation, identity.pci_fabric_generation,
                                        identity.platform_topology_generation))
        return NA_STATUS_OBJECT_REVOKED;

    result = std::move(source);
    return NA_STATUS_OK;
}

} // namespace pci
