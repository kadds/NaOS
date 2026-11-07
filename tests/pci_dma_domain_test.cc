#include <catch2/catch_test_macros.hpp>
#include <utility>

#include "kernel/arch/mm.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/pci/dma_domain.hpp"
#include "kernel/pci/dma_fault_source.hpp"
#include "kernel/pci/dma_status.hpp"
#include "kernel/physical_resource.hpp"
#include "kernel/resource.hpp"

#include <cstdlib>

namespace
{
struct backend_fixture
{
    u32 enable_calls = 0;
    u32 disable_calls = 0;
    u32 destroy_calls = 0;
    bool saw_authority = false;
    bool fail_disable = false;
    bool fail_destroy = false;
    bool fail_map = false;
    bool uncertain_map = false;
    u32 map_calls = 0;
    u32 unmap_calls = 0;
    u64 last_physical_address = 0;
    pci::function_identity target{};
};

struct physical_resource_fixture
{
    physical_resource::ledger ledger;
    physical_resource::root root;

    physical_resource_fixture()
        : ledger(memory::KernelCommonAllocatorV)
        , root(&ledger)
    {
    }
};

constexpr process_id test_owner_pid = 7;

na_status_t map_page(void *context, void *, u64, u64 physical_address, naos::data_plane::dma_direction, bool &uncertain)
{
    auto &fixture = *static_cast<backend_fixture *>(context);
    fixture.map_calls++;
    fixture.last_physical_address = physical_address;
    uncertain = fixture.uncertain_map;
    return fixture.fail_map ? NA_STATUS_IO_ERROR : NA_STATUS_OK;
}

na_status_t unmap_page(void *context, void *, u64, bool &uncertain)
{
    auto &fixture = *static_cast<backend_fixture *>(context);
    fixture.unmap_calls++;
    uncertain = false;
    return NA_STATUS_OK;
}

na_status_t set_bus_master(void *context, void *, bool enabled, const pci::bus_master_enable_authority *authority,
                           bool &uncertain)
{
    auto &fixture = *static_cast<backend_fixture *>(context);
    uncertain = false;
    if (enabled)
    {
        fixture.enable_calls++;
        fixture.saw_authority = authority != nullptr && authority->segment() == fixture.target.segment &&
                                authority->bus() == fixture.target.bus &&
                                authority->device() == fixture.target.device &&
                                authority->function() == fixture.target.function && authority->state().domain_ready;
        return fixture.saw_authority ? NA_STATUS_OK : NA_STATUS_ACCESS_DENIED;
    }
    fixture.disable_calls++;
    if (fixture.fail_disable)
    {
        uncertain = true;
        // The status is deliberately not an I/O error: once the backend says
        // the hardware outcome is uncertain, callers must not mistake this
        // known-looking status for a definitive denial.
        return NA_STATUS_ACCESS_DENIED;
    }
    return NA_STATUS_OK;
}

na_status_t destroy_domain(void *context, void *, bool &uncertain)
{
    auto &fixture = *static_cast<backend_fixture *>(context);
    fixture.destroy_calls++;
    uncertain = fixture.fail_destroy;
    return fixture.fail_destroy ? NA_STATUS_IO_ERROR : NA_STATUS_OK;
}

pci::dma_domain::backend_ops operations() { return {map_page, unmap_page, set_bus_master, destroy_domain}; }

pci::function_identity make_identity()
{
    pci::function_identity result{};
    result.segment = 2;
    result.bus = 9;
    result.device = 3;
    result.function = 1;
    result.firmware_generation = 103;
    return result;
}

bool reserve_dma_page(physical_resource_fixture &resources, u64 physical_address, u64 generation,
                      physical_resource::lease &lease)
{
    constexpr u64 page_size = 4096;
    return resources.ledger.reserve(resources.root, physical_address, page_size, physical_address,
                                    physical_address + page_size, physical_resource::kind::dma_window,
                                    physical_resource::cache::write_back, physical_resource::access::read_write,
                                    generation, lease);
}
} // namespace

