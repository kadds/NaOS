#include <catch2/catch_test_macros.hpp>

#include "kernel/mm/data_plane_dma.hpp"

using naos::data_plane::dma_direction;
using naos::data_plane::dma_page_policy_error;
using naos::data_plane::dma_page_policy_input;

TEST_CASE("DMA page admission only exports owned complete MemoryObject pages", "[memory][dma]")
{
    constexpr u64 page_size = 4096;
    const dma_page_policy_input owned_page{page_size * 2, page_size, true, true, false, false, true, false};

    CHECK(naos::data_plane::validate_dma_page_policy(owned_page, page_size, dma_direction::device_reads) ==
          dma_page_policy_error::none);
    CHECK(naos::data_plane::validate_dma_page_policy(owned_page, page_size, dma_direction::device_writes) ==
          dma_page_policy_error::none);

    auto borrowed_page = owned_page;
    borrowed_page.owns_frames = false;
    CHECK(naos::data_plane::validate_dma_page_policy(borrowed_page, page_size, dma_direction::device_reads) ==
          dma_page_policy_error::borrowed_frames);

    auto external_view = owned_page;
    external_view.external_view = true;
    CHECK(naos::data_plane::validate_dma_page_policy(external_view, page_size, dma_direction::device_reads) ==
          dma_page_policy_error::external_storage);

    auto direct_physical_view = owned_page;
    direct_physical_view.direct_physical = true;
    CHECK(naos::data_plane::validate_dma_page_policy(direct_physical_view, page_size, dma_direction::device_reads) ==
          dma_page_policy_error::external_storage);
}

TEST_CASE("DMA page admission rejects partial, unaligned, and out-of-range spans", "[memory][dma]")
{
    constexpr u64 page_size = 4096;
    const dma_page_policy_input object{page_size * 2, 0, true, true, false, false, true, false};
    CHECK(naos::data_plane::validate_dma_page_policy(object, page_size, dma_direction::device_reads) ==
          dma_page_policy_error::none);

    auto partial = object;
    partial.size = page_size + 17;
    partial.offset = page_size;
    CHECK(naos::data_plane::validate_dma_page_policy(partial, page_size, dma_direction::device_reads) ==
          dma_page_policy_error::partial_page);

    auto unaligned = object;
    unaligned.size = page_size * 2;
    unaligned.offset = 1;
    CHECK(naos::data_plane::validate_dma_page_policy(unaligned, page_size, dma_direction::device_reads) ==
          dma_page_policy_error::invalid_range);

    auto overrun = object;
    overrun.offset = object.size;
    CHECK(naos::data_plane::validate_dma_page_policy(overrun, page_size, dma_direction::device_reads) ==
          dma_page_policy_error::invalid_range);

    auto empty = object;
    empty.size = 0;
    CHECK(naos::data_plane::validate_dma_page_policy(empty, page_size, dma_direction::device_reads) ==
          dma_page_policy_error::invalid_range);

    CHECK(naos::data_plane::validate_dma_page_policy(object, 0, dma_direction::device_reads) ==
          dma_page_policy_error::invalid_range);
}

TEST_CASE("DMA page admission rejects unreadied pager pages and device writes to read-only objects", "[memory][dma]")
{
    constexpr u64 page_size = 4096;
    const dma_page_policy_input pager_page{page_size, 0, true, true, false, false, false, false};
    CHECK(naos::data_plane::validate_dma_page_policy(pager_page, page_size, dma_direction::device_reads) ==
          dma_page_policy_error::page_not_ready);

    auto ready_page = pager_page;
    ready_page.page_ready = true;
    ready_page.read_only = true;
    CHECK(naos::data_plane::validate_dma_page_policy(ready_page, page_size, dma_direction::device_reads) ==
          dma_page_policy_error::none);
    CHECK(naos::data_plane::validate_dma_page_policy(ready_page, page_size, dma_direction::device_writes) ==
          dma_page_policy_error::read_only);

    auto unpaged = ready_page;
    unpaged.page_backed = false;
    CHECK(naos::data_plane::validate_dma_page_policy(unpaged, page_size, dma_direction::device_reads) ==
          dma_page_policy_error::not_page_backed);
}
