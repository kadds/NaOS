#include <catch2/catch_test_macros.hpp>

#include "kernel/handle.hpp"
#include "kernel/iommu/amd_vi_fault_dispatch.hpp"

#include <array>

namespace
{
struct registry_callback_state
{
    mutable u32 quarantine_count = 0;
    mutable u32 unit_overflow_count = 0;
};

struct reentrant_registry_callback_state
{
    iommu::provider_kind provider = iommu::provider_kind::none;
    u32 unit_index = 0;
    pci::function_identity identity{};
    u16 requester_id = 0;
    mutable u32 push_count = 0;
    mutable u32 quarantine_count = 0;
    mutable bool registry_query_succeeded = true;
    mutable bool quarantine_visible = false;
};

struct fault_source_registration
{
    u32 unit_index = 0;
    u16 requester_id = 0;
    u32 quarantine_count = 0;
    bool active = true;
    bool quarantined = false;
    pci::dma_fault_source *source = nullptr;
};

bool push_fault(fault_source_registration &registration, const pci::dma_fault_record &record)
{
    return registration.source->push_fault(record);
}

void quarantine_source(fault_source_registration &registration) { registration.quarantine_count++; }

bool registry_push_fault(const void *, const iommu::dma_fault_source_registration &registration,
                         const pci::dma_fault_record &record)
{
    handle_t<pci::dma_fault_source> source(registration.source_control);
    return source->push_fault(record);
}

void registry_quarantine_source(const void *opaque_context, const iommu::dma_fault_source_registration &registration,
                                bool unit_overflow)
{
    const auto &context = *static_cast<const registry_callback_state *>(opaque_context);
    context.quarantine_count++;
    context.unit_overflow_count += u32{unit_overflow};
    handle_t<pci::dma_fault_source> source(registration.source_control);
    source->close();
}

bool reentrant_registry_push_fault(const void *opaque_context, const iommu::dma_fault_source_registration &registration,
                                   const pci::dma_fault_record &record)
{
    const auto &context = *static_cast<const reentrant_registry_callback_state *>(opaque_context);
    context.push_count++;
    context.registry_query_succeeded &= !iommu::dma_fault_source_quarantined(context.provider, context.unit_index,
                                                                             context.identity, context.requester_id);
    handle_t<pci::dma_fault_source> source(registration.source_control);
    return source->push_fault(record);
}

void reentrant_registry_quarantine(const void *opaque_context, const iommu::dma_fault_source_registration &registration,
                                   bool unit_overflow)
{
    const auto &context = *static_cast<const reentrant_registry_callback_state *>(opaque_context);
    context.quarantine_count++;
    context.quarantine_visible = iommu::dma_fault_source_quarantined(context.provider, context.unit_index,
                                                                     context.identity, context.requester_id);
    handle_t<pci::dma_fault_source> source(registration.source_control);
    if (unit_overflow)
        source->note_unit_overflow();
    else
        source->close();
}

void quarantine_fault_source(fault_source_registration &registration, bool unit_overflow)
{
    registration.quarantine_count++;
    if (unit_overflow)
        registration.source->note_unit_overflow();
    else
        registration.source->close();
}

pci::function_identity make_identity(u32 bus, u32 device, u32 function)
{
    pci::function_identity identity{};
    identity.segment = 0;
    identity.bus = bus;
    identity.device = device;
    identity.function = function;
    identity.boot_epoch = 1;
    identity.firmware_generation = 2;
    identity.aml_namespace_generation = 3;
    identity.platform_topology_generation = 4;
    identity.pci_fabric_generation = 5;
    return identity;
}

iommu::amd_vi::event_record memory_fault(u16 requester_id, u64 address, u64 raw, bool write)
{
    iommu::amd_vi::event_record record{};
    record.address = address;
    record.raw[0] = raw;
    record.device_id = requester_id;
    record.event_code = 0x02;
    record.is_present = true;
    record.is_write = write;
    return record;
}
} // namespace