TEST_CASE("DMA domain rejects every operation from a non-owner process", "[pci][dma][ownership]")
{
    backend_fixture fixture{};
    fixture.target = make_identity();
    const process_id owner = 41;
    const process_id non_owner = 42;
    const pci::dma_domain::configuration config{true, false, true, false, 39};
    pci::dma_domain domain(owner, make_identity(), operations(), &fixture, &fixture, config);

    CHECK(domain.map_page(non_owner, {}, 0x5000, 0, naos::data_plane::dma_direction::device_reads).status ==
          NA_STATUS_ACCESS_DENIED);
    CHECK(domain.unmap_page(non_owner, 0x5000).status == NA_STATUS_ACCESS_DENIED);
    CHECK(domain.set_bus_master(non_owner, true).status == NA_STATUS_ACCESS_DENIED);
    CHECK(domain.close(non_owner).status == NA_STATUS_ACCESS_DENIED);
    CHECK(fixture.enable_calls == 0);
    CHECK(fixture.disable_calls == 0);
    CHECK(fixture.destroy_calls == 0);

    CHECK(domain.close(owner).status == NA_STATUS_OK);
    CHECK(fixture.destroy_calls == 1);
    CHECK((pci::dma_capability_meta_rights & NA_RIGHT_TRANSFER) != 0);
    CHECK((pci::dma_capability_meta_rights & NA_RIGHT_WAIT) != 0);
    CHECK((pci::dma_capability_meta_rights & NA_RIGHT_INSPECT) != 0);
}

TEST_CASE("DMA fault source rejects a non-owner without consuming its queue", "[pci][dma][ownership]")
{
    const process_id owner = 51;
    const process_id non_owner = 52;
    const auto identity = make_identity();
    const u16 requester_id = static_cast<u16>((identity.bus << 8) | (identity.device << 3) | identity.function);
    pci::dma_fault_source source(owner, identity);
    CHECK(source.push_fault({0x5000, requester_id, 0x05, pci::dma_fault_direction::device_write}));

    pci::dma_fault_batch batch{};
    CHECK(source.take_faults(non_owner, 1, batch) == NA_STATUS_ACCESS_DENIED);
    CHECK((source.capability_signals() & NA_SIGNAL_READABLE) != 0);
    CHECK(source.take_faults(owner, 1, batch) == NA_STATUS_OK);
    REQUIRE(batch.count == 1);
    CHECK(batch.records[0].iova == 0x5000);
}

