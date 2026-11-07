#include <catch2/catch_test_macros.hpp>

#include "kernel/pci/discovery_fabric.hpp"

namespace
{
TEST_CASE("ECAM requester lookup rejects missing and ambiguous MCFG regions", "[pci_discovery]")
{
    const pci::ecam_region regions[] = {
        {0xe0000000, 4, 0, 127, 11},
        {0xe8000000, 4, 128, 255, 12},
    };
    const pci::ecam_region *selected = nullptr;

    REQUIRE(pci::find_ecam_region_for_requester(regions, 2, 4, 200, selected));
    CHECK(selected == &regions[1]);
    CHECK_FALSE(pci::find_ecam_region_for_requester(regions, 2, 3, 200, selected));
    CHECK(selected == nullptr);
    CHECK(pci::find_ecam_region_for_requester(regions, 2, 4, 127, selected));
    CHECK(selected == &regions[0]);

    const pci::ecam_region overlapping[] = {
        {0xe0000000, 4, 0, 200, 13},
        {0xe8000000, 4, 128, 255, 14},
    };
    CHECK_FALSE(pci::find_ecam_region_for_requester(overlapping, 2, 4, 150, selected));
    CHECK(selected == nullptr);
}

struct bridge_fixture
{
    u64 bridge_page = 0;
    u32 bridge_id = 0x1234'8086;
    u32 bridge_class = 0x0604'0001;
    u32 bridge_buses = 0x0007'0500;
};

bool read_bridge_fixture(void *context, u64 page, u32 offset, u32 &value)
{
    const auto &fixture = *static_cast<const bridge_fixture *>(context);
    if (page != fixture.bridge_page)
        return false;
    switch (offset)
    {
        case 0:
            value = fixture.bridge_id;
            return true;
        case 8:
            value = fixture.bridge_class;
            return true;
        case 0x18:
            value = fixture.bridge_buses;
            return true;
        default:
            return false;
    }
}

pci::root_resource_attestation attestation_for(const pci::root_binding_request &request)
{
    pci::root_resource_attestation attestation{};
    static_cast<pci::root_feature_scope &>(attestation) = pci::bind_root_feature_scope(
        request.platform_root_id, request.boot_epoch, request.firmware_generation, request.aml_namespace_generation,
        request.platform_topology_generation, request.pci_fabric_generation, 29, request.segment, request.start_bus,
        request.end_bus);
    attestation.memory_window_count = 1;
    attestation.memory_windows[0] = {0x80000000, 0x01000000};
    return attestation;
}
} // namespace

TEST_CASE("ECAM scope narrows a checked MCFG bus window", "[pci_discovery]")
{
    const pci::ecam_region region{0xe0000000, 0, 0, 255, 7};
    pci::ecam_scope scope{};

    REQUIRE(pci::scope_ecam_region(region, 32, 63, scope));
    CHECK(scope.base == 0xe2000000);
    CHECK(scope.length == (32ULL << 20));
    CHECK(scope.start_bus == 32);
    CHECK(scope.end_bus == 63);
}

TEST_CASE("ECAM scope rejects windows outside MCFG and arithmetic overflow", "[pci_discovery]")
{
    const pci::ecam_region region{0xe0000000, 0, 16, 31, 9};
    pci::ecam_scope scope{};
    scope.base = 0xfeed;

    REQUIRE_FALSE(pci::scope_ecam_region(region, 15, 20, scope));
    CHECK(scope.base == 0xfeed);
    REQUIRE_FALSE(pci::scope_ecam_region(region, 20, 32, scope));
    REQUIRE_FALSE(pci::scope_ecam_region(region, 24, 23, scope));

    const pci::ecam_region overflowing{~u64(0) - 0x80000, 0, 0, 255, 10};
    REQUIRE_FALSE(pci::scope_ecam_region(overflowing, 1, 1, scope));
}

