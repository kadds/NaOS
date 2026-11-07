#pragma once

#include "kernel/iommu/amd_vi.hpp"
#include "kernel/iommu/fault_dispatch.hpp"

namespace iommu::amd_vi
{
struct fault_dispatch_result
{
    u32 unexpected_event_count = 0;
    u32 source_overflow_count = 0;
    bool unit_quarantine_required = false;
};

/// Route drained AMD I/O-page-fault events to matching requester-owned sources.
/// The callbacks keep source queue and PCI bus-master policy in the kernel
/// adapter while this bounded policy remains directly testable.
fault_dispatch_result decode_fault_events(const event_record *records, u32 record_count, auto &&dispatch_event)
{
    fault_dispatch_result result{};
    if (record_count != 0 && records == nullptr)
    {
        result.unit_quarantine_required = true;
        return result;
    }

    for (u32 record_index = 0; record_index < record_count; ++record_index)
    {
        const auto &record = records[record_index];
        if (record.event_code != 0x02)
        {
            result.unexpected_event_count++;
            result.unit_quarantine_required = true;
            continue;
        }
        // AMD specifies that I/O page faults caused by translation requests
        // are returned to the requester and are not written to the event log.
        // Seeing one here means the log does not match the specified format.
        if (record.is_translation)
        {
            result.unexpected_event_count++;
            result.unit_quarantine_required = true;
            continue;
        }
        if (record.is_interrupt)
            continue;

        const u8 raw_reason = static_cast<u8>((record.raw[0] >> 48) & 0xff);
        const auto direction = record.is_present ? (record.is_write ? pci::dma_fault_direction::device_write
                                                                    : pci::dma_fault_direction::device_read)
                                                 : pci::dma_fault_direction::unknown;
        const dma_fault_event event{record.address, record.device_id, raw_reason, direction};
        const auto dispatched = dispatch_event(event);
        result.source_overflow_count += dispatched.source_overflow_count;
        result.unit_quarantine_required = result.unit_quarantine_required || dispatched.invalid_batch;
    }
    return result;
}

template <typename FaultSources, typename PushFault, typename QuarantineSource>
fault_dispatch_result dispatch_fault_events(u32 unit_index, const event_record *records, u32 record_count,
                                            FaultSources &sources, PushFault &&push_fault,
                                            QuarantineSource &&quarantine_source)
{
    return decode_fault_events(records, record_count, [&](const dma_fault_event &event) {
        return dispatch_dma_fault_events(unit_index, &event, 1, sources, push_fault, quarantine_source);
    });
}

/// Propagate unit-wide event-ring overflow or drain failure to each registered
/// source on that unit. The callback owns source notification and BME policy.
template <typename FaultSources, typename QuarantineSource>
u32 quarantine_fault_sources(u32 unit_index, FaultSources &sources, bool unit_overflow,
                             QuarantineSource &&quarantine_source)
{
    u32 quarantined_count = 0;
    for (auto &entry : sources)
    {
        if (!entry.active || entry.unit_index != unit_index)
            continue;
        entry.quarantined = true;
        quarantine_source(entry, unit_overflow);
        quarantined_count++;
    }
    return quarantined_count;
}

template <typename QuarantineUnit>
bool quarantine_fault_poll_unit(u32 unit_index, bool drain_succeeded, bool event_log_overflow,
                                const fault_dispatch_result &dispatch, QuarantineUnit &&quarantine_unit)
{
    if (drain_succeeded && !event_log_overflow && !dispatch.unit_quarantine_required)
        return false;

    quarantine_unit(unit_index, event_log_overflow);
    return true;
}
} // namespace iommu::amd_vi