TEST_CASE("moving DMA capabilities between resource tables preserves creator ownership",
          "[pci][dma][ownership][transfer]")
{
    backend_fixture fixture{};
    const auto identity = make_identity();
    fixture.target = identity;
    const process_id owner = 61;
    const process_id receiver = 62;
    const pci::dma_domain::configuration config{true, false, true, false, 39};
    auto domain = handle_t<pci::dma_domain>::make(owner, identity, operations(), &fixture, &fixture, config);
    auto fault_source = handle_t<pci::dma_fault_source>::make(owner, identity);
    REQUIRE(domain);
    REQUIRE(fault_source);

    const u16 requester_id = static_cast<u16>((identity.bus << 8) | (identity.device << 3) | identity.function);
    CHECK(fault_source->push_fault({0x7000, requester_id, 0x05, pci::dma_fault_direction::device_write}));

    task::resource_table_t owner_resources;
    task::resource_table_t receiver_resources;
    capability::metadata metadata{};
    metadata.meta_rights = NA_RIGHT_TRANSFER | NA_RIGHT_WAIT | NA_RIGHT_INSPECT;
    const na_handle_t domain_handle = owner_resources.install_native(domain.as<kobject>(), metadata);
    const na_handle_t fault_handle = owner_resources.install_native(fault_source.as<kobject>(), metadata);
    REQUIRE(domain_handle != NA_HANDLE_INVALID);
    REQUIRE(fault_handle != NA_HANDLE_INVALID);

    na_resource_disposition_t dispositions[2]{};
    dispositions[0].handle = domain_handle;
    dispositions[0].operation = NA_RESOURCE_MOVE;
    dispositions[0].rights = metadata.meta_rights;
    dispositions[1].handle = fault_handle;
    dispositions[1].operation = NA_RESOURCE_MOVE;
    dispositions[1].rights = metadata.meta_rights;

    capability::transfer_record_list records(memory::KernelCommonAllocatorV);
    REQUIRE(owner_resources.take_native_batch(dispositions, 2, NA_HANDLE_INVALID, records) == NA_STATUS_OK);
    capability::entry source_entry{};
    CHECK_FALSE(owner_resources.lookup_native(domain_handle, source_entry));
    CHECK_FALSE(owner_resources.lookup_native(fault_handle, source_entry));

    freelibcxx::vector<na_handle_t> receiver_handles(memory::KernelCommonAllocatorV);
    REQUIRE(receiver_resources.reserve_native(receiver_handles, 2) == NA_STATUS_OK);
    REQUIRE(receiver_resources.activate_native(receiver_handles[0], std::move(records[0].resource)) == NA_STATUS_OK);
    REQUIRE(receiver_resources.activate_native(receiver_handles[1], std::move(records[1].resource)) == NA_STATUS_OK);
    owner_resources.commit_native_batch(records);

    capability::entry received_domain_entry{};
    capability::entry received_fault_entry{};
    REQUIRE(receiver_resources.lookup_native(receiver_handles[0], received_domain_entry));
    REQUIRE(receiver_resources.lookup_native(receiver_handles[1], received_fault_entry));
    auto received_domain = received_domain_entry.object.as<pci::dma_domain>();
    auto received_fault_source = received_fault_entry.object.as<pci::dma_fault_source>();
    REQUIRE(received_domain);
    REQUIRE(received_fault_source);
    CHECK(received_domain->owner() == owner);

    CHECK(received_domain->map_page(receiver, {}, 0x7000, 0, naos::data_plane::dma_direction::device_reads).status ==
          NA_STATUS_ACCESS_DENIED);
    CHECK(received_domain->set_bus_master(receiver, true).status == NA_STATUS_ACCESS_DENIED);
    CHECK(received_domain->close(receiver).status == NA_STATUS_ACCESS_DENIED);
    CHECK(fixture.enable_calls == 0);
    CHECK(fixture.disable_calls == 0);
    CHECK(fixture.destroy_calls == 0);

    pci::dma_fault_batch batch{};
    CHECK(received_fault_source->take_faults(receiver, 1, batch) == NA_STATUS_ACCESS_DENIED);
    CHECK((received_fault_source->capability_signals() & NA_SIGNAL_READABLE) != 0);
    CHECK(received_fault_source->take_faults(owner, 1, batch) == NA_STATUS_OK);
    REQUIRE(batch.count == 1);
    CHECK(batch.records[0].iova == 0x7000);

    CHECK(received_domain->close(owner).status == NA_STATUS_OK);
    CHECK(fixture.destroy_calls == 1);
}

TEST_CASE("DMA protocol keeps revoked resources distinct from denied access", "[pci][dma][protocol]")
{
    CHECK(pci::dma::status_errno(NA_STATUS_ACCESS_DENIED) == EACCES);
    CHECK(pci::dma::status_errno(NA_STATUS_OBJECT_REVOKED) == EIDRM);
    CHECK(pci::dma::status_errno(NA_STATUS_OBJECT_REVOKED) != ESTALE);
}

TEST_CASE("DMA domain denies BME without an IOMMU or trusted-driver override", "[pci][dma]")
{
    backend_fixture fixture{};
    fixture.target = make_identity();
    const pci::dma_domain::configuration config{false, false, true, false, 39};
    {
        pci::dma_domain domain(test_owner_pid, make_identity(), operations(), &fixture, &fixture, config);
        CHECK(domain.set_bus_master(test_owner_pid, true).status == NA_STATUS_ACCESS_DENIED);
        CHECK(fixture.enable_calls == 0);
        CHECK(domain.set_bus_master(test_owner_pid, false).status == NA_STATUS_OK);
        CHECK(fixture.disable_calls == 0);
    }
    CHECK(fixture.disable_calls == 1);
    CHECK(fixture.destroy_calls == 1);
}