TEST_CASE("platform root binding is pinned to one current MCFG allocation", "[pci_discovery]")
{
    const pci::ecam_region regions[] = {{}, {}, {0xe0000000, 4, 0, 255, 14}};
    const pci::firmware_inventory inventory{11, 12, 13, regions, 3};
    const auto &region = regions[2];
    const pci::root_binding_request request{2, 0xabc, 11, 12, 15, 16, 13, 4, 32, 63};
    const auto attestation = attestation_for(request);
    pci::root_binding binding{};

    REQUIRE(pci::make_root_binding(inventory, region, request, attestation, binding));
    CHECK(binding.region_index == 2);
    CHECK(binding.platform_root_id == 0xabc);
    CHECK(binding.segment == 4);
    CHECK(binding.ecam.base == 0xe2000000);
    CHECK(binding.ecam.length == (32ULL << 20));
    CHECK(binding.ecam.start_bus == 32);
    CHECK(binding.ecam.end_bus == 63);
    CHECK(binding.aml_namespace_generation == 15);
    CHECK(binding.platform_topology_generation == 16);
    REQUIRE(binding.resources.memory_window_count == 1);
    CHECK(binding.resources.memory_windows[0].base == 0x80000000);
}

TEST_CASE("platform root binding rejects stale or widened claims", "[pci_discovery]")
{
    const pci::ecam_region regions[] = {{}, {}, {0xe0000000, 4, 16, 31, 14}};
    const pci::firmware_inventory inventory{11, 12, 13, regions, 3};
    const auto &region = regions[2];
    const pci::root_binding_request valid{2, 0xabc, 11, 12, 15, 16, 13, 4, 20, 24};
    const auto attestation = attestation_for(valid);
    pci::root_binding binding{};
    binding.platform_root_id = 0xfeed;

    auto invalid = valid;
    invalid.platform_root_id = 0;
    CHECK_FALSE(pci::make_root_binding(inventory, region, invalid, attestation, binding));
    CHECK(binding.platform_root_id == 0xfeed);
    invalid = valid;
    invalid.boot_epoch++;
    CHECK_FALSE(pci::make_root_binding(inventory, region, invalid, attestation, binding));
    invalid = valid;
    invalid.firmware_generation++;
    CHECK_FALSE(pci::make_root_binding(inventory, region, invalid, attestation, binding));
    invalid = valid;
    invalid.pci_fabric_generation++;
    CHECK_FALSE(pci::make_root_binding(inventory, region, invalid, attestation, binding));
    invalid = valid;
    invalid.segment++;
    CHECK_FALSE(pci::make_root_binding(inventory, region, invalid, attestation, binding));
    invalid = valid;
    invalid.start_bus = 15;
    CHECK_FALSE(pci::make_root_binding(inventory, region, invalid, attestation, binding));
    invalid = valid;
    invalid.end_bus = 32;
    CHECK_FALSE(pci::make_root_binding(inventory, region, invalid, attestation, binding));
    invalid = valid;
    invalid.aml_namespace_generation = 0;
    CHECK_FALSE(pci::make_root_binding(inventory, region, invalid, attestation, binding));
    invalid = valid;
    invalid.platform_topology_generation = 0;
    CHECK_FALSE(pci::make_root_binding(inventory, region, invalid, attestation, binding));
    const pci::ecam_region no_lease{0xe0000000, 4, 16, 31, 0};
    CHECK_FALSE(pci::make_root_binding(inventory, no_lease, valid, attestation, binding));

    auto mismatched_attestation = attestation;
    mismatched_attestation.root_id++;
    CHECK_FALSE(pci::make_root_binding(inventory, region, valid, mismatched_attestation, binding));
    mismatched_attestation = attestation;
    mismatched_attestation.memory_window_count = 0;
    CHECK_FALSE(pci::make_root_binding(inventory, region, valid, mismatched_attestation, binding));
}

TEST_CASE("PCI function selectors must stay inside the bound root", "[pci_discovery]")
{
    using pci::function_selector_error;
    CHECK(pci::validate_function_selector(32, 63, 32, 0, 0) == function_selector_error::none);
    CHECK(pci::validate_function_selector(0, 255, 255, 31, 7) == function_selector_error::none);
    CHECK(pci::validate_function_selector(32, 63, 31, 0, 0) == function_selector_error::outside_root);
    CHECK(pci::validate_function_selector(32, 63, 64, 0, 0) == function_selector_error::outside_root);
    CHECK(pci::validate_function_selector(32, 31, 32, 0, 0) == function_selector_error::invalid_argument);
    CHECK(pci::validate_function_selector(0, 256, 0, 0, 0) == function_selector_error::invalid_argument);
    CHECK(pci::validate_function_selector(0, 255, 0, 32, 0) == function_selector_error::invalid_argument);
    CHECK(pci::validate_function_selector(0, 255, 0, 0, 8) == function_selector_error::invalid_argument);
    CHECK(pci::validate_function_selector(0, 255, 256, 0, 0) == function_selector_error::out_of_range);
}

