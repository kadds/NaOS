#include <catch2/catch_test_macros.hpp>

#include "kernel/iommu/intel_vtd_page_table.hpp"

#include <array>

namespace
{
constexpr u64 page_size = 4096;
constexpr u64 page_address_mask = ~u64{page_size - 1};
constexpr u32 fake_page_count = 64;

struct fake_page_store
{
    alignas(page_size) std::array<std::array<u8, page_size>, fake_page_count> pages{};
    std::array<bool, fake_page_count> allocated{};
    u64 next_physical = 0x01000000;
    u32 allocation_count = 0;
    u32 release_count = 0;
    u32 flush_count = 0;
    u32 allocation_limit = fake_page_count;
    bool fail_flush = false;
    u32 fail_flush_on_call = 0;
};

bool allocate_table_page(void *context, void *&virtual_page, u64 &physical_address)
{
    auto &store = *static_cast<fake_page_store *>(context);
    if (store.allocation_count >= store.allocation_limit)
        return false;
    for (u32 index = 0; index < store.pages.size(); ++index)
    {
        if (store.allocated[index])
            continue;
        store.allocated[index] = true;
        store.pages[index].fill(0xaa);
        store.allocation_count++;
        physical_address = 0x01000000 + static_cast<u64>(index) * page_size;
        virtual_page = store.pages[index].data();
        return true;
    }
    return false;
}

void release_table_page(void *context, void *virtual_page)
{
    auto &store = *static_cast<fake_page_store *>(context);
    for (u32 index = 0; index < store.pages.size(); ++index)
    {
        if (store.pages[index].data() == virtual_page && store.allocated[index])
        {
            store.allocated[index] = false;
            store.release_count++;
            return;
        }
    }
}

void *resolve_table_page(void *context, u64 physical_address)
{
    auto &store = *static_cast<fake_page_store *>(context);
    if ((physical_address & (page_size - 1)) != 0 || physical_address < 0x01000000)
        return nullptr;
    const u64 index = (physical_address - 0x01000000) / page_size;
    if (index >= store.pages.size() || !store.allocated[index])
        return nullptr;
    return store.pages[index].data();
}

bool flush_table_page(void *context, void *)
{
    auto &store = *static_cast<fake_page_store *>(context);
    store.flush_count++;
    return !store.fail_flush && store.flush_count != store.fail_flush_on_call;
}

iommu::intel_vtd::page_table_ops operations(fake_page_store &store)
{
    return {allocate_table_page, release_table_page, resolve_table_page, flush_table_page, &store};
}

u64 *lookup_leaf(fake_page_store &store, const iommu::intel_vtd::second_level_table &table, u64 iova)
{
    const u32 level_count = (table.address_width - 12) / 9;
    auto *entries = static_cast<u64 *>(table.root_virtual);
    for (u32 level = level_count; level > 1; --level)
    {
        const u32 shift = 12 + 9 * (level - 1);
        const u32 index = static_cast<u32>((iova >> shift) & 0x1ff);
        const u64 entry = entries[index];
        if ((entry & 1) == 0)
            return nullptr;
        entries = static_cast<u64 *>(resolve_table_page(&store, entry & page_address_mask));
        if (entries == nullptr)
            return nullptr;
    }
    return &entries[(iova >> 12) & 0x1ff];
}

} // namespace

TEST_CASE("VT-d second-level tables map and unmap bounded 4 KiB DMA pages", "[intel_vtd][dma]")
{
    fake_page_store store{};
    const auto io = operations(store);
    iommu::intel_vtd::second_level_table table{};

    REQUIRE(iommu::intel_vtd::create_second_level_table(48, 48, true, io, table) ==
            iommu::intel_vtd::page_table_error::none);
    CHECK(store.allocation_count == 1);

    constexpr u64 iova = 0x123456789000;
    constexpr u64 physical = 0x000000024000;
    REQUIRE(iommu::intel_vtd::map_dma_page(table, iova, physical, iommu::intel_vtd::dma_access::read_write) ==
            iommu::intel_vtd::page_table_error::none);
    auto *leaf = lookup_leaf(store, table, iova);
    REQUIRE(leaf != nullptr);
    CHECK((*leaf & page_address_mask) == physical);
    CHECK((*leaf & 0x3) == 0x3);

    CHECK(iommu::intel_vtd::map_dma_page(table, iova, physical + page_size, iommu::intel_vtd::dma_access::read) ==
          iommu::intel_vtd::page_table_error::already_mapped);
    CHECK((*leaf & page_address_mask) == physical);

    constexpr u64 write_only_iova = iova + page_size;
    REQUIRE(iommu::intel_vtd::map_dma_page(table, write_only_iova, physical + page_size,
                                           iommu::intel_vtd::dma_access::write) ==
            iommu::intel_vtd::page_table_error::none);
    auto *write_only_leaf = lookup_leaf(store, table, write_only_iova);
    REQUIRE(write_only_leaf != nullptr);
    CHECK((*write_only_leaf & 0x3) == 0x2);
    REQUIRE(iommu::intel_vtd::unmap_dma_page(table, write_only_iova) == iommu::intel_vtd::page_table_error::none);

    REQUIRE(iommu::intel_vtd::unmap_dma_page(table, iova) == iommu::intel_vtd::page_table_error::none);
    CHECK(*leaf == 0);
    CHECK(iommu::intel_vtd::unmap_dma_page(table, iova) == iommu::intel_vtd::page_table_error::not_mapped);
    CHECK(iommu::intel_vtd::destroy_second_level_table(table) == iommu::intel_vtd::page_table_error::none);
    CHECK(store.release_count == store.allocation_count);
}

