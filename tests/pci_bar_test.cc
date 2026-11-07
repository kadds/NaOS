#include <catch2/catch_test_macros.hpp>

#include "kernel/pci/bar.hpp"
#include "kernel/pci/root_resource_authority.hpp"

namespace
{
constexpr u32 io_space_bit = 1u;
constexpr u32 memory_type_mask = 3u << 1;
constexpr u32 memory_type_64 = 2u << 1;
constexpr u32 prefetchable_bit = 1u << 3;
} // namespace

TEST_CASE("PCI BAR decoder identifies assigned 32-bit memory BARs", "[pci][bar]")
{
    pci::bar_descriptor bar{};

    CHECK(pci::decode_bar(6, 2, 0x12345000u | prefetchable_bit, false, 0, bar) == pci::bar_decode_error::none);
    CHECK(bar.base == 0x12345000);
    CHECK(bar.register_count == 1);
    CHECK(bar.prefetchable);
}

TEST_CASE("PCI BAR decoder identifies 64-bit BAR pairs without writing config", "[pci][bar]")
{
    pci::bar_descriptor bar{};

    CHECK(pci::decode_bar(6, 1, 0x23456000u | memory_type_64, true, 0x12, bar) == pci::bar_decode_error::none);
    CHECK(bar.base == 0x0000001223456000ull);
    CHECK(bar.register_count == 2);
    CHECK_FALSE(bar.prefetchable);
}

TEST_CASE("PCI BAR decoder distinguishes unsupported and reserved encodings", "[pci][bar]")
{
    pci::bar_descriptor bar{};

    CHECK(pci::decode_bar(6, 0, 0x1001, false, 0, bar) == pci::bar_decode_error::io_space_unsupported);
    CHECK(pci::decode_bar(6, 0, 0x1002, false, 0, bar) == pci::bar_decode_error::memory_below_1mb_unsupported);
    CHECK(pci::decode_bar(6, 0, 0x1006, true, 1, bar) == pci::bar_decode_error::reserved_memory_type);
}

TEST_CASE("PCI BAR decoder bounds register pairs and rejects unassigned BARs", "[pci][bar]")
{
    pci::bar_descriptor bar{};

    CHECK(pci::decode_bar(0, 0, 0x1000, false, 0, bar) == pci::bar_decode_error::invalid_bar_count);
    CHECK(pci::decode_bar(6, 6, 0x1000, false, 0, bar) == pci::bar_decode_error::invalid_index);
    CHECK(pci::decode_bar(6, 5, 0x1004, true, 1, bar) == pci::bar_decode_error::missing_upper_register);
    CHECK(pci::decode_bar(6, 0, 0x1004, false, 0, bar) == pci::bar_decode_error::missing_upper_register);
    CHECK(pci::decode_bar(6, 0, 0, false, 0, bar) == pci::bar_decode_error::unassigned);
}

TEST_CASE("PCI BAR page cover and root-window checks reject overflow and escape", "[pci][bar]")
{
    pci::bar_page_cover cover{};

    REQUIRE(pci::make_bar_page_cover(0x1003, 0x1000, cover));
    CHECK(cover.base == 0x1000);
    CHECK(cover.end == 0x3000);
    CHECK(pci::bar_range_within_window(0x1003, 0x1000, 0x1000, 0x4000));
    CHECK_FALSE(pci::bar_range_within_window(0x1003, 0x1000, 0x2000, 0x4000));
    CHECK_FALSE(pci::bar_range_within_window(0x1003, 0x1000, 0x1000, 0x1000));
    CHECK_FALSE(pci::make_bar_page_cover(~u64(0) - 2, 8, cover));
}

TEST_CASE("PCI BAR apertures require conventional power-of-two size and base alignment", "[pci][bar]")
{
    CHECK(pci::valid_bar_aperture(0x80000000, 0x1000));
    CHECK(pci::valid_bar_aperture(0x80000000, 0x200000));
    CHECK_FALSE(pci::valid_bar_aperture(0x80001000, 0x2000));
    CHECK_FALSE(pci::valid_bar_aperture(0x80000000, 0x1800));
    CHECK_FALSE(pci::valid_bar_aperture(0, 0));
}

TEST_CASE("PCI root memory windows normalize ACPI translations", "[pci][bar]")
{
    pci::memory_window window{};

    CHECK(pci::decode_root_memory_window(0, 1, 0, 0xffff, 0x1000, 0x1fff, 0x100, 0x1000, window) ==
          pci::root_window_error::none);
    CHECK(window.base == 0x1100);
    CHECK(window.length == 0x1000);

    CHECK(pci::decode_root_memory_window(0, 1, 0, 0xffff, 0x1100, 0x20ff, ~u64(0) - 0xff, 0x1000, window) ==
          pci::root_window_error::none);
    CHECK(window.base == 0x1000);
    CHECK(window.length == 0x1000);
}

