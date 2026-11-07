#include <array>

#include <catch2/catch_test_macros.hpp>

#include "kernel/pci/msi_capability.hpp"

namespace
{
struct fake_config
{
    std::array<u32, 64> words{};
    bool fail_reads = false;
};

bool read_config(void *context, u16 offset, u32 &value)
{
    auto &config = *static_cast<fake_config *>(context);
    if (config.fail_reads || (offset & 3) != 0 || offset >= 0x100)
        return false;
    value = config.words[offset / 4];
    return true;
}

void set_capability(fake_config &config, u8 offset, u8 id, u8 next, u16 control = 0)
{
    config.words[offset / 4] = static_cast<u32>(id) | (static_cast<u32>(next) << 8) | (static_cast<u32>(control) << 16);
}
} // namespace

TEST_CASE("PCI MSI capability walker skips other caps and decodes MSI layout", "[pci][msi]")
{
    fake_config config{};
    config.words[0x04 / 4] = 1u << 20;
    config.words[0x34 / 4] = 0x40;
    set_capability(config, 0x40, 0x01, 0x50);
    constexpr u16 msi_64bit_and_maskable = (1u << 1) | (1u << 7) | (1u << 8);
    set_capability(config, 0x50, 0x05, 0, msi_64bit_and_maskable);

    pci::msi_capability capability{};
    pci::msi_capability_error failure = pci::msi_capability_error::none;
    REQUIRE(pci::find_msi_capability(read_config, &config, capability, failure));
    CHECK(failure == pci::msi_capability_error::none);
    CHECK(capability.offset == 0x50);
    CHECK(capability.maximum_messages == 2);
    CHECK(capability.address_64_bit);
    CHECK(capability.per_vector_masking);
}

TEST_CASE("PCI MSI capability walker rejects absent and malformed capability lists", "[pci][msi]")
{
    fake_config config{};
    pci::msi_capability capability{};
    pci::msi_capability_error failure = pci::msi_capability_error::none;

    CHECK_FALSE(pci::find_msi_capability(read_config, &config, capability, failure));
    CHECK(failure == pci::msi_capability_error::capability_list_unavailable);

    config.words[0x04 / 4] = 1u << 20;
    config.words[0x34 / 4] = 0x42;
    CHECK_FALSE(pci::find_msi_capability(read_config, &config, capability, failure));
    CHECK(failure == pci::msi_capability_error::invalid_capability_pointer);

    config.words[0x34 / 4] = 0x40;
    set_capability(config, 0x40, 0x01, 0x40);
    CHECK_FALSE(pci::find_msi_capability(read_config, &config, capability, failure));
    CHECK(failure == pci::msi_capability_error::invalid_capability_pointer);
}

TEST_CASE("PCI MSI capability walker rejects duplicate and reserved message counts", "[pci][msi]")
{
    fake_config config{};
    config.words[0x04 / 4] = 1u << 20;
    config.words[0x34 / 4] = 0x40;
    set_capability(config, 0x40, 0x05, 0x50);
    set_capability(config, 0x50, 0x05, 0);

    pci::msi_capability capability{};
    pci::msi_capability_error failure = pci::msi_capability_error::none;
    CHECK_FALSE(pci::find_msi_capability(read_config, &config, capability, failure));
    CHECK(failure == pci::msi_capability_error::duplicate_msi_capability);

    set_capability(config, 0x40, 0x05, 0, static_cast<u16>(6u << 1));
    CHECK_FALSE(pci::find_msi_capability(read_config, &config, capability, failure));
    CHECK(failure == pci::msi_capability_error::unsupported_message_count);
}

TEST_CASE("PCI MSI capability walker rejects truncated capabilities and failed config reads", "[pci][msi]")
{
    fake_config config{};
    config.words[0x04 / 4] = 1u << 20;
    config.words[0x34 / 4] = 0xf0;
    constexpr u16 msi_64bit_and_maskable = (1u << 7) | (1u << 8);
    set_capability(config, 0xf0, 0x05, 0, msi_64bit_and_maskable);

    pci::msi_capability capability{};
    pci::msi_capability_error failure = pci::msi_capability_error::none;
    CHECK_FALSE(pci::find_msi_capability(read_config, &config, capability, failure));
    CHECK(failure == pci::msi_capability_error::truncated_capability);

    config.fail_reads = true;
    CHECK_FALSE(pci::find_msi_capability(read_config, &config, capability, failure));
    CHECK(failure == pci::msi_capability_error::config_read_failed);
}

TEST_CASE("PCI MSI-X capability walker decodes vector count and table regions", "[pci][msix]")
{
    fake_config config{};
    config.words[0x04 / 4] = 1u << 20;
    config.words[0x34 / 4] = 0x50;
    constexpr u16 msix_control = 3u | (1u << 14);
    set_capability(config, 0x50, 0x11, 0, msix_control);
    config.words[0x54 / 4] = 0x00002002; // BAR 2, table offset 0x2000
    config.words[0x58 / 4] = 0x00001001; // BAR 1, PBA offset 0x1000

    pci::msix_capability capability{};
    pci::msix_capability_error failure = pci::msix_capability_error::none;
    REQUIRE(pci::find_msix_capability(read_config, &config, capability, failure));
    CHECK(failure == pci::msix_capability_error::none);
    CHECK(capability.offset == 0x50);
    CHECK(capability.table_size == 4);
    CHECK(capability.function_masked);
    CHECK_FALSE(capability.enabled);
    CHECK(capability.table_bar == 2);
    CHECK(capability.table_offset == 0x2000);
    CHECK(capability.pba_bar == 1);
    CHECK(capability.pba_offset == 0x1000);

    const std::array<pci::msix_bar_aperture, 6> bars{{
        {},
        {0xfee00000, 0x2000},
        {0xfed00000, 0x4000},
    }};
    pci::msix_region table{};
    pci::msix_region pba{};
    REQUIRE(pci::resolve_msix_regions(capability, bars, table, pba, failure));
    CHECK(table.base == 0xfed02000);
    CHECK(table.length == 4 * 16);
    CHECK(pba.base == 0xfee01000);
    CHECK(pba.length == 8);
}

