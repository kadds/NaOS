#include <catch2/catch_test_macros.hpp>

#include "kernel/iommu/amd_vi_dte.hpp"

static_assert(iommu::amd_vi::maximum_device_id_count * iommu::amd_vi::device_table_entry_bytes == 2 * 1024 * 1024);

TEST_CASE("AMD default DTE denies DMA and fixed or arbitrated interrupts", "[amd_vi][iommu]")
{
    iommu::amd_vi::device_table_entry entry{};

    REQUIRE(iommu::amd_vi::encode_default_deny_device_table_entry(0x1234, entry));
    CHECK(entry.words[0] == 0x3);               // V=1, TV=1, Mode=000b, IR=0, IW=0.
    CHECK(entry.words[1] == 0x1234ull); // DTE bits 79:64 contain the allocated DomainID.
    CHECK(entry.words[2] == 0x1);               // IV=1, IntCtl=00b.
    CHECK(entry.words[3] == 0);
    CHECK(iommu::amd_vi::is_default_deny_device_table_entry(entry, 0x1234));
}

TEST_CASE("AMD default DTE encoder rejects reserved DomainID zero without changing output", "[amd_vi][iommu]")
{
    iommu::amd_vi::device_table_entry entry{{0x1111, 0x2222, 0x3333, 0x4444}};
    const auto previous = entry;

    CHECK_FALSE(iommu::amd_vi::encode_default_deny_device_table_entry(0, entry));
    CHECK(entry.words[0] == previous.words[0]);
    CHECK(entry.words[1] == previous.words[1]);
    CHECK(entry.words[2] == previous.words[2]);
    CHECK(entry.words[3] == previous.words[3]);
}

TEST_CASE("AMD translated DTE points at a four-level tree while blocking interrupts", "[amd_vi][iommu]")
{
    iommu::amd_vi::device_table_entry entry{};

    REQUIRE(iommu::amd_vi::encode_dma_domain_device_table_entry(0x1234, 0x1000, entry));
    CHECK(entry.words[0] == (0x1000ull | (4ull << 9) | (1ull << 0) | (1ull << 1) | (1ull << 61) | (1ull << 62)));
    CHECK(entry.words[1] == 0x1234ull);
    CHECK(entry.words[2] == 1); // IV=1, IntCtl=00b until a route is installed.
    CHECK(entry.words[3] == 0);
}

TEST_CASE("AMD translated DTE encoder rejects invalid root pointers", "[amd_vi][iommu]")
{
    iommu::amd_vi::device_table_entry entry{{1, 2, 3, 4}};
    const auto previous = entry;

    CHECK_FALSE(iommu::amd_vi::encode_dma_domain_device_table_entry(1, 0, entry));
    CHECK_FALSE(iommu::amd_vi::encode_dma_domain_device_table_entry(1, 0x1001, entry));
    CHECK_FALSE(iommu::amd_vi::encode_dma_domain_device_table_entry(1, 1ull << 52, entry));
    CHECK(entry.words[0] == previous.words[0]);
    CHECK(entry.words[1] == previous.words[1]);
    CHECK(entry.words[2] == previous.words[2]);
    CHECK(entry.words[3] == previous.words[3]);
}

TEST_CASE("AMD default DTE validation rejects either DMA permission bit", "[amd_vi][iommu]")
{
    iommu::amd_vi::device_table_entry entry{};
    REQUIRE(iommu::amd_vi::encode_default_deny_device_table_entry(1, entry));

    auto readable = entry;
    readable.words[0] |= 1ull << 61; // DTE.IR.
    CHECK_FALSE(iommu::amd_vi::is_default_deny_device_table_entry(readable, 1));

    auto writable = entry;
    writable.words[0] |= 1ull << 62; // DTE.IW.
    CHECK_FALSE(iommu::amd_vi::is_default_deny_device_table_entry(writable, 1));

    auto unexpected_page_table = entry;
    unexpected_page_table.words[1] |= 1ull << 16; // DTE[95:80] is reserved without guest translation.
    CHECK_FALSE(iommu::amd_vi::is_default_deny_device_table_entry(unexpected_page_table, 1));

    auto unexpected_interrupt_table = entry;
    unexpected_interrupt_table.words[2] |= 1ull << 8;
    CHECK_FALSE(iommu::amd_vi::is_default_deny_device_table_entry(unexpected_interrupt_table, 1));

    CHECK_FALSE(iommu::amd_vi::is_default_deny_device_table_entry(entry, 2));
}

TEST_CASE("AMD IOAPIC DTE preserves SysMgt and encodes DomainID in bits 79:64", "[amd_vi][iommu]")
{
    iommu::amd_vi::device_table_entry entry{};

    REQUIRE(iommu::amd_vi::encode_ioapic_default_deny_device_table_entry(0x1234, 0x30, entry));
    CHECK(entry.words[1] == (0x1234ull | (0x3ull << 40)));
    CHECK(entry.words[2] == (1ull | (1ull << 60)));
}
