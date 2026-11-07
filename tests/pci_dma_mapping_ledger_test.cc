#include <catch2/catch_test_macros.hpp>

#include "kernel/pci/dma_mapping_ledger.hpp"

#include <memory>

TEST_CASE("DMA mapping ledger reserves and commits a bounded IOVA exactly once", "[pci][dma]")
{
    pci::dma_mapping_ledger<std::shared_ptr<int>, 2> ledger;
    u32 first_slot = 99;
    CHECK(ledger.reserve(0x1000, 4096, 39, first_slot) == pci::dma_mapping_error::none);
    CHECK(first_slot == 0);

    u32 duplicate_slot = 99;
    CHECK(ledger.reserve(0x1000, 4096, 39, duplicate_slot) == pci::dma_mapping_error::already_present);
    CHECK(ledger.reserve(0x1001, 4096, 39, duplicate_slot) == pci::dma_mapping_error::invalid_address);
    CHECK(ledger.reserve(0x200000, 4096, 21, duplicate_slot) == pci::dma_mapping_error::address_width_exceeded);

    auto backing = std::make_shared<int>(7);
    CHECK(ledger.commit(first_slot, 0x8000, naos::data_plane::dma_direction::device_reads, backing) ==
          pci::dma_mapping_error::none);
    CHECK(backing.use_count() == 2);
    const auto *record = ledger.find(0x1000);
    REQUIRE(record != nullptr);
    CHECK(record->state == pci::dma_mapping_state::mapped);
    CHECK(record->physical_address == 0x8000);
    CHECK(record->direction == naos::data_plane::dma_direction::device_reads);
    CHECK(record->resource.get() == backing.get());

    std::shared_ptr<int> released;
    CHECK(ledger.release_after_unmap(0x1000, true, released) == pci::dma_mapping_error::bus_master_enabled);
    CHECK(ledger.release_after_unmap(0x1000, false, released) == pci::dma_mapping_error::none);
    CHECK(released == backing);
    CHECK(ledger.find(0x1000) == nullptr);
    CHECK(ledger.size() == 0);

    CHECK(ledger.reserve(0x3000, 4096, 39, first_slot) == pci::dma_mapping_error::none);
    auto failed_backing = std::make_shared<int>(9);
    CHECK(ledger.commit(first_slot, 0xc000, naos::data_plane::dma_direction::device_reads, failed_backing) ==
          pci::dma_mapping_error::none);
    CHECK(ledger.rollback_failed_map(0x3000, released) == pci::dma_mapping_error::none);
    CHECK(released == failed_backing);
    CHECK(ledger.find(0x3000) == nullptr);
}

TEST_CASE("DMA mapping ledger rolls back pre-map reservations and quarantines uncertain mappings", "[pci][dma]")
{
    pci::dma_mapping_ledger<std::shared_ptr<int>, 1> ledger;
    u32 slot = 99;
    CHECK(ledger.reserve(0x4000, 4096, 48, slot) == pci::dma_mapping_error::none);
    CHECK(ledger.cancel(slot) == pci::dma_mapping_error::none);
    CHECK(ledger.size() == 0);

    CHECK(ledger.reserve(0x4000, 4096, 48, slot) == pci::dma_mapping_error::none);
    auto backing = std::make_shared<int>(11);
    CHECK(ledger.commit(slot, 0xa000, naos::data_plane::dma_direction::device_writes, backing) ==
          pci::dma_mapping_error::none);
    CHECK(ledger.mark_uncertain(0x4000) == pci::dma_mapping_error::none);
    CHECK(ledger.find(0x4000)->state == pci::dma_mapping_state::uncertain);

    std::shared_ptr<int> released;
    CHECK(ledger.release_after_unmap(0x4000, false, released) == pci::dma_mapping_error::state_uncertain);
    CHECK(released == nullptr);
    CHECK(backing.use_count() == 2);
    CHECK(ledger.reserve(0x8000, 4096, 48, slot) == pci::dma_mapping_error::capacity_exhausted);
}