TEST_CASE("VT-d second-level tables enforce address and permission bounds", "[intel_vtd][dma]")
{
    fake_page_store store{};
    const auto io = operations(store);
    iommu::intel_vtd::second_level_table table{};
    REQUIRE(iommu::intel_vtd::create_second_level_table(39, 48, true, io, table) ==
            iommu::intel_vtd::page_table_error::none);

    CHECK(iommu::intel_vtd::map_dma_page(table, 0x1001, 0x2000, iommu::intel_vtd::dma_access::read) ==
          iommu::intel_vtd::page_table_error::invalid_address);
    CHECK(iommu::intel_vtd::map_dma_page(table, 0x1000, 0x2001, iommu::intel_vtd::dma_access::read) ==
          iommu::intel_vtd::page_table_error::invalid_address);
    CHECK(iommu::intel_vtd::map_dma_page(table, 1ULL << 39, 0x2000, iommu::intel_vtd::dma_access::read) ==
          iommu::intel_vtd::page_table_error::invalid_address);
    CHECK(iommu::intel_vtd::map_dma_page(table, 0x1000, 0x2000, static_cast<iommu::intel_vtd::dma_access>(0)) ==
          iommu::intel_vtd::page_table_error::invalid_permissions);
    CHECK(store.allocation_count == 1);
    CHECK(iommu::intel_vtd::destroy_second_level_table(table) == iommu::intel_vtd::page_table_error::none);
}

TEST_CASE("VT-d rejects unsupported page-table widths and unsafe cache maintenance", "[intel_vtd][dma]")
{
    fake_page_store store{};
    const auto io = operations(store);
    iommu::intel_vtd::second_level_table table{};

    CHECK(iommu::intel_vtd::create_second_level_table(47, 48, true, io, table) ==
          iommu::intel_vtd::page_table_error::unsupported_address_width);
    auto noncoherent = io;
    noncoherent.flush_page = nullptr;
    CHECK(iommu::intel_vtd::create_second_level_table(48, 48, false, noncoherent, table) ==
          iommu::intel_vtd::page_table_error::cache_maintenance_unavailable);
    CHECK(store.allocation_count == 0);
}

TEST_CASE("VT-d page-table allocation and flush failures remain explicit", "[intel_vtd][dma]")
{
    fake_page_store store{};
    auto io = operations(store);
    iommu::intel_vtd::second_level_table table{};
    store.allocation_limit = 0;
    CHECK(iommu::intel_vtd::create_second_level_table(48, 48, true, io, table) ==
          iommu::intel_vtd::page_table_error::allocation_failed);

    store.allocation_limit = fake_page_count;
    store.fail_flush = true;
    CHECK(iommu::intel_vtd::create_second_level_table(48, 48, false, io, table) ==
          iommu::intel_vtd::page_table_error::cache_maintenance_unavailable);
    CHECK(store.release_count == 1);
}

TEST_CASE("VT-d page tables retain uncertain device-visible state", "[intel_vtd][dma]")
{
    fake_page_store store{};
    auto io = operations(store);
    iommu::intel_vtd::second_level_table table{};
    REQUIRE(iommu::intel_vtd::create_second_level_table(48, 48, false, io, table) ==
            iommu::intel_vtd::page_table_error::none);

    // Root initialization is flush 1. The first intermediate page is flush 2,
    // and publishing its parent entry is flush 3.
    store.fail_flush_on_call = 3;
    CHECK(iommu::intel_vtd::map_dma_page(table, 0x12345000, 0x2000, iommu::intel_vtd::dma_access::read_write) ==
          iommu::intel_vtd::page_table_error::cache_maintenance_unavailable);
    CHECK(table.state_uncertain);
    CHECK(iommu::intel_vtd::map_dma_page(table, 0x12345000, 0x2000, iommu::intel_vtd::dma_access::read_write) ==
          iommu::intel_vtd::page_table_error::table_state_uncertain);

    CHECK(iommu::intel_vtd::destroy_second_level_table(table) ==
          iommu::intel_vtd::page_table_error::table_state_uncertain);
    CHECK(store.release_count == 0);
    CHECK(store.allocated[0]);
    CHECK(store.allocated[1]);
}