TEST_CASE("PCI config selector resolves exactly one MCFG-backed function page", "[pci_discovery]")
{
    const pci::ecam_region region{0xe0000000, 4, 0, 255, 14};
    pci::ecam_scope function_page{};

    REQUIRE(pci::scope_ecam_function_page(region, 32, 63, 40, 2, 1, function_page));
    CHECK(function_page.base == 0xe2811000);
    CHECK(function_page.length == 4096);
    CHECK(function_page.start_bus == 40);
    CHECK(function_page.end_bus == 40);

    CHECK_FALSE(pci::scope_ecam_function_page(region, 32, 63, 31, 2, 1, function_page));
    CHECK_FALSE(pci::scope_ecam_function_page(region, 32, 63, 40, 32, 1, function_page));
    CHECK_FALSE(pci::scope_ecam_function_page(region, 32, 63, 40, 2, 8, function_page));
    const pci::ecam_region unleased{0xe0000000, 4, 0, 255, 0};
    CHECK_FALSE(pci::scope_ecam_function_page(unleased, 32, 63, 40, 2, 1, function_page));
}

TEST_CASE("PCI config reads accept only naturally aligned scalar widths within one function", "[pci_discovery]")
{
    using pci::config_read_error;

    CHECK(pci::validate_config_read(0, 1) == config_read_error::none);
    CHECK(pci::validate_config_read(3, 1) == config_read_error::none);
    CHECK(pci::validate_config_read(2, 2) == config_read_error::none);
    CHECK(pci::validate_config_read(0xffc, 4) == config_read_error::none);
    CHECK(pci::validate_config_read(0xfff, 1) == config_read_error::none);

    CHECK(pci::validate_config_read(0, 0) == config_read_error::invalid_width);
    CHECK(pci::validate_config_read(0, 3) == config_read_error::invalid_width);
    CHECK(pci::validate_config_read(1, 2) == config_read_error::misaligned);
    CHECK(pci::validate_config_read(2, 4) == config_read_error::misaligned);
    CHECK(pci::validate_config_read(0x1000, 1) == config_read_error::out_of_range);
    CHECK(pci::validate_config_read(0xfff, 2) == config_read_error::out_of_range);
}

TEST_CASE("PCI_Config OperationRegions are bounded to one read-only function page", "[pci_discovery]")
{
    CHECK(pci::valid_read_only_pci_config_region(0, 0, 4096));
    CHECK(pci::valid_read_only_pci_config_region(0, 0x60, 0x0c));
    CHECK(pci::valid_read_only_pci_config_region(0, 0xffc, 4));
    CHECK_FALSE(pci::valid_read_only_pci_config_region(1, 0x60, 0x0c));
    CHECK_FALSE(pci::valid_read_only_pci_config_region(0, 0x1000, 1));
    CHECK_FALSE(pci::valid_read_only_pci_config_region(0, 0xfff, 2));
    CHECK_FALSE(pci::valid_read_only_pci_config_region(0, 0, 0));
}

TEST_CASE("PCI config path resolves downstream buses through validated bridges", "[pci_discovery]")
{
    const pci::ecam_region region{0xe0000000, 0, 0, 255, 15};
    bridge_fixture fixture{};
    fixture.bridge_page = 0xe0008000;                      // bus 0, device 1, function 0
    const u8 path[] = {1, static_cast<u8>((1u << 5) | 2)}; // bridge 0:1.0, then target 5:2.1
    pci::ecam_scope function_page{};

    REQUIRE(pci::resolve_ecam_function_path(region, 0, 15, path, 2, read_bridge_fixture, &fixture, function_page));
    CHECK(function_page.base == 0xe0511000);
    CHECK(function_page.length == 4096);
    CHECK(function_page.start_bus == 5);
    CHECK(function_page.end_bus == 5);

    CHECK_FALSE(pci::resolve_ecam_function_path(region, 0, 4, path, 2, read_bridge_fixture, &fixture, function_page));
    fixture.bridge_class = 0x0200'0001;
    CHECK_FALSE(pci::resolve_ecam_function_path(region, 0, 15, path, 2, read_bridge_fixture, &fixture, function_page));
}