TEST_CASE("PCI MSI-X capability rejects invalid BARs, truncated lists, and overlapping regions", "[pci][msix]")
{
    fake_config config{};
    config.words[0x04 / 4] = 1u << 20;
    config.words[0x34 / 4] = 0x50;
    set_capability(config, 0x50, 0x11, 0, 0);
    config.words[0x54 / 4] = 0x00000007; // Reserved BIR 7
    config.words[0x58 / 4] = 0x00001001;

    pci::msix_capability capability{};
    pci::msix_capability_error failure = pci::msix_capability_error::none;
    CHECK_FALSE(pci::find_msix_capability(read_config, &config, capability, failure));
    CHECK(failure == pci::msix_capability_error::invalid_bar_index);

    config.words[0x54 / 4] = 0x00000000;
    config.words[0x58 / 4] = 0x00000000;
    REQUIRE(pci::find_msix_capability(read_config, &config, capability, failure));
    const std::array<pci::msix_bar_aperture, 6> bars{{{0xfed00000, 0x4000}}};
    pci::msix_region table{};
    pci::msix_region pba{};
    CHECK_FALSE(pci::resolve_msix_regions(capability, bars, table, pba, failure));
    CHECK(failure == pci::msix_capability_error::table_pba_overlap);

    config.words[0x34 / 4] = 0xf8;
    set_capability(config, 0xf8, 0x11, 0, 0);
    CHECK_FALSE(pci::find_msix_capability(read_config, &config, capability, failure));
    CHECK(failure == pci::msix_capability_error::truncated_capability);
}

TEST_CASE("PCI MSI-X capability distinguishes a missing capability", "[pci][msix]")
{
    fake_config config{};
    config.words[0x04 / 4] = 1u << 20;
    config.words[0x34 / 4] = 0x40;
    set_capability(config, 0x40, 0x01, 0);

    pci::msix_capability capability{};
    pci::msix_capability_error failure = pci::msix_capability_error::none;
    CHECK_FALSE(pci::find_msix_capability(read_config, &config, capability, failure));
    CHECK(failure == pci::msix_capability_error::capability_not_found);
}

TEST_CASE("PCI MSI-X region resolution rejects wrapping BAR apertures", "[pci][msix]")
{
    const pci::msix_capability capability{
        .offset = 0x40,
        .table_size = 1,
        .table_bar = 0,
        .table_offset = 0,
        .pba_bar = 1,
        .pba_offset = 0,
    };
    const std::array<pci::msix_bar_aperture, 6> bars{{
        {~u64{0} - 0xfff, 0x2000},
        {0xfee00000, 0x1000},
    }};
    pci::msix_region table{};
    pci::msix_region pba{};
    pci::msix_capability_error failure = pci::msix_capability_error::none;
    CHECK_FALSE(pci::resolve_msix_regions(capability, bars, table, pba, failure));
    CHECK(failure == pci::msix_capability_error::invalid_bar_aperture);
}

TEST_CASE("PCI BAR mapping reserves the MSI-X table aperture and fails closed on malformed capabilities", "[pci][msix]")
{
    fake_config config{};
    config.words[0x04 / 4] = 1u << 20;
    config.words[0x34 / 4] = 0x50;
    set_capability(config, 0x50, 0x11, 0, 0);
    config.words[0x54 / 4] = 0x00002002; // BAR 2 owns the MSI-X table.
    config.words[0x58 / 4] = 0x00001001;

    CHECK(pci::validate_msix_bar_mapping(read_config, &config, 2) == pci::msix_bar_mapping_error::table_bar_reserved);
    CHECK(pci::validate_msix_bar_mapping(read_config, &config, 1) == pci::msix_bar_mapping_error::none);
    CHECK(pci::validate_msix_bar_mapping(read_config, &config, 6) == pci::msix_bar_mapping_error::invalid_bar_index);

    config.words[0x54 / 4] = 0x00000007; // Reserved BIR must not make any BAR mappable.
    CHECK(pci::validate_msix_bar_mapping(read_config, &config, 0) == pci::msix_bar_mapping_error::invalid_capability);
}

TEST_CASE("MSI-X kernel table mapping plan stays in writable attested BAR and root windows", "[pci][msix]")
{
    const pci::msix_region table{0xfed05ff8, 64};
    const pci::msix_bar_aperture bar{0xfed04000, 0x4000};
    const std::array<pci::memory_window, 1> roots{{{0xfed00000, 0x10000}}};
    pci::msix_table_mapping_plan plan{};

    CHECK(pci::plan_msix_table_mapping(table, bar, 1, roots.data(), roots.size(), plan) ==
          pci::msix_table_mapping_error::none);
    CHECK(plan.page_cover.base == 0xfed05000);
    CHECK(plan.page_count == 2);
    CHECK(plan.table_offset == 0xff8);
    CHECK(pci::plan_msix_table_mapping(table, bar, 0, roots.data(), roots.size(), plan) ==
          pci::msix_table_mapping_error::not_writable);

    const std::array<pci::memory_window, 1> narrow_root{{{0xfed04000, 0x1000}}};
    CHECK(pci::plan_msix_table_mapping(table, bar, 1, narrow_root.data(), narrow_root.size(), plan) ==
          pci::msix_table_mapping_error::root_escape);
}
