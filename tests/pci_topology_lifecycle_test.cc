#include <catch2/catch_test_macros.hpp>

#include "kernel/pci/topology_lifecycle.hpp"

TEST_CASE("PCI topology lifecycle binds one generation and revokes it monotonically", "[pci_topology_lifecycle]")
{
    pci::topology_lifecycle lifecycle;

    CHECK_FALSE(lifecycle.is_initialized());
    CHECK_FALSE(lifecycle.initialize(0, 2, 3));
    CHECK(lifecycle.initialize(1, 2, 3));
    CHECK_FALSE(lifecycle.initialize(1, 2, 3));

    CHECK_FALSE(lifecycle.bind_generation(0, 2, 5, 3, 4));
    CHECK(lifecycle.bind_platform_generation(1, 2, 5, 4));
    CHECK(lifecycle.bind_generation(1, 2, 5, 3, 4));
    CHECK(lifecycle.is_current(1, 2, 5, 3, 4));
    CHECK_FALSE(lifecycle.is_current(1, 2, 5, 3, 5));
    CHECK_FALSE(lifecycle.bind_generation(1, 2, 6, 3, 5));

    CHECK_FALSE(lifecycle.invalidate(9, 2));
    CHECK(lifecycle.invalidate(1, 2));
    CHECK(lifecycle.is_invalidated());
    CHECK_FALSE(lifecycle.is_current(1, 2, 5, 3, 4));
    CHECK_FALSE(lifecycle.bind_generation(1, 2, 5, 3, 4));
    CHECK_FALSE(lifecycle.invalidate(1, 2));
}

TEST_CASE("PCI topology failure before first root permanently blocks generation binding", "[pci_topology_lifecycle]")
{
    pci::topology_lifecycle lifecycle;
    REQUIRE(lifecycle.initialize(7, 8, 9));

    CHECK(lifecycle.invalidate(7, 8));
    CHECK_FALSE(lifecycle.bind_generation(7, 8, 9, 9, 10));
    CHECK_FALSE(lifecycle.is_current(7, 8, 9, 9, 10));
}

TEST_CASE("PCI topology lifecycle pins the SCI platform lineage once per boot", "[pci_topology_lifecycle]")
{
    pci::topology_lifecycle lifecycle;
    REQUIRE(lifecycle.initialize(21, 22, 25));

    CHECK_FALSE(lifecycle.bind_platform_generation(20, 22, 23, 24));
    CHECK_FALSE(lifecycle.bind_platform_generation(21, 22, 0, 24));
    REQUIRE(lifecycle.bind_platform_generation(21, 22, 23, 24));
    CHECK(lifecycle.bind_platform_generation(21, 22, 23, 24));
    CHECK_FALSE(lifecycle.bind_platform_generation(21, 22, 33, 24));
    CHECK_FALSE(lifecycle.bind_platform_generation(21, 22, 23, 34));

    pci::platform_generation_lineage lineage{};
    REQUIRE(lifecycle.get_platform_generation_lineage(lineage));
    CHECK(lineage.boot_epoch == 21);
    CHECK(lineage.firmware_generation == 22);
    CHECK(lineage.aml_namespace_generation == 23);
    CHECK(lineage.platform_topology_generation == 24);
    CHECK(lineage.pci_fabric_generation == 25);

    CHECK(lifecycle.bind_generation(21, 22, 23, 25, 24));
    CHECK_FALSE(lifecycle.bind_generation(21, 22, 33, 25, 24));
    CHECK(lifecycle.is_platform_generation_current(lineage));
    CHECK(lifecycle.invalidate(21, 22));
    CHECK_FALSE(lifecycle.is_platform_generation_current(lineage));
    // The immutable identity remains available to label terminal SCI events.
    CHECK(lifecycle.get_platform_generation_lineage(lineage));
}
