#include "catch2_compat.hpp"

#include "kernel/pci/driver_grant.hpp"

TEST_CASE("PCI driver function profile rejects rights outside its typed allowlist", "[pci][driver-grant]")
{
    const pci::driver_function_profile profile{pci::driver_profile_read_config, 0};

    CHECK(pci::validate_driver_function_profile(profile, false) == pci::driver_profile_error::none);
    CHECK(pci::validate_driver_function_profile({0, 0}, false) == pci::driver_profile_error::none);
    CHECK(pci::validate_driver_function_profile({1u << 31, 0}, false) == pci::driver_profile_error::unknown_rights);
}

TEST_CASE("PCI driver BAR profile requires attestation and stays inside the implemented BAR count",
          "[pci][driver-grant]")
{
    CHECK(pci::validate_driver_function_profile({0, 0x01}, false) ==
          pci::driver_profile_error::bars_require_attestation);
    CHECK(pci::validate_driver_function_profile({0, 0x3f}, true) == pci::driver_profile_error::none);
    CHECK(pci::validate_driver_function_profile({0, 0x40}, true) == pci::driver_profile_error::unsupported_bar);

    constexpr pci::driver_function_profile profile{0, 1u << 5};
    CHECK(pci::driver_profile_allows_bar(profile, 5));
    CHECK_FALSE(pci::driver_profile_allows_bar(profile, 4));
    CHECK_FALSE(pci::driver_profile_allows_bar(profile, 6));
}

TEST_CASE("PCI driver profile separates read, DMA, and interrupt authorities", "[pci][driver-grant]")
{
    constexpr auto profile =
        pci::driver_profile_read_config | pci::driver_profile_dma_domain | pci::driver_profile_route_interrupt;

    CHECK(pci::validate_driver_function_profile({profile, 0}, false) == pci::driver_profile_error::none);
    CHECK((profile & pci::driver_profile_read_config) != 0);
    CHECK((profile & pci::driver_profile_dma_domain) != 0);
    CHECK((profile & pci::driver_profile_route_interrupt) != 0);
    CHECK((profile & pci::driver_profile_known_rights) == profile);
}

TEST_CASE("PCI driver target must be a distinct live process that has not started", "[pci][driver-grant]")
{
    CHECK(pci::validate_driver_process_target(17, 23, false, false) == pci::driver_target_error::none);
    CHECK(pci::validate_driver_process_target(17, 17, false, false) == pci::driver_target_error::same_process);
    CHECK(pci::validate_driver_process_target(17, 23, true, false) == pci::driver_target_error::already_started);
    CHECK(pci::validate_driver_process_target(17, 23, false, true) == pci::driver_target_error::exited);
    CHECK(pci::validate_driver_process_target(17, 0, false, false) == pci::driver_target_error::invalid_process);
}