TEST_CASE("PCI root memory windows reject malformed, unsupported, and overflowing descriptors", "[pci][bar]")
{
    pci::memory_window window{};
    const auto decode = [&window](u32 type, u32 revision, u32 flags, u64 granularity, u64 minimum, u64 maximum,
                                  u64 translation, u64 length) {
        return pci::decode_root_memory_window(type, revision, flags, granularity, minimum, maximum, translation, length,
                                              window);
    };

    CHECK(decode(1, 1, 0, 0xffff, 0x1000, 0x1fff, 0, 0x1000) == pci::root_window_error::unsupported_resource_type);
    CHECK(decode(0, 2, 0, 0xffff, 0x1000, 0x1fff, 0, 0x1000) == pci::root_window_error::invalid_descriptor);
    CHECK(decode(0, 1, 0x10, 0xffff, 0x1000, 0x1fff, 0, 0x1000) == pci::root_window_error::invalid_descriptor);
    CHECK(decode(0, 1, 0, 0x1000, 0x1000, 0x1fff, 0, 0x1000) == pci::root_window_error::invalid_descriptor);
    CHECK(decode(0, 1, 0, 0xffff, 0x1000, 0x1fff, 0, 0) == pci::root_window_error::invalid_range);
    CHECK(decode(0, 1, 0, 0xffff, 0x2000, 0x1fff, 0, 0x1000) == pci::root_window_error::invalid_range);
    CHECK(decode(0, 1, 0, 0xffff, 0x1000, 0x1fff, 0, 0x1001) == pci::root_window_error::invalid_range);
    CHECK(decode(0, 1, 0, 0xffff, ~u64(0) - 0x7ff, ~u64(0), 0, 0x1000) == pci::root_window_error::invalid_range);
    CHECK(decode(0, 1, 0, 0xffff, ~u64(0) - 0x0fff, ~u64(0), 0x1000, 0x1000) ==
          pci::root_window_error::invalid_translation);
    CHECK(decode(0, 1, 0, 0xffff, 0x0fff, 0x1ffe, ~u64(0) - 0x0fff, 0x1000) ==
          pci::root_window_error::invalid_translation);
}

TEST_CASE("PCI root resource attestation binds translated memory windows to the exact root lineage",
          "[pci][root-resource]")
{
    pci::root_resource_evidence evidence{};
    static_cast<pci::root_feature_scope &>(evidence) =
        pci::bind_root_feature_scope(7, 11, 13, 17, 19, 23, 29, 0, 0, 255);
    evidence.window_count = 2;
    evidence.windows[0] = {2, 1, 0, 0, 0, 0, 255, 0, 256, 0};
    evidence.windows[1] = {0, 1, 0, 0, 0xffff, 0x80000000, 0x80ffffff, 0, 0x01000000, 0};

    const auto expected = static_cast<const pci::root_feature_scope &>(evidence);
    pci::root_resource_attestation authority{};

    CHECK(pci::validate_root_resource_evidence(evidence, expected, authority) == pci::root_resource_error::none);
    CHECK(authority.root_id == expected.root_id);
    CHECK(authority.memory_window_count == 1);
    CHECK(authority.memory_windows[0].base == 0x80000000);
    CHECK(authority.memory_windows[0].length == 0x01000000);
}

TEST_CASE("PCI root resource attestation rejects stale, malformed, or incomplete root claims", "[pci][root-resource]")
{
    pci::root_resource_evidence evidence{};
    static_cast<pci::root_feature_scope &>(evidence) =
        pci::bind_root_feature_scope(7, 11, 13, 17, 19, 23, 29, 0, 0, 255);
    evidence.window_count = 2;
    evidence.windows[0] = {2, 1, 0, 0, 0, 0, 255, 0, 256, 0};
    evidence.windows[1] = {0, 1, 0, 0, 0xffff, 0x80000000, 0x80ffffff, 0, 0x01000000, 0};
    const auto expected = static_cast<const pci::root_feature_scope &>(evidence);
    pci::root_resource_attestation authority{};

    auto stale = evidence;
    ++stale.aml_namespace_generation;
    CHECK(pci::validate_root_resource_evidence(stale, expected, authority) == pci::root_resource_error::stale_scope);

    auto missing_bus = evidence;
    missing_bus.windows[0].resource_type = 1;
    CHECK(pci::validate_root_resource_evidence(missing_bus, expected, authority) ==
          pci::root_resource_error::missing_bus_window);

    auto wrong_bus = evidence;
    wrong_bus.windows[0].length = 255;
    CHECK(pci::validate_root_resource_evidence(wrong_bus, expected, authority) ==
          pci::root_resource_error::invalid_bus_window);

    auto unsupported = evidence;
    unsupported.windows[1].resource_type = 3;
    CHECK(pci::validate_root_resource_evidence(unsupported, expected, authority) ==
          pci::root_resource_error::invalid_descriptor);

    auto no_memory = evidence;
    no_memory.windows[1].resource_type = 1;
    CHECK(pci::validate_root_resource_evidence(no_memory, expected, authority) ==
          pci::root_resource_error::missing_memory_window);

    auto too_many = evidence;
    too_many.window_count = pci::max_root_resource_windows + 1;
    CHECK(pci::validate_root_resource_evidence(too_many, expected, authority) ==
          pci::root_resource_error::invalid_window_count);
}