TEST_CASE("kernel fault registry isolates providers and quarantines an overflowing requester once",
          "[iommu][fault][registry]")
{
    constexpr process_id owner = 91;
    constexpr u32 unit = 4;
    constexpr u16 requester_id = 0x1818;
    const auto identity = make_identity(0x18, 3, 0);
    auto intel_source = handle_t<pci::dma_fault_source>::make(owner, identity);
    auto amd_source = handle_t<pci::dma_fault_source>::make(owner, identity);
    REQUIRE(intel_source);
    REQUIRE(amd_source);
    REQUIRE(
        iommu::register_dma_fault_source(iommu::provider_kind::intel_vtd, unit, identity, requester_id, intel_source));
    REQUIRE(iommu::register_dma_fault_source(iommu::provider_kind::amd_vi, unit, identity, requester_id, amd_source));
    CHECK_FALSE(
        iommu::register_dma_fault_source(iommu::provider_kind::intel_vtd, unit, identity, requester_id, intel_source));

    registry_callback_state callback_state{};
    const iommu::dma_fault_event initial{0x5000, requester_id, 0x2a, pci::dma_fault_direction::device_write};
    const auto intel_dispatch =
        iommu::dispatch_registered_dma_fault_events(iommu::provider_kind::intel_vtd, unit, &initial, 1,
                                                    registry_push_fault, registry_quarantine_source, &callback_state);
    REQUIRE_FALSE(intel_dispatch.invalid_batch);
    CHECK(intel_dispatch.source_overflow_count == 0);
    CHECK((intel_source->capability_signals() & NA_SIGNAL_READABLE) != 0);
    CHECK((amd_source->capability_signals() & NA_SIGNAL_READABLE) == 0);

    const auto amd_dispatch =
        iommu::dispatch_registered_dma_fault_events(iommu::provider_kind::amd_vi, unit, &initial, 1,
                                                    registry_push_fault, registry_quarantine_source, &callback_state);
    REQUIRE_FALSE(amd_dispatch.invalid_batch);
    CHECK(amd_dispatch.source_overflow_count == 0);
    CHECK((amd_source->capability_signals() & NA_SIGNAL_READABLE) != 0);

    std::array<iommu::dma_fault_event, pci::max_dma_fault_queue_records + 3> events{};
    for (u32 index = 0; index < events.size(); ++index)
        events[index] = {0x8000 + static_cast<u64>(index) * 0x1000, requester_id, 0x2c,
                         pci::dma_fault_direction::device_write};
    const auto overflow = iommu::dispatch_registered_dma_fault_events(
        iommu::provider_kind::intel_vtd, unit, events.data(), static_cast<u32>(events.size()), registry_push_fault,
        registry_quarantine_source, &callback_state);
    CHECK_FALSE(overflow.invalid_batch);
    CHECK(overflow.source_overflow_count == 1);
    CHECK(callback_state.quarantine_count == 1);
    CHECK(iommu::dma_fault_source_quarantined(iommu::provider_kind::intel_vtd, unit, identity, requester_id));
    CHECK_FALSE(iommu::dma_fault_source_quarantined(iommu::provider_kind::amd_vi, unit, identity, requester_id));

    CHECK(iommu::quarantine_registered_dma_fault_sources(iommu::provider_kind::intel_vtd, unit, true,
                                                         registry_quarantine_source, &callback_state) == 1);
    CHECK(callback_state.quarantine_count == 2);
    CHECK(callback_state.unit_overflow_count == 1);

    const auto later_identity = make_identity(0x19, 3, 0);
    auto rejected_intel_source = handle_t<pci::dma_fault_source>::make(owner, later_identity);
    auto accepted_amd_source = handle_t<pci::dma_fault_source>::make(owner, later_identity);
    REQUIRE(rejected_intel_source);
    REQUIRE(accepted_amd_source);
    CHECK_FALSE(iommu::register_dma_fault_source(iommu::provider_kind::intel_vtd, unit, later_identity, 0x1918,
                                                 rejected_intel_source));
    REQUIRE(iommu::register_dma_fault_source(iommu::provider_kind::amd_vi, unit, later_identity, 0x1918,
                                             accepted_amd_source));
    iommu::unregister_dma_fault_source(iommu::provider_kind::amd_vi, unit, later_identity, 0x1918);

    iommu::unregister_dma_fault_source(iommu::provider_kind::intel_vtd, unit, identity, requester_id);
    iommu::unregister_dma_fault_source(iommu::provider_kind::amd_vi, unit, identity, requester_id);
    CHECK((intel_source->capability_signals() & NA_SIGNAL_OBJECT_REVOKED) != 0);
    CHECK((amd_source->capability_signals() & NA_SIGNAL_OBJECT_REVOKED) != 0);
    CHECK(iommu::dma_fault_source_quarantined(iommu::provider_kind::amd_vi, unit, identity, requester_id));
}

