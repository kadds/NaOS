#include <catch2/catch_test_macros.hpp>

#include "kernel/iommu/amd_vi_page_table.hpp"

#include <array>

namespace
{
constexpr u64 page_size = 4096;
constexpr u32 page_count = 16;

struct page_store
{
    alignas(page_size) std::array<std::array<u8, page_size>, page_count> pages{};
    std::array<bool, page_count> allocated{};
    u32 allocation_count = 0;
    u32 release_count = 0;
    u32 flush_count = 0;
    u32 allocation_limit = page_count;
    u32 fail_flush_on_call = 0;
};

bool allocate_page(void *context, void *&virtual_page, u64 &physical_address)
{
    auto &store = *static_cast<page_store *>(context);
    if (store.allocation_count >= store.allocation_limit)
        return false;
    for (u32 index = 0; index < store.pages.size(); ++index)
    {
        if (store.allocated[index])
            continue;
        store.allocated[index] = true;
        store.pages[index].fill(0xaa);
        store.allocation_count++;
        virtual_page = store.pages[index].data();
        physical_address = 0x01000000 + static_cast<u64>(index) * page_size;
        return true;
    }
    return false;
}

void release_page(void *context, void *virtual_page)
{
    auto &store = *static_cast<page_store *>(context);
    for (u32 index = 0; index < store.pages.size(); ++index)
    {
        if (store.allocated[index] && store.pages[index].data() == virtual_page)
        {
            store.allocated[index] = false;
            store.release_count++;
            return;
        }
    }
}

void *physical_to_virtual(void *context, u64 physical_address)
{
    auto &store = *static_cast<page_store *>(context);
    if ((physical_address & (page_size - 1)) != 0 || physical_address < 0x01000000)
        return nullptr;
    const u64 index = (physical_address - 0x01000000) / page_size;
    if (index >= store.pages.size() || !store.allocated[index])
        return nullptr;
    return store.pages[index].data();
}

bool flush_page(void *context, void *)
{
    auto &store = *static_cast<page_store *>(context);
    store.flush_count++;
    return store.flush_count != store.fail_flush_on_call;
}

iommu::amd_vi::page_table_ops operations(page_store &store)
{
    return {allocate_page, release_page, physical_to_virtual, flush_page, &store};
}

u64 *find_leaf(page_store &store, const iommu::amd_vi::page_table &table, u64 iova)
{
    auto *entries = static_cast<u64 *>(table.root_virtual);
    for (u32 level = iommu::amd_vi::default_page_table_levels; level > 1; --level)
    {
        const u32 shift = 12 + 9 * (level - 1);
        const u32 index = static_cast<u32>((iova >> shift) & 0x1ff);
        const u64 entry = entries[index];
        if ((entry & 1) == 0 || ((entry >> 9) & 7) != level - 1)
            return nullptr;
        entries = static_cast<u64 *>(physical_to_virtual(&store, entry & iommu::amd_vi::page_address_mask));
        if (entries == nullptr)
            return nullptr;
    }
    return &entries[(iova >> 12) & 0x1ff];
}
} // namespace

TEST_CASE("AMD Mode 4 table maps and unmaps one 4 KiB IOVA", "[amd_vi][iommu]")
{
    page_store store{};
    iommu::amd_vi::page_table table{};
    REQUIRE(iommu::amd_vi::create_page_table(48, true, operations(store), table) ==
            iommu::amd_vi::page_table_error::none);

    constexpr u64 iova = 0x123456789000ull;
    constexpr u64 physical = 0x000000024000ull;
    REQUIRE(iommu::amd_vi::map_dma_page(table, iova, physical, iommu::amd_vi::dma_access::read_write) ==
            iommu::amd_vi::page_table_error::none);
    auto *leaf = find_leaf(store, table, iova);
    REQUIRE(leaf != nullptr);
    CHECK(*leaf == (physical | 1ull | (1ull << 61) | (1ull << 62)));

    CHECK(iommu::amd_vi::map_dma_page(table, iova, physical + page_size, iommu::amd_vi::dma_access::read) ==
          iommu::amd_vi::page_table_error::already_mapped);
    REQUIRE(iommu::amd_vi::unmap_dma_page(table, iova) == iommu::amd_vi::page_table_error::none);
    CHECK(*leaf == 0);
    CHECK(iommu::amd_vi::unmap_dma_page(table, iova) == iommu::amd_vi::page_table_error::not_mapped);
    REQUIRE(iommu::amd_vi::destroy_page_table(table) == iommu::amd_vi::page_table_error::none);
    CHECK(store.release_count == store.allocation_count);
}

TEST_CASE("AMD Mode 4 table rejects out-of-range addresses and invalid permissions", "[amd_vi][iommu]")
{
    page_store store{};
    iommu::amd_vi::page_table table{};
    REQUIRE(iommu::amd_vi::create_page_table(48, true, operations(store), table) ==
            iommu::amd_vi::page_table_error::none);

    CHECK(iommu::amd_vi::map_dma_page(table, 0x1001, 0x2000, iommu::amd_vi::dma_access::read) ==
          iommu::amd_vi::page_table_error::invalid_address);
    CHECK(iommu::amd_vi::map_dma_page(table, 0x1000, 0x2001, iommu::amd_vi::dma_access::read) ==
          iommu::amd_vi::page_table_error::invalid_address);
    CHECK(iommu::amd_vi::map_dma_page(table, 1ull << 48, 0x2000, iommu::amd_vi::dma_access::read) ==
          iommu::amd_vi::page_table_error::invalid_address);
    CHECK(iommu::amd_vi::map_dma_page(table, 0x1000, 0x2000, static_cast<iommu::amd_vi::dma_access>(0)) ==
          iommu::amd_vi::page_table_error::invalid_permissions);
    CHECK(store.allocation_count == 1);
    REQUIRE(iommu::amd_vi::destroy_page_table(table) == iommu::amd_vi::page_table_error::none);
}

TEST_CASE("AMD table allocation failure is recoverable, but failed publication is quarantined", "[amd_vi][iommu]")
{
    page_store allocation_store{};
    iommu::amd_vi::page_table allocation_table{};
    REQUIRE(iommu::amd_vi::create_page_table(48, true, operations(allocation_store), allocation_table) ==
            iommu::amd_vi::page_table_error::none);
    allocation_store.allocation_limit = 2;
    CHECK(iommu::amd_vi::map_dma_page(allocation_table, 0x123456789000ull, 0x24000, iommu::amd_vi::dma_access::read) ==
          iommu::amd_vi::page_table_error::allocation_failed);
    CHECK(iommu::amd_vi::destroy_page_table(allocation_table) == iommu::amd_vi::page_table_error::none);
    CHECK(allocation_store.release_count == allocation_store.allocation_count);

    page_store flush_store{};
    auto io = operations(flush_store);
    iommu::amd_vi::page_table flush_table{};
    REQUIRE(iommu::amd_vi::create_page_table(48, false, io, flush_table) == iommu::amd_vi::page_table_error::none);
    flush_store.fail_flush_on_call = flush_store.flush_count + 2;
    CHECK(iommu::amd_vi::map_dma_page(flush_table, 0x123456789000ull, 0x24000, iommu::amd_vi::dma_access::read) ==
          iommu::amd_vi::page_table_error::cache_maintenance_unavailable);
    CHECK(iommu::amd_vi::destroy_page_table(flush_table) == iommu::amd_vi::page_table_error::table_state_uncertain);
}