TEST_CASE("DMA domain passes a BDF-bound authority only after trusted policy admits BME", "[pci][dma]")
{
    backend_fixture fixture{};
    fixture.target = make_identity();
    const pci::dma_domain::configuration config{false, true, true, false, 39};
    {
        pci::dma_domain domain(test_owner_pid, make_identity(), operations(), &fixture, &fixture, config);
        CHECK(domain.set_bus_master(test_owner_pid, true).status == NA_STATUS_OK);
        CHECK(fixture.enable_calls == 1);
        CHECK(fixture.saw_authority);
        CHECK(domain.set_bus_master(test_owner_pid, false).status == NA_STATUS_OK);
        CHECK(fixture.disable_calls == 1);
    }
    CHECK(fixture.destroy_calls == 1);
}

TEST_CASE("DMA domain treats an uncertain BME outcome as quarantined state", "[pci][dma]")
{
    backend_fixture fixture{};
    fixture.target = make_identity();
    fixture.fail_disable = true;
    const pci::dma_domain::configuration config{true, false, true, false, 39};
    {
        pci::dma_domain domain(test_owner_pid, make_identity(), operations(), &fixture, &fixture, config);
        CHECK(domain.set_bus_master(test_owner_pid, true).status == NA_STATUS_OK);
        const auto disable_result = domain.set_bus_master(test_owner_pid, false);
        CHECK(disable_result.status == NA_STATUS_IO_ERROR);
        CHECK(disable_result.outcome_unknown);
        CHECK(domain.state_uncertain());
        const auto retry_result = domain.set_bus_master(test_owner_pid, true);
        CHECK(retry_result.status == NA_STATUS_IO_ERROR);
        CHECK_FALSE(retry_result.outcome_unknown);
    }
    CHECK(fixture.destroy_calls == 0);
}

TEST_CASE("DMA domain exposes uncertain hardware teardown as an unknown outcome", "[pci][dma]")
{
    backend_fixture fixture{};
    fixture.target = make_identity();
    fixture.fail_destroy = true;
    const pci::dma_domain::configuration config{true, false, true, false, 39};
    pci::dma_domain domain(test_owner_pid, make_identity(), operations(), &fixture, &fixture, config);

    const auto result = domain.close(test_owner_pid);
    CHECK(result.status == NA_STATUS_IO_ERROR);
    CHECK(result.outcome_unknown);
    CHECK(domain.state_uncertain());
    CHECK(fixture.destroy_calls == 1);
}

TEST_CASE("DMA page mappings reserve physical ownership until confirmed unmap", "[pci][dma][physical_resource]")
{
    backend_fixture backend{};
    backend.target = make_identity();
    physical_resource_fixture resources;
    const pci::dma_domain::configuration config{true, false, true, false, 39};
    auto domain = handle_t<pci::dma_domain>::make(test_owner_pid, backend.target, operations(), &backend, &backend,
                                                  config, &resources.root);
    auto backing = handle_t<naos::data_plane::memory_object>::make(memory::page_size, 0);
    REQUIRE(domain);
    REQUIRE(backing);
    const u64 physical_address = 0x9000'0000;

    const auto mapped = domain->map_page(test_owner_pid, std::move(backing), 0x8000, 0,
                                         naos::data_plane::dma_direction::device_reads_and_writes);
    CHECK(mapped.status == NA_STATUS_OK);
    CHECK(backend.map_calls == 1);
    CHECK(backend.last_physical_address == physical_address);

    physical_resource::lease collision{};
    CHECK_FALSE(reserve_dma_page(resources, physical_address, backend.target.firmware_generation, collision));

    CHECK(domain->unmap_page(test_owner_pid, 0x8000).status == NA_STATUS_OK);
    REQUIRE(reserve_dma_page(resources, physical_address, backend.target.firmware_generation, collision));
    CHECK(resources.ledger.revoke_kernel(collision.id));
    CHECK(domain->close(test_owner_pid).status == NA_STATUS_OK);
}