TEST_CASE("kernel fault registry invokes push and quarantine callbacks outside its lock",
          "[iommu][fault][registry][lock]")
{
    constexpr process_id owner = 92;
    constexpr u32 unit = 6;
    constexpr u16 requester_id = 0x1818;
    const auto identity = make_identity(0x18, 3, 0);
    auto source = handle_t<pci::dma_fault_source>::make(owner, identity);
    REQUIRE(source);
    REQUIRE(iommu::register_dma_fault_source(iommu::provider_kind::intel_vtd, unit, identity, requester_id, source));

    reentrant_registry_callback_state callback_state{};
    callback_state.provider = iommu::provider_kind::intel_vtd;
    callback_state.unit_index = unit;
    callback_state.identity = identity;
    callback_state.requester_id = requester_id;
    const iommu::dma_fault_event initial{0x5000, requester_id, 0x2a, pci::dma_fault_direction::device_write};
    const auto initial_dispatch = iommu::dispatch_registered_dma_fault_events(
        callback_state.provider, unit, &initial, 1, reentrant_registry_push_fault, reentrant_registry_quarantine,
        &callback_state);
    REQUIRE_FALSE(initial_dispatch.invalid_batch);
    CHECK(initial_dispatch.source_overflow_count == 0);
    CHECK(callback_state.push_count == 1);
    CHECK(callback_state.registry_query_succeeded);

    std::array<iommu::dma_fault_event, pci::max_dma_fault_queue_records> events{};
    for (u32 index = 0; index < events.size(); ++index)
        events[index] = {0x8000 + static_cast<u64>(index) * 0x1000, requester_id, 0x2c,
                         pci::dma_fault_direction::device_write};
    const auto overflow = iommu::dispatch_registered_dma_fault_events(
        callback_state.provider, unit, events.data(), static_cast<u32>(events.size()), reentrant_registry_push_fault,
        reentrant_registry_quarantine, &callback_state);
    CHECK_FALSE(overflow.invalid_batch);
    CHECK(overflow.source_overflow_count == 1);
    CHECK(callback_state.push_count == 1 + events.size());
    CHECK(callback_state.quarantine_count == 1);
    CHECK(callback_state.registry_query_succeeded);
    CHECK(callback_state.quarantine_visible);

    CHECK(iommu::quarantine_registered_dma_fault_sources(callback_state.provider, unit, true,
                                                         reentrant_registry_quarantine, &callback_state) == 1);
    CHECK(callback_state.quarantine_count == 2);
    CHECK(callback_state.quarantine_visible);
    iommu::unregister_dma_fault_source(callback_state.provider, unit, identity, requester_id);
}

