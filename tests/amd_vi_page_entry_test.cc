#include <catch2/catch_test_macros.hpp>

#include "kernel/iommu/amd_vi_page_entry.hpp"

TEST_CASE("AMD 4 KiB leaf entries encode independent read and write permissions", "[amd_vi][iommu]")
{
    u64 entry = 0;
    constexpr u64 physical = 0x00000012345000ull;

    REQUIRE(iommu::amd_vi::encode_page_translation_entry(physical, iommu::amd_vi::dma_access::read, entry));
    CHECK(entry == (physical | 1ull | (1ull << 61)));

    REQUIRE(iommu::amd_vi::encode_page_translation_entry(physical, iommu::amd_vi::dma_access::write, entry));
    CHECK(entry == (physical | 1ull | (1ull << 62)));

    REQUIRE(iommu::amd_vi::encode_page_translation_entry(physical, iommu::amd_vi::dma_access::read_write, entry));
    CHECK(entry == (physical | 1ull | (1ull << 61) | (1ull << 62)));
}

TEST_CASE("AMD directory entries specify the child lookup level", "[amd_vi][iommu]")
{
    u64 entry = 0;
    REQUIRE(iommu::amd_vi::encode_page_directory_entry(0x2000, 3, entry));
    CHECK(entry == (0x2000ull | 1ull | (1ull << 61) | (1ull << 62) | (3ull << 9)));

    REQUIRE(iommu::amd_vi::encode_page_directory_entry(0x3000, 1, entry));
    CHECK((entry & (7ull << 9)) == (1ull << 9));
}

TEST_CASE("AMD page-entry encoders reject malformed addresses and leaf permissions", "[amd_vi][iommu]")
{
    u64 entry = 0xfeedface;
    CHECK_FALSE(iommu::amd_vi::encode_page_translation_entry(0x2001, iommu::amd_vi::dma_access::read, entry));
    CHECK_FALSE(iommu::amd_vi::encode_page_translation_entry(1ull << 52, iommu::amd_vi::dma_access::read, entry));
    CHECK_FALSE(iommu::amd_vi::encode_page_translation_entry(0x2000, static_cast<iommu::amd_vi::dma_access>(0), entry));
    CHECK(entry == 0xfeedface);

    CHECK_FALSE(iommu::amd_vi::encode_page_directory_entry(0x2001, 3, entry));
    CHECK_FALSE(iommu::amd_vi::encode_page_directory_entry(0x2000, 0, entry));
    CHECK_FALSE(iommu::amd_vi::encode_page_directory_entry(0x2000, 7, entry));
    CHECK(entry == 0xfeedface);
}
