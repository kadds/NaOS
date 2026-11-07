#include "kernel/iommu/backend.hpp"
#include "kernel/iommu/fault_dispatch.hpp"

#include "kernel/handle.hpp"
#include "kernel/log.hpp"
#include "kernel/ucontext.hpp"

namespace iommu
{
namespace
{
provider_router router;
constexpr u32 max_fault_source_units_per_provider = 32;

struct fault_source_registry
{
    lock::spinlock_t lock;
    struct entry
    {
        dma_fault_source_registration registration{};
        u64 generation = 0;
        bool retiring = false;
    } entries[512]{};
    bool quarantined_units[3][max_fault_source_units_per_provider]{};
    bool provider_quarantined[3]{};
    u64 next_generation = 1;
};

fault_source_registry fault_sources;

struct provider_fault_adapter_entry
{
    provider_fault_drain_adapter drain = nullptr;
    provider_fault_quarantine_adapter quarantine_units = nullptr;
    provider_fault_interrupt_configure_adapter configure_interrupt = nullptr;
    provider_fault_poll_start_adapter start_fault_polling = nullptr;
    provider_fault_poll_active_adapter fault_polling_active = nullptr;
    provider_dma_domain_adapter_ops dma_domain{};
};

struct provider_fault_adapter_registry
{
    lock::spinlock_t lock;
    provider_fault_adapter_entry entries[3]{};
};

provider_fault_adapter_registry provider_fault_adapters;

struct fault_source_callback_snapshot
{
    u32 index = ~u32{0};
    u64 generation = 0;
    dma_fault_source_registration registration{};
    handle_t<pci::dma_fault_source> source;