TEST_CASE("PCI device resource evidence stays inside its root and retains the exact ADR path", "[pci][device-resource]")
{
    pci::root_resource_attestation root{};
    static_cast<pci::root_feature_scope &>(root) = pci::bind_root_feature_scope(7, 11, 13, 17, 19, 23, 29, 0, 0, 255);
    root.memory_window_count = 1;
    root.memory_windows[0] = {0x80000000, 0x01000000};

    pci::device_resource_evidence evidence{};
    static_cast<pci::root_feature_scope &>(evidence) = static_cast<const pci::root_feature_scope &>(root);
    evidence.device_id = 55;
    evidence.device_function_path = {0x08, 0x13};
    evidence.device_function_path[15] = 0xaa;
    evidence.path_length = 2;
    evidence.memory_window_count = 1;
    evidence.memory_windows[0] = {0x80f01000, 0x2000, 1};
    evidence.memory_windows[5] = {0x81000000, 0x1000};

    pci::device_resource_attestation authority{};
    CHECK(pci::validate_device_resource_evidence(evidence, root, authority) == pci::device_resource_error::none);
    CHECK(authority.root_id == root.root_id);
    CHECK(authority.device_id == evidence.device_id);
    CHECK(authority.path_length == evidence.path_length);
    CHECK(authority.device_function_path[0] == evidence.device_function_path[0]);
    CHECK(authority.device_function_path[1] == evidence.device_function_path[1]);
    CHECK(authority.device_function_path[15] == 0);
    CHECK(authority.memory_window_count == 1);
    CHECK(authority.memory_windows[0].base == evidence.memory_windows[0].base);
    CHECK(authority.memory_windows[0].length == evidence.memory_windows[0].length);
    CHECK(authority.memory_windows[0].flags == 1);
    CHECK(authority.memory_windows[5].length == 0);
}

TEST_CASE("PCI device resource evidence rejects stale lineage, malformed ranges, and root escapes",
          "[pci][device-resource]")
{
    pci::root_resource_attestation root{};
    static_cast<pci::root_feature_scope &>(root) = pci::bind_root_feature_scope(7, 11, 13, 17, 19, 23, 29, 0, 0, 255);
    root.memory_window_count = 1;
    root.memory_windows[0] = {0x80000000, 0x01000000};

    pci::device_resource_evidence evidence{};
    static_cast<pci::root_feature_scope &>(evidence) = static_cast<const pci::root_feature_scope &>(root);
    evidence.device_id = 55;
    evidence.device_function_path[0] = 0x08;
    evidence.path_length = 1;
    evidence.memory_window_count = 1;
    evidence.memory_windows[0] = {0x80f00000, 0x1000};
    pci::device_resource_attestation authority{};

    auto stale = evidence;
    ++stale.pci_fabric_generation;
    CHECK(pci::validate_device_resource_evidence(stale, root, authority) == pci::device_resource_error::stale_scope);

    auto no_device = evidence;
    no_device.device_id = 0;
    CHECK(pci::validate_device_resource_evidence(no_device, root, authority) ==
          pci::device_resource_error::invalid_device);

    auto no_path = evidence;
    no_path.path_length = 0;
    CHECK(pci::validate_device_resource_evidence(no_path, root, authority) == pci::device_resource_error::invalid_path);

    auto too_many = evidence;
    too_many.memory_window_count = pci::max_device_resource_windows + 1;
    CHECK(pci::validate_device_resource_evidence(too_many, root, authority) ==
          pci::device_resource_error::invalid_window_count);

    auto zero_length = evidence;
    zero_length.memory_windows[0].length = 0;
    CHECK(pci::validate_device_resource_evidence(zero_length, root, authority) ==
          pci::device_resource_error::invalid_range);

    auto overflowing = evidence;
    overflowing.memory_windows[0] = {~u64(0) - 0x7ff, 0x1000};
    CHECK(pci::validate_device_resource_evidence(overflowing, root, authority) ==
          pci::device_resource_error::invalid_range);

    auto outside = evidence;
    outside.memory_windows[0] = {0x80fff000, 0x2000};
    CHECK(pci::validate_device_resource_evidence(outside, root, authority) == pci::device_resource_error::outside_root);

    auto overlapping = evidence;
    overlapping.memory_window_count = 2;
    overlapping.memory_windows[1] = {0x80f00800, 0x1000};
    CHECK(pci::validate_device_resource_evidence(overlapping, root, authority) ==
          pci::device_resource_error::overlapping_windows);
}
