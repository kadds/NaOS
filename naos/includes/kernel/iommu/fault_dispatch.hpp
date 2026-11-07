#pragma once

#include "kernel/iommu/backend.hpp"
#include "kernel/pci/dma_fault_source.hpp"
#include "kernel/types.hpp"

template <typename T> class handle_t;
struct handle_control;

namespace iommu
{
/// A backend has decoded its hardware event into this provider-neutral shape.
/// `reason` remains the provider's raw event code and is not cross-vendor ABI.
struct dma_fault_event
{
    u64 address = 0;
    u16 requester_id = 0;
    u8 reason = 0;
    pci::dma_fault_direction direction = pci::dma_fault_direction::unknown;
};

struct dma_fault_dispatch_result
{
    u32 source_overflow_count = 0;
    bool invalid_batch = false;
};

/// A live requester fault stream registered with the kernel IOMMU owner.
/// The source capability remains owned by its domain; this record holds one
/// internal reference until teardown removes the registration.
struct dma_fault_source_registration
{
    provider_kind provider = provider_kind::none;
    u32 unit_index = 0;
    u16 segment = 0;
    u16 requester_id = 0;
    pci::function_identity identity{};
    bool quarantined = false;
    bool active = false;
    handle_control *source_control = nullptr;
};

using dma_fault_push_callback = bool (*)(const void *context, const dma_fault_source_registration &source,
                                         const pci::dma_fault_record &record);
using dma_fault_quarantine_callback = void (*)(const void *context, const dma_fault_source_registration &source,
                                               bool unit_overflow);

/// PCI policy remains outside the vendor adapter. Fault decoders invoke this
/// callback only after dropping their own controller and buffer reservations.
struct fault_quiesce_ops
{
    void *context = nullptr;
    na_status_t (*disable_bus_master)(void *context, const pci::function_identity &identity,
                                      bool &outcome_uncertain) = nullptr;
};

/// Vendor-neutral result of draining one provider's hardware fault source.
/// `status_code` carries a numeric diagnostic only; callers must not interpret
/// it as a vendor error enum.
struct fault_drain_report
{
    u32 record_count = 0;
    u32 unexpected_record_count = 0;
    u32 source_overflow_count = 0;
    u32 status_code = 0;
    bool drain_succeeded = false;
    bool event_log_overflow = false;
    bool more_records = false;
    bool unit_quarantined = false;
    bool retry = false;
};

using provider_fault_drain_adapter = bool (*)(u32 unit_index, const fault_quiesce_ops &quiesce_ops, bool log_records,
                                              fault_drain_report &report);
using provider_fault_quarantine_adapter = bool (*)(const fault_quiesce_ops &quiesce_ops);
using provider_fault_interrupt_configure_adapter = na_status_t (*)(u32 unit_index, u32 destination_apic_id, u8 vector,
                                                                   bool &outcome_uncertain);
using provider_fault_poll_start_adapter = bool (*)(const fault_quiesce_ops &quiesce_ops);
using provider_fault_poll_active_adapter = bool (*)();

bool register_dma_fault_source(provider_kind provider, u32 unit_index, const pci::function_identity &identity,
                               u16 requester_id, handle_t<pci::dma_fault_source> &source);
void unregister_dma_fault_source(provider_kind provider, u32 unit_index, const pci::function_identity &identity,
                                 u16 requester_id);
bool dma_fault_source_quarantined(provider_kind provider, u32 unit_index, const pci::function_identity &identity,
                                  u16 requester_id);
dma_fault_dispatch_result dispatch_registered_dma_fault_events(provider_kind provider, u32 unit_index,
                                                               const dma_fault_event *events, u32 event_count,
                                                               dma_fault_push_callback push_fault,
                                                               dma_fault_quarantine_callback quarantine_source,
                                                               const void *context);
u32 quarantine_registered_dma_fault_sources(provider_kind provider, u32 unit_index, bool unit_overflow,
                                            dma_fault_quarantine_callback quarantine_source, const void *context);
bool register_provider_fault_adapter(provider_kind provider, provider_fault_drain_adapter drain,
                                     provider_fault_quarantine_adapter quarantine_units = nullptr,
                                     provider_fault_interrupt_configure_adapter configure_interrupt = nullptr);
bool drain_provider_fault_events(provider_kind provider, u32 unit_index, const fault_quiesce_ops &quiesce_ops,
                                 bool log_records, fault_drain_report &report);
bool quarantine_provider_fault_units(provider_kind provider, const fault_quiesce_ops &quiesce_ops);
na_status_t configure_provider_fault_interrupt(provider_kind provider, u32 unit_index, u32 destination_apic_id,
                                               u8 vector, bool &outcome_uncertain);
bool register_provider_fault_poll_adapter(provider_kind provider, provider_fault_poll_start_adapter start,
                                          provider_fault_poll_active_adapter active);
bool start_provider_fault_polling(provider_kind provider, const fault_quiesce_ops &quiesce_ops);
bool provider_fault_polling_active(provider_kind provider);

/// Deliver normalized events only to the registered source for the same unit
/// and requester. Callers serialize drains per provider/unit to preserve event
/// order. Registry callbacks run after the registry lock is released; a copied
/// source reference keeps the capability alive across callback execution.
template <typename FaultSources, typename PushFault, typename QuarantineSource>
dma_fault_dispatch_result dispatch_dma_fault_events(u32 unit_index, const dma_fault_event *events, u32 event_count,
                                                    FaultSources &sources, PushFault &&push_fault,
                                                    QuarantineSource &&quarantine_source)
{
    dma_fault_dispatch_result result{};
    if (event_count != 0 && events == nullptr)
    {
        result.invalid_batch = true;
        return result;
    }

    for (u32 event_index = 0; event_index < event_count; ++event_index)
    {
        const auto &event = events[event_index];
        for (auto &entry : sources)
        {
            if (!entry.active || entry.unit_index != unit_index || entry.requester_id != event.requester_id)
                continue;

            const pci::dma_fault_record record{event.address, event.requester_id, event.reason, event.direction};
            if (push_fault(entry, record) || entry.quarantined)
                continue;

            entry.quarantined = true;
            result.source_overflow_count++;
            quarantine_source(entry);
        }
    }
    return result;
}
} // namespace iommu