    explicit operator bool() const noexcept { return static_cast<bool>(source); }
};

bool valid_provider(provider_kind provider)
{
    return provider == provider_kind::intel_vtd || provider == provider_kind::amd_vi;
}

bool same_fault_source(const dma_fault_source_registration &entry, provider_kind provider, u32 unit_index,
                       const pci::function_identity &identity, u16 requester_id)
{
    return entry.active && entry.provider == provider && entry.unit_index == unit_index &&
           entry.segment == identity.segment && entry.requester_id == requester_id;
}

bool same_fault_source_identity(const dma_fault_source_registration &entry, provider_kind provider, u32 unit_index,
                                const pci::function_identity &identity, u16 requester_id)
{
    return entry.provider == provider && entry.unit_index == unit_index && entry.segment == identity.segment &&
           entry.requester_id == requester_id;
}

provider_fault_adapter_entry fault_adapter_for(provider_kind provider)
{
    const auto index = static_cast<u32>(provider);
    if (index >= 3)
        return {};
    uctx::RawSpinLockUninterruptibleContext guard(provider_fault_adapters.lock);
    return provider_fault_adapters.entries[index];
}

fault_source_callback_snapshot snapshot_fault_source(fault_source_registry::entry &entry, u32 index)
{
    fault_source_callback_snapshot snapshot{};
    if (!entry.registration.active || entry.registration.source_control == nullptr)
        return snapshot;
    snapshot.index = index;
    snapshot.generation = entry.generation;
    snapshot.registration = entry.registration;
    snapshot.source = handle_t<pci::dma_fault_source>(entry.registration.source_control);
    return snapshot;
}

bool snapshot_still_registered(const fault_source_registry::entry &entry,
                               const fault_source_callback_snapshot &snapshot)
{
    return entry.registration.active && entry.generation == snapshot.generation &&
           entry.registration.source_control == snapshot.registration.source_control;
}
} // namespace

provider_router &kernel_provider_router() noexcept { return router; }

bool register_dma_fault_source(provider_kind provider, u32 unit_index, const pci::function_identity &identity,
                               u16 requester_id, handle_t<pci::dma_fault_source> &source)
{
    if (!valid_provider(provider) || identity.segment > 0xffffu)
        return false;
    auto *source_control = source.get_control();
    if (source_control == nullptr || unit_index >= max_fault_source_units_per_provider)
        return false;

    uctx::RawSpinLockUninterruptibleContext guard(fault_sources.lock);
    u32 provider_source_count = 0;
    const auto provider_index = static_cast<u32>(provider);
    if (fault_sources.provider_quarantined[provider_index] ||
        fault_sources.quarantined_units[provider_index][unit_index])
        return false;
    if (fault_sources.next_generation == 0)
        return false;
    for (const auto &slot : fault_sources.entries)
    {
        const auto &entry = slot.registration;
        if ((entry.active || slot.retiring) && entry.provider == provider)
            provider_source_count++;
        if ((entry.active || slot.retiring) &&
            same_fault_source_identity(entry, provider, unit_index, identity, requester_id))
            return false;
    }
    if (provider_source_count >= 256)
        return false;
    for (auto &slot : fault_sources.entries)
    {
        auto &entry = slot.registration;
        if (entry.active || slot.retiring)
            continue;
        source_control->ref.fetch_add(1, std::memory_order_relaxed);
        entry.provider = provider;
        entry.unit_index = unit_index;
        entry.segment = static_cast<u16>(identity.segment);
        entry.requester_id = requester_id;
        entry.identity = identity;
        entry.quarantined = false;
        entry.active = true;
        entry.source_control = source_control;
        slot.generation = fault_sources.next_generation++;
        return true;
    }
    return false;
}

void unregister_dma_fault_source(provider_kind provider, u32 unit_index, const pci::function_identity &identity,
                                 u16 requester_id)
{
    handle_t<pci::dma_fault_source> removed;
    u32 removed_index = ~u32{0};
    u64 removed_generation = 0;
    {
        uctx::RawSpinLockUninterruptibleContext guard(fault_sources.lock);
        for (u32 index = 0; index < 512; ++index)
        {
            auto &slot = fault_sources.entries[index];
            auto &entry = slot.registration;
            if (!same_fault_source(entry, provider, unit_index, identity, requester_id))
                continue;
            auto *source_control = entry.source_control;
            if (source_control == nullptr)
                break;
            removed = handle_t<pci::dma_fault_source>(source_control);
            const auto registry_reference = source_control->ref.fetch_sub(1, std::memory_order_acq_rel);
            kassert(registry_reference > 1, "DMA fault source registry lost its owner reference");
            entry.active = false;
            slot.retiring = true;
            removed_index = index;
            removed_generation = slot.generation;
            break;
        }
    }
    if (removed)
    {
        removed->close();
        uctx::RawSpinLockUninterruptibleContext guard(fault_sources.lock);
        auto &slot = fault_sources.entries[removed_index];
        if (slot.generation == removed_generation && slot.retiring && !slot.registration.active)
        {
            slot.registration = {};
            slot.retiring = false;
        }
    }
}

bool dma_fault_source_quarantined(provider_kind provider, u32 unit_index, const pci::function_identity &identity,
                                  u16 requester_id)
{
    if (!valid_provider(provider) || unit_index >= max_fault_source_units_per_provider)
        return true;
    uctx::RawSpinLockUninterruptibleContext guard(fault_sources.lock);
    const auto provider_index = static_cast<u32>(provider);
    if (fault_sources.provider_quarantined[provider_index] ||
        fault_sources.quarantined_units[provider_index][unit_index])
        return true;
    for (const auto &slot : fault_sources.entries)
    {
        const auto &entry = slot.registration;
        if (same_fault_source(entry, provider, unit_index, identity, requester_id))
            return entry.quarantined;
    }
    return true;
}

dma_fault_dispatch_result dispatch_registered_dma_fault_events(provider_kind provider, u32 unit_index,
                                                               const dma_fault_event *events, u32 event_count,
                                                               dma_fault_push_callback push_fault,
                                                               dma_fault_quarantine_callback quarantine_source,
                                                               const void *context)
{
    dma_fault_dispatch_result result{};
    if (!valid_provider(provider) || (event_count != 0 && events == nullptr) ||
        (event_count != 0 && (push_fault == nullptr || quarantine_source == nullptr)))
    {
        result.invalid_batch = true;
        return result;
    }

    for (u32 event_index = 0; event_index < event_count; ++event_index)
    {
        const auto &event = events[event_index];
        fault_source_callback_snapshot snapshot{};
        {
            uctx::RawSpinLockUninterruptibleContext guard(fault_sources.lock);
            for (u32 index = 0; index < 512; ++index)
            {
                const auto &entry = fault_sources.entries[index].registration;
                if (entry.active && entry.provider == provider && entry.unit_index == unit_index &&
                    entry.requester_id == event.requester_id)
                {
                    snapshot = snapshot_fault_source(fault_sources.entries[index], index);
                    break;
                }
            }
        }
        if (!snapshot)
            continue;

        const pci::dma_fault_record record{event.address, event.requester_id, event.reason, event.direction};
        if (push_fault(context, snapshot.registration, record) || snapshot.registration.quarantined)
            continue;

        bool quarantine = false;
        {
            uctx::RawSpinLockUninterruptibleContext guard(fault_sources.lock);
            auto &entry = fault_sources.entries[snapshot.index];
            if (snapshot_still_registered(entry, snapshot) && !entry.registration.quarantined)
            {
                entry.registration.quarantined = true;
                snapshot.registration.quarantined = true;
                quarantine = true;
            }
        }
        if (quarantine)
        {
            result.source_overflow_count++;
            quarantine_source(context, snapshot.registration, false);
        }
    }
    return result;
}

u32 quarantine_registered_dma_fault_sources(provider_kind provider, u32 unit_index, bool unit_overflow,
                                            dma_fault_quarantine_callback quarantine_source, const void *context)
{
    if (!valid_provider(provider) || quarantine_source == nullptr)
        return 0;

    u64 generation_cutoff = 0;
    {
        uctx::RawSpinLockUninterruptibleContext guard(fault_sources.lock);
        generation_cutoff = fault_sources.next_generation - 1;
        const auto provider_index = static_cast<u32>(provider);
        if (unit_index < max_fault_source_units_per_provider)
            fault_sources.quarantined_units[provider_index][unit_index] = true;
        else
            fault_sources.provider_quarantined[provider_index] = true;
        for (auto &slot : fault_sources.entries)
        {
            auto &entry = slot.registration;
            if (entry.active && entry.provider == provider && entry.unit_index == unit_index)
                entry.quarantined = true;
        }
    }

    u32 quarantined_count = 0;
    for (u32 index = 0; index < 512; ++index)
    {
        fault_source_callback_snapshot snapshot{};
        {
            uctx::RawSpinLockUninterruptibleContext guard(fault_sources.lock);
            auto &slot = fault_sources.entries[index];
            const auto &entry = slot.registration;
            if (slot.generation <= generation_cutoff && entry.active && entry.quarantined &&
                entry.provider == provider && entry.unit_index == unit_index)
                snapshot = snapshot_fault_source(slot, index);
        }
        if (!snapshot)
            continue;

        quarantine_source(context, snapshot.registration, unit_overflow);
        quarantined_count++;
    }
    return quarantined_count;
}

bool register_provider_fault_adapter(provider_kind provider, provider_fault_drain_adapter drain,
                                     provider_fault_quarantine_adapter quarantine_units,
                                     provider_fault_interrupt_configure_adapter configure_interrupt)
{
    if (!valid_provider(provider) || drain == nullptr)
        return false;
    const auto index = static_cast<u32>(provider);
    uctx::RawSpinLockUninterruptibleContext guard(provider_fault_adapters.lock);
    auto &entry = provider_fault_adapters.entries[index];
    if ((entry.drain != nullptr && entry.drain != drain) ||
        (entry.quarantine_units != nullptr && entry.quarantine_units != quarantine_units) ||
        (entry.configure_interrupt != nullptr && entry.configure_interrupt != configure_interrupt))
        return false;
    entry.drain = drain;
    entry.quarantine_units = quarantine_units;
    entry.configure_interrupt = configure_interrupt;
    return true;
}

bool register_provider_dma_domain_adapter(provider_kind provider, const provider_dma_domain_adapter_ops &ops)
{
    if (!valid_provider(provider) || ops.create == nullptr || ops.map_page == nullptr || ops.unmap_page == nullptr ||
        ops.destroy == nullptr)
        return false;
    const auto index = static_cast<u32>(provider);
    uctx::RawSpinLockUninterruptibleContext guard(provider_fault_adapters.lock);
    auto &entry = provider_fault_adapters.entries[index];
    if (entry.dma_domain.create != nullptr)
        return entry.dma_domain.create == ops.create && entry.dma_domain.map_page == ops.map_page &&
               entry.dma_domain.unmap_page == ops.unmap_page && entry.dma_domain.destroy == ops.destroy &&
               entry.dma_domain.unit_state_uncertain == ops.unit_state_uncertain &&
               entry.dma_domain.ready_for_bus_master == ops.ready_for_bus_master;
    entry.dma_domain = ops;
    return true;
}

bool drain_provider_fault_events(provider_kind provider, u32 unit_index, const fault_quiesce_ops &quiesce_ops,
                                 bool log_records, fault_drain_report &report)
{
    report = {};
    const auto adapter = fault_adapter_for(provider);
    return adapter.drain != nullptr && adapter.drain(unit_index, quiesce_ops, log_records, report);
}

bool quarantine_provider_fault_units(provider_kind provider, const fault_quiesce_ops &quiesce_ops)
{
    const auto adapter = fault_adapter_for(provider);
    return adapter.quarantine_units != nullptr && adapter.quarantine_units(quiesce_ops);
}

na_status_t configure_provider_fault_interrupt(provider_kind provider, u32 unit_index, u32 destination_apic_id,
                                               u8 vector, bool &outcome_uncertain)
{
    outcome_uncertain = false;
    const auto adapter = fault_adapter_for(provider);
    if (adapter.configure_interrupt == nullptr)
        return NA_STATUS_NOT_SUPPORTED;
    return adapter.configure_interrupt(unit_index, destination_apic_id, vector, outcome_uncertain);
}

bool register_provider_fault_poll_adapter(provider_kind provider, provider_fault_poll_start_adapter start,
                                          provider_fault_poll_active_adapter active)
{
    if (!valid_provider(provider) || start == nullptr || active == nullptr)
        return false;
    const auto index = static_cast<u32>(provider);
    uctx::RawSpinLockUninterruptibleContext guard(provider_fault_adapters.lock);
    auto &entry = provider_fault_adapters.entries[index];
    if ((entry.start_fault_polling != nullptr && entry.start_fault_polling != start) ||
        (entry.fault_polling_active != nullptr && entry.fault_polling_active != active))
        return false;
    entry.start_fault_polling = start;
    entry.fault_polling_active = active;
    return true;
}

bool start_provider_fault_polling(provider_kind provider, const fault_quiesce_ops &quiesce_ops)
{
    const auto adapter = fault_adapter_for(provider);
    return adapter.start_fault_polling != nullptr && adapter.start_fault_polling(quiesce_ops);
}

bool provider_fault_polling_active(provider_kind provider)
{
    const auto adapter = fault_adapter_for(provider);
    return adapter.fault_polling_active != nullptr && adapter.fault_polling_active();
}

na_status_t create_provider_dma_domain_backend(provider_kind provider, const device_source_ref &source,
                                               const pci::function_identity &identity, process_id owner,
                                               handle_t<pci::dma_fault_source> &fault_source, void *&backend_state,
                                               u8 &address_width, u32 &unit_index, bool &outcome_uncertain)
{
    backend_state = nullptr;
    address_width = 0;
    unit_index = ~u32{0};
    outcome_uncertain = false;
    const auto adapter = fault_adapter_for(provider);
    if (adapter.dma_domain.create == nullptr)
        return NA_STATUS_NOT_SUPPORTED;
    return adapter.dma_domain.create(source, identity, owner, fault_source, backend_state, address_width, unit_index,
                                     outcome_uncertain);
}

na_status_t map_provider_dma_domain_page(provider_kind provider, void *backend_state, u64 iova, u64 physical_address,
                                         naos::data_plane::dma_direction direction, bool &outcome_uncertain)
{
    outcome_uncertain = false;
    const auto adapter = fault_adapter_for(provider);
    if (adapter.dma_domain.map_page == nullptr)
        return NA_STATUS_NOT_SUPPORTED;
    return adapter.dma_domain.map_page(backend_state, iova, physical_address, direction, outcome_uncertain);
}

na_status_t unmap_provider_dma_domain_page(provider_kind provider, void *backend_state, u64 iova,
                                           bool &outcome_uncertain)
{
    outcome_uncertain = false;
    const auto adapter = fault_adapter_for(provider);
    if (adapter.dma_domain.unmap_page == nullptr)
        return NA_STATUS_NOT_SUPPORTED;
    return adapter.dma_domain.unmap_page(backend_state, iova, outcome_uncertain);
}

na_status_t destroy_provider_dma_domain_backend(provider_kind provider, void *backend_state, bool &outcome_uncertain)
{
    outcome_uncertain = false;
    const auto adapter = fault_adapter_for(provider);
    if (adapter.dma_domain.destroy == nullptr)
        return NA_STATUS_NOT_SUPPORTED;
    return adapter.dma_domain.destroy(backend_state, outcome_uncertain);
}

bool provider_unit_state_uncertain(provider_kind provider, u32 unit_index)
{
    const auto adapter = fault_adapter_for(provider);
    return adapter.dma_domain.unit_state_uncertain == nullptr || adapter.dma_domain.unit_state_uncertain(unit_index);
}

bool provider_dma_domain_ready_for_bus_master(provider_kind provider)
{
    const auto adapter = fault_adapter_for(provider);
    return adapter.dma_domain.ready_for_bus_master == nullptr || adapter.dma_domain.ready_for_bus_master();
}

} // namespace iommu