TEST_CASE("certain DMA map failure releases its physical ownership lease", "[pci][dma][physical_resource]")
{
    backend_fixture backend{};
    backend.target = make_identity();
    backend.fail_map = true;
    physical_resource_fixture resources;
    const pci::dma_domain::configuration config{true, false, true, false, 39};
    auto domain = handle_t<pci::dma_domain>::make(test_owner_pid, backend.target, operations(), &backend, &backend,
                                                  config, &resources.root);
    auto backing = handle_t<naos::data_plane::memory_object>::make(memory::page_size, 0);
    REQUIRE(domain);
    REQUIRE(backing);

    CHECK(domain->map_page(test_owner_pid, std::move(backing), 0x8000, 0, naos::data_plane::dma_direction::device_reads)
              .status == NA_STATUS_IO_ERROR);
    CHECK_FALSE(domain->state_uncertain());
    physical_resource::lease collision{};
    REQUIRE(reserve_dma_page(resources, 0x9000'0000, backend.target.firmware_generation, collision));
    CHECK(resources.ledger.revoke_kernel(collision.id));
    CHECK(domain->close(test_owner_pid).status == NA_STATUS_OK);
}

TEST_CASE("uncertain DMA map retains physical ownership and backing storage", "[pci][dma][physical_resource]")
{
    backend_fixture backend{};
    backend.target = make_identity();
    backend.fail_map = true;
    backend.uncertain_map = true;
    physical_resource_fixture resources;
    const pci::dma_domain::configuration config{true, false, true, false, 39};
    auto domain = handle_t<pci::dma_domain>::make(test_owner_pid, backend.target, operations(), &backend, &backend,
                                                  config, &resources.root);
    auto backing = handle_t<naos::data_plane::memory_object>::make(memory::page_size, 0);
    REQUIRE(domain);
    REQUIRE(backing);

    const auto mapped =
        domain->map_page(test_owner_pid, std::move(backing), 0x8000, 0, naos::data_plane::dma_direction::device_reads);
    CHECK(mapped.status == NA_STATUS_IO_ERROR);
    CHECK(mapped.outcome_unknown);
    CHECK(domain->state_uncertain());
    domain.reset();

    physical_resource::lease collision{};
    CHECK_FALSE(reserve_dma_page(resources, 0x9000'0000, backend.target.firmware_generation, collision));
}

TEST_CASE("DMA fault source filters by bound requester and drains records in order", "[pci][dma][fault]")
{
    auto identity = make_identity();
    identity.boot_epoch = 101;
    identity.firmware_generation = 102;
    identity.aml_namespace_generation = 103;
    identity.platform_topology_generation = 104;
    identity.pci_fabric_generation = 105;
    const u16 requester_id = static_cast<u16>((identity.bus << 8) | (identity.device << 3) | identity.function);
    pci::dma_fault_source source(test_owner_pid, identity);

    CHECK((source.capability_signals() & NA_SIGNAL_READABLE) == 0);
    CHECK_FALSE(source.push_fault(
        {0xdead'0000, static_cast<u16>(requester_id + 1), 0x05, pci::dma_fault_direction::device_write}));
    CHECK(source.push_fault({0x5000, requester_id, 0x05, pci::dma_fault_direction::device_write}));
    CHECK(source.push_fault({0x6000, requester_id, 0x06, pci::dma_fault_direction::device_read}));
    CHECK((source.capability_signals() & NA_SIGNAL_READABLE) != 0);

    pci::dma_fault_batch batch{};
    REQUIRE(source.take_faults(test_owner_pid, 1, batch) == NA_STATUS_OK);
    REQUIRE(batch.count == 1);
    CHECK(batch.boot_epoch == identity.boot_epoch);
    CHECK(batch.firmware_generation == identity.firmware_generation);
    CHECK(batch.aml_namespace_generation == identity.aml_namespace_generation);
    CHECK(batch.platform_topology_generation == identity.platform_topology_generation);
    CHECK(batch.pci_fabric_generation == identity.pci_fabric_generation);
    CHECK(batch.records[0].iova == 0x5000);
    CHECK(batch.records[0].requester_id == requester_id);
    CHECK(batch.records[0].reason == 0x05);
    CHECK(batch.records[0].direction == pci::dma_fault_direction::device_write);
    CHECK((source.capability_signals() & NA_SIGNAL_READABLE) != 0);

    REQUIRE(source.take_faults(test_owner_pid, pci::max_dma_fault_batch_records, batch) == NA_STATUS_OK);
    REQUIRE(batch.count == 1);
    CHECK(batch.records[0].iova == 0x6000);
    CHECK(batch.records[0].reason == 0x06);
    CHECK(batch.records[0].direction == pci::dma_fault_direction::device_read);
    CHECK((source.capability_signals() & NA_SIGNAL_READABLE) == 0);
}

TEST_CASE("DMA fault source reports bounded-queue loss without losing queued faults", "[pci][dma][fault]")
{
    const auto identity = make_identity();
    const u16 requester_id = static_cast<u16>((identity.bus << 8) | (identity.device << 3) | identity.function);
    pci::dma_fault_source source(test_owner_pid, identity);

    for (u32 index = 0; index < pci::max_dma_fault_queue_records + 1; ++index)
        source.push_fault(
            {0x4000 + static_cast<u64>(index) * 0x1000, requester_id, 0x05, pci::dma_fault_direction::device_write});

    pci::dma_fault_batch batch{};
    REQUIRE(source.take_faults(test_owner_pid, pci::max_dma_fault_batch_records, batch) == NA_STATUS_OK);
    CHECK(batch.count == pci::max_dma_fault_batch_records);
    CHECK(batch.dropped_records == 1);
    CHECK(batch.records[0].iova == 0x4000);
    CHECK(batch.records[batch.count - 1].iova ==
          0x4000 + static_cast<u64>(pci::max_dma_fault_batch_records - 1) * 0x1000);
    CHECK((source.capability_signals() & NA_SIGNAL_READABLE) != 0);
}

TEST_CASE("DMA fault source reports unit-wide hardware overflow without inventing a record", "[pci][dma][fault]")
{
    const auto identity = make_identity();
    const u16 requester_id = static_cast<u16>((identity.bus << 8) | (identity.device << 3) | identity.function);
    pci::dma_fault_source source(test_owner_pid, identity);
    CHECK((source.capability_signals() & NA_SIGNAL_READABLE) == 0);

    CHECK(source.note_unit_overflow());
    CHECK((source.capability_signals() & NA_SIGNAL_READABLE) != 0);
    const u64 first_sequence = source.capability_state();
    CHECK(source.note_unit_overflow());
    CHECK(source.capability_state() == first_sequence);

    pci::dma_fault_batch batch{};
    REQUIRE(source.take_faults(test_owner_pid, pci::max_dma_fault_batch_records, batch) == NA_STATUS_OK);
    CHECK(batch.count == 0);
    CHECK(batch.dropped_records == 0);
    CHECK(batch.unit_overflow);
    CHECK((source.capability_signals() & NA_SIGNAL_READABLE) == 0);

    REQUIRE(source.take_faults(test_owner_pid, pci::max_dma_fault_batch_records, batch) == NA_STATUS_OK);
    CHECK_FALSE(batch.unit_overflow);

    CHECK(source.push_fault({0x7000, requester_id, 0x05, pci::dma_fault_direction::device_write}));
    CHECK(source.note_unit_overflow());
    CHECK(source.capability_state() == first_sequence + 2);
    source.close();
    CHECK_FALSE(source.note_unit_overflow());
    REQUIRE(source.take_faults(test_owner_pid, pci::max_dma_fault_batch_records, batch) == NA_STATUS_OK);
    CHECK(batch.count == 1);
    CHECK(batch.records[0].iova == 0x7000);
    CHECK(batch.unit_overflow);
    CHECK((source.capability_signals() & NA_SIGNAL_OBJECT_REVOKED) != 0);
    CHECK((source.capability_signals() & NA_SIGNAL_READABLE) == 0);

    CHECK(source.take_faults(test_owner_pid, pci::max_dma_fault_batch_records, batch) == NA_STATUS_OBJECT_REVOKED);
    CHECK(batch.count == 0);
    CHECK_FALSE(batch.unit_overflow);
}