TEST_CASE("AMD memory I/O page faults reach only the matching requester source", "[amd_vi][fault][dispatch]")
{
    constexpr process_id owner = 73;
    const auto first_identity = make_identity(0x12, 6, 4);
    const auto second_identity = make_identity(0xab, 0x19, 5);
    pci::dma_fault_source first_source(owner, first_identity);
    pci::dma_fault_source second_source(owner, second_identity);
    pci::dma_fault_source same_requester_other_unit(owner, first_identity);
    std::array<fault_source_registration, 3> sources{{
        {0, 0x1234, 0, true, false, &first_source},
        {0, 0xabcd, 0, true, false, &second_source},
        {1, 0x1234, 0, true, false, &same_requester_other_unit},
    }};

    const auto write = memory_fault(0x1234, 0x5000, 0x00a5'0000'0000'1234ull, true);
    const auto read = memory_fault(0xabcd, 0x6000, 0x005a'0000'0000'abcdull, false);
    auto interrupt = memory_fault(0x1234, 0xfee0'0000, 0x0012'0000'0000'1234ull, false);
    interrupt.is_interrupt = true;
    const std::array records{write, read, interrupt};

    const auto result = iommu::amd_vi::dispatch_fault_events(0, records.data(), static_cast<u32>(records.size()),
                                                             sources, push_fault, quarantine_source);

    CHECK(result.unexpected_event_count == 0);
    CHECK(result.source_overflow_count == 0);
    CHECK_FALSE(result.unit_quarantine_required);
    CHECK(sources[0].quarantine_count == 0);
    CHECK(sources[1].quarantine_count == 0);

    CHECK((first_source.capability_signals() & NA_SIGNAL_READABLE) != 0);
    pci::dma_fault_batch first_batch{};
    REQUIRE(first_source.take_faults(owner, pci::max_dma_fault_batch_records, first_batch) == NA_STATUS_OK);
    REQUIRE(first_batch.count == 1);
    CHECK(first_batch.records[0].iova == 0x5000);
    CHECK(first_batch.records[0].requester_id == 0x1234);
    CHECK(first_batch.records[0].reason == static_cast<u8>((write.raw[0] >> 48) & 0xff));
    CHECK(first_batch.records[0].direction == pci::dma_fault_direction::device_write);

    CHECK((second_source.capability_signals() & NA_SIGNAL_READABLE) != 0);
    pci::dma_fault_batch second_batch{};
    REQUIRE(second_source.take_faults(owner, pci::max_dma_fault_batch_records, second_batch) == NA_STATUS_OK);
    REQUIRE(second_batch.count == 1);
    CHECK(second_batch.records[0].iova == 0x6000);
    CHECK(second_batch.records[0].requester_id == 0xabcd);
    CHECK(second_batch.records[0].direction == pci::dma_fault_direction::device_read);

    pci::dma_fault_batch other_unit_batch{};
    REQUIRE(same_requester_other_unit.take_faults(owner, pci::max_dma_fault_batch_records, other_unit_batch) ==
            NA_STATUS_OK);
    CHECK(other_unit_batch.count == 0);
    CHECK((first_source.capability_signals() & NA_SIGNAL_READABLE) == 0);
    CHECK((second_source.capability_signals() & NA_SIGNAL_READABLE) == 0);
}

TEST_CASE("vendor-neutral DMA fault dispatch scopes units and quarantines queue overflow", "[iommu][fault][dispatch]")
{
    constexpr process_id owner = 74;
    pci::dma_fault_source first_source(owner, make_identity(0x12, 6, 4));
    pci::dma_fault_source same_requester_other_unit(owner, make_identity(0x12, 6, 4));
    pci::dma_fault_source other_requester(owner, make_identity(0xab, 0x19, 5));
    std::array<fault_source_registration, 3> sources{{
        {0, 0x1234, 0, true, false, &first_source},
        {1, 0x1234, 0, true, false, &same_requester_other_unit},
        {0, 0xabcd, 0, true, false, &other_requester},
    }};
    const std::array events{
        iommu::dma_fault_event{0x5000, 0x1234, 0x2a, pci::dma_fault_direction::device_write},
        iommu::dma_fault_event{0x6000, 0x1234, 0x2b, pci::dma_fault_direction::device_read},
    };

    const auto result = iommu::dispatch_dma_fault_events(0, events.data(), static_cast<u32>(events.size()), sources,
                                                         push_fault, quarantine_source);

    CHECK_FALSE(result.invalid_batch);
    CHECK(result.source_overflow_count == 0);
    pci::dma_fault_batch batch{};
    REQUIRE(first_source.take_faults(owner, pci::max_dma_fault_batch_records, batch) == NA_STATUS_OK);
    REQUIRE(batch.count == 2);
    CHECK(batch.records[0].iova == 0x5000);
    CHECK(batch.records[0].reason == 0x2a);
    CHECK(batch.records[0].direction == pci::dma_fault_direction::device_write);
    CHECK(batch.records[1].iova == 0x6000);
    CHECK(batch.records[1].direction == pci::dma_fault_direction::device_read);
    pci::dma_fault_batch unrelated{};
    REQUIRE(same_requester_other_unit.take_faults(owner, pci::max_dma_fault_batch_records, unrelated) == NA_STATUS_OK);
    CHECK(unrelated.count == 0);
    REQUIRE(other_requester.take_faults(owner, pci::max_dma_fault_batch_records, unrelated) == NA_STATUS_OK);
    CHECK(unrelated.count == 0);

    std::array<iommu::dma_fault_event, pci::max_dma_fault_queue_records + 3> overflow_events{};
    for (u32 index = 0; index < overflow_events.size(); ++index)
        overflow_events[index] = {0x8000 + static_cast<u64>(index) * 0x1000, 0x1234, 0x2c,
                                  pci::dma_fault_direction::device_write};
    const auto overflow = iommu::dispatch_dma_fault_events(
        0, overflow_events.data(), static_cast<u32>(overflow_events.size()), sources, push_fault, quarantine_source);
    CHECK_FALSE(overflow.invalid_batch);
    CHECK(overflow.source_overflow_count == 1);
    CHECK(sources[0].quarantined);
    CHECK(sources[0].quarantine_count == 1);
    REQUIRE(first_source.take_faults(owner, pci::max_dma_fault_batch_records, batch) == NA_STATUS_OK);
    CHECK(batch.count == pci::max_dma_fault_batch_records);
    CHECK(batch.dropped_records == 3);
}

TEST_CASE("AMD absent-page faults preserve unknown direction", "[amd_vi][fault][dispatch]")
{
    constexpr process_id owner = 77;
    pci::dma_fault_source source(owner, make_identity(0x12, 6, 4));
    std::array<fault_source_registration, 1> sources{{{0, 0x1234, 0, true, false, &source}}};
    auto fault = memory_fault(0x1234, 0x7000, 0x00a5'0000'0000'1234ull, true);
    fault.is_present = false;

    const auto result = iommu::amd_vi::dispatch_fault_events(0, &fault, 1, sources, push_fault, quarantine_source);

    CHECK(result.unexpected_event_count == 0);
    CHECK_FALSE(result.unit_quarantine_required);
    pci::dma_fault_batch batch{};
    REQUIRE(source.take_faults(owner, 1, batch) == NA_STATUS_OK);
    REQUIRE(batch.count == 1);
    CHECK(batch.records[0].direction == pci::dma_fault_direction::unknown);
}

TEST_CASE("AMD logged translation-request IOPF quarantines its unit", "[amd_vi][fault][failure]")
{
    constexpr process_id owner = 78;
    pci::dma_fault_source source(owner, make_identity(0x12, 6, 4));
    std::array<fault_source_registration, 1> sources{{{0, 0x1234, 0, true, false, &source}}};
    auto unexpected = memory_fault(0x1234, 0x8000, 0x0001'0000'0000'1234ull, true);
    unexpected.is_translation = true;

    const auto result = iommu::amd_vi::dispatch_fault_events(0, &unexpected, 1, sources, push_fault, quarantine_source);

    CHECK(result.unexpected_event_count == 1);
    CHECK(result.source_overflow_count == 0);
    CHECK(result.unit_quarantine_required);
    CHECK((source.capability_signals() & NA_SIGNAL_READABLE) == 0);
}

TEST_CASE("AMD fault-source queue overflow quarantines that requester once", "[amd_vi][fault][overflow]")
{
    constexpr process_id owner = 79;
    const auto identity = make_identity(0x12, 6, 4);
    pci::dma_fault_source source(owner, identity);
    std::array<fault_source_registration, 1> sources{{{0, 0x1234, 0, true, false, &source}}};

    std::array<iommu::amd_vi::event_record, pci::max_dma_fault_queue_records + 3> records{};
    for (u32 index = 0; index < records.size(); ++index)
        records[index] =
            memory_fault(0x1234, 0x5000 + static_cast<u64>(index) * 0x1000, 0x0001'0000'0000'1234ull, true);
    const auto result = iommu::amd_vi::dispatch_fault_events(0, records.data(), static_cast<u32>(records.size()),
                                                             sources, push_fault, quarantine_source);

    CHECK(result.unexpected_event_count == 0);
    CHECK(result.source_overflow_count == 1);
    CHECK_FALSE(result.unit_quarantine_required);
    CHECK(sources[0].quarantined);
    CHECK(sources[0].quarantine_count == 1);
    CHECK((source.capability_signals() & NA_SIGNAL_READABLE) != 0);

    pci::dma_fault_batch first_batch{};
    REQUIRE(source.take_faults(owner, pci::max_dma_fault_batch_records, first_batch) == NA_STATUS_OK);
    REQUIRE(first_batch.count == pci::max_dma_fault_batch_records);
    CHECK(first_batch.dropped_records == 3);
    CHECK(first_batch.records[0].iova == 0x5000);
    CHECK(first_batch.records[first_batch.count - 1].iova == 0x5000 + 15 * 0x1000);

    pci::dma_fault_batch second_batch{};
    REQUIRE(source.take_faults(owner, pci::max_dma_fault_batch_records, second_batch) == NA_STATUS_OK);
    REQUIRE(second_batch.count == pci::max_dma_fault_batch_records);
    CHECK(second_batch.records[0].iova == 0x5000 + 16 * 0x1000);
    CHECK(second_batch.records[second_batch.count - 1].iova == 0x5000 + 31 * 0x1000);
    CHECK((source.capability_signals() & NA_SIGNAL_READABLE) == 0);
}

TEST_CASE("Unexpected AMD event types request unit quarantine", "[amd_vi][fault][failure]")
{
    constexpr process_id owner = 83;
    pci::dma_fault_source source(owner, make_identity(0x12, 6, 4));
    std::array<fault_source_registration, 1> sources{{{0, 0x1234, 0, true, false, &source}}};
    iommu::amd_vi::event_record unexpected{};
    unexpected.event_code = 0x04;

    const auto result = iommu::amd_vi::dispatch_fault_events(0, &unexpected, 1, sources, push_fault, quarantine_source);

    CHECK(result.unexpected_event_count == 1);
    CHECK(result.source_overflow_count == 0);
    CHECK(result.unit_quarantine_required);
    CHECK_FALSE(sources[0].quarantined);
    CHECK((source.capability_signals() & NA_SIGNAL_READABLE) == 0);
}

TEST_CASE("AMD poll quarantines the unit and requester sources after dispatch rejection", "[amd_vi][fault][failure]")
{
    constexpr process_id owner = 80;
    pci::dma_fault_source source(owner, make_identity(0x12, 6, 4));
    std::array<fault_source_registration, 1> sources{{{0, 0x1234, 0, true, false, &source}}};
    auto invalid_translation = memory_fault(0x1234, 0x9000, 0x0001'0000'0000'1234ull, true);
    invalid_translation.is_translation = true;

    const auto dispatch =
        iommu::amd_vi::dispatch_fault_events(0, &invalid_translation, 1, sources, push_fault, quarantine_source);
    bool controller_quarantined = false;
    u32 bus_master_disable_requests = 0;
    const bool unit_quarantined =
        iommu::amd_vi::quarantine_fault_poll_unit(0, true, false, dispatch, [&](u32 unit_index, bool unit_overflow) {
            CHECK(unit_index == 0);
            CHECK_FALSE(unit_overflow);
            controller_quarantined = true;
            iommu::amd_vi::quarantine_fault_sources(unit_index, sources, unit_overflow,
                                                    [&](fault_source_registration &entry, bool report_overflow) {
                                                        bus_master_disable_requests++;
                                                        if (report_overflow)
                                                            entry.source->note_unit_overflow();
                                                        else
                                                            entry.source->close();
                                                    });
        });

    CHECK(unit_quarantined);
    CHECK(controller_quarantined);
    CHECK(bus_master_disable_requests == 1);
    CHECK(sources[0].quarantined);
    CHECK((source.capability_signals() & NA_SIGNAL_OBJECT_REVOKED) != 0);
    CHECK((source.capability_signals() & NA_SIGNAL_READABLE) == 0);
}

TEST_CASE("AMD poll quarantines failed drains and event overflow but leaves clean polls alone",
          "[amd_vi][fault][failure]")
{
    const iommu::amd_vi::fault_dispatch_result no_faults{};
    u32 quarantine_calls = 0;
    bool last_overflow = false;
    const auto quarantine = [&](u32 unit_index, bool event_log_overflow) {
        CHECK(unit_index == 3);
        quarantine_calls++;
        last_overflow = event_log_overflow;
    };

    CHECK_FALSE(iommu::amd_vi::quarantine_fault_poll_unit(3, true, false, no_faults, quarantine));
    CHECK(quarantine_calls == 0);

    CHECK(iommu::amd_vi::quarantine_fault_poll_unit(3, false, false, no_faults, quarantine));
    CHECK(quarantine_calls == 1);
    CHECK_FALSE(last_overflow);

    CHECK(iommu::amd_vi::quarantine_fault_poll_unit(3, true, true, no_faults, quarantine));
    CHECK(quarantine_calls == 2);
    CHECK(last_overflow);
}

TEST_CASE("AMD event-log overflow reaches every source on only the affected unit", "[amd_vi][fault][overflow]")
{
    constexpr process_id owner = 89;
    const auto first_identity = make_identity(0x12, 6, 4);
    const auto second_identity = make_identity(0xab, 0x19, 5);
    pci::dma_fault_source first_source(owner, first_identity);
    pci::dma_fault_source second_source(owner, second_identity);
    pci::dma_fault_source other_unit_source(owner, first_identity);
    std::array<fault_source_registration, 3> sources{{
        {0, 0x1234, 0, true, false, &first_source},
        {0, 0xabcd, 0, true, false, &second_source},
        {1, 0x1234, 0, true, false, &other_unit_source},
    }};

    const u32 count = iommu::amd_vi::quarantine_fault_sources(0, sources, true, quarantine_fault_source);

    CHECK(count == 2);
    CHECK(sources[0].quarantined);
    CHECK(sources[1].quarantined);
    CHECK_FALSE(sources[2].quarantined);
    CHECK(sources[0].quarantine_count == 1);
    CHECK(sources[1].quarantine_count == 1);
    CHECK(sources[2].quarantine_count == 0);

    for (auto *source : std::array<pci::dma_fault_source *, 2>{&first_source, &second_source})
    {
        CHECK((source->capability_signals() & NA_SIGNAL_READABLE) != 0);
        pci::dma_fault_batch batch{};
        REQUIRE(source->take_faults(owner, pci::max_dma_fault_batch_records, batch) == NA_STATUS_OK);
        CHECK(batch.count == 0);
        CHECK(batch.dropped_records == 0);
        CHECK(batch.unit_overflow);
        CHECK((source->capability_signals() & NA_SIGNAL_READABLE) == 0);
    }
    CHECK((other_unit_source.capability_signals() & NA_SIGNAL_READABLE) == 0);
    CHECK((other_unit_source.capability_signals() & NA_SIGNAL_OBJECT_REVOKED) == 0);
}

TEST_CASE("AMD event-drain failure closes only sources on the failed unit", "[amd_vi][fault][failure]")
{
    constexpr process_id owner = 97;
    const auto identity = make_identity(0x12, 6, 4);
    pci::dma_fault_source affected_source(owner, identity);
    pci::dma_fault_source other_unit_source(owner, identity);
    std::array<fault_source_registration, 2> sources{{
        {0, 0x1234, 0, true, false, &affected_source},
        {1, 0x1234, 0, true, false, &other_unit_source},
    }};

    const u32 count = iommu::amd_vi::quarantine_fault_sources(0, sources, false, quarantine_fault_source);

    CHECK(count == 1);
    CHECK(sources[0].quarantined);
    CHECK_FALSE(sources[1].quarantined);
    CHECK(sources[0].quarantine_count == 1);
    CHECK(sources[1].quarantine_count == 0);
    CHECK((affected_source.capability_signals() & NA_SIGNAL_OBJECT_REVOKED) != 0);
    CHECK((affected_source.capability_signals() & NA_SIGNAL_READABLE) == 0);
    CHECK((other_unit_source.capability_signals() & NA_SIGNAL_OBJECT_REVOKED) == 0);
}
