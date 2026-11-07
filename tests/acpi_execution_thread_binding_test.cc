#include <catch2/catch_test_macros.hpp>

#include "kernel/acpi/execution_thread_binding.hpp"
#include "kernel/acpi/resource_authority.hpp"
#include "kernel/pci/topology_lifecycle.hpp"

TEST_CASE("ACPI resource authority rejects writable region modes in the read-only profile", "[acpi][permissions]")
{
    CHECK(acpi::resource_authority::is_read_only_mode(0));
    CHECK_FALSE(acpi::resource_authority::is_read_only_mode(1));
    CHECK_FALSE(acpi::resource_authority::is_read_only_mode(2));
    CHECK(acpi::resource_authority::authorization_mode_allowed(acpi::resource_authority::region_space::system_io, 0));
    CHECK(acpi::resource_authority::authorization_mode_allowed(acpi::resource_authority::region_space::system_io, 1));
    CHECK_FALSE(
        acpi::resource_authority::authorization_mode_allowed(acpi::resource_authority::region_space::system_io, 2));
    CHECK_FALSE(
        acpi::resource_authority::authorization_mode_allowed(acpi::resource_authority::region_space::system_memory, 1));
}

TEST_CASE("ACPI resource authority binds one process thread and rejects other threads", "[acpi][thread]")
{
    acpi::detail::executor_thread_binding binding{};

    REQUIRE(acpi::detail::bind_executor_thread(binding, 12, true, 4));
    CHECK(acpi::detail::executor_thread_matches(binding, 12, 4));
    CHECK(acpi::detail::executor_owner_matches(binding, 12));
    CHECK_FALSE(acpi::detail::executor_owner_matches(binding, 13));
    CHECK_FALSE(acpi::detail::executor_thread_matches(binding, 12, 5));
    CHECK_FALSE(acpi::detail::executor_thread_matches(binding, 13, 4));
    CHECK(acpi::detail::bind_executor_thread(binding, 12, true, 4));
    CHECK_FALSE(acpi::detail::bind_executor_thread(binding, 12, true, 5));
    CHECK_FALSE(acpi::detail::bind_executor_thread(binding, 13, true, 4));
    CHECK_FALSE(acpi::detail::bind_executor_thread(binding, 12, false, 5));
}

TEST_CASE("ACPI executor thread exit permanently revokes the boot binding", "[acpi][thread]")
{
    acpi::detail::executor_thread_binding binding{};
    REQUIRE(acpi::detail::bind_executor_thread(binding, 12, true, 4));

    acpi::detail::unbind_executor_thread(binding, 13, 4);
    CHECK(acpi::detail::executor_thread_matches(binding, 12, 4));
    acpi::detail::unbind_executor_thread(binding, 12, 5);
    CHECK(acpi::detail::executor_thread_matches(binding, 12, 4));

    acpi::detail::unbind_executor_thread(binding, 12, 4);
    CHECK_FALSE(acpi::detail::executor_thread_matches(binding, 12, 4));
    CHECK_FALSE(acpi::detail::bind_executor_thread(binding, 12, true, 5));
    CHECK_FALSE(acpi::detail::bind_executor_thread(binding, 13, true, 4));
}

TEST_CASE("ACPI executor process exit prevents same-boot AML rebinding", "[acpi][thread]")
{
    acpi::detail::executor_thread_binding binding{};
    REQUIRE(acpi::detail::bind_executor_thread(binding, 12, true, 4));

    acpi::detail::unbind_executor_owner(binding, 12);

    CHECK_FALSE(acpi::detail::executor_thread_matches(binding, 12, 4));
    CHECK_FALSE(acpi::detail::executor_owner_matches(binding, 12));
    CHECK_FALSE(acpi::detail::bind_executor_thread(binding, 12, true, 5));
    CHECK_FALSE(acpi::detail::bind_executor_thread(binding, 13, true, 6));

    // A controlled reboot constructs a new kernel executor with fresh
    // boot-scoped binding state; the current boot's revoked object is not reset.
    acpi::detail::executor_thread_binding next_boot{};
    CHECK(acpi::detail::bind_executor_thread(next_boot, 22, true, 9));
}

TEST_CASE("AML executor owner exit revokes PCI topology before SCI ownership", "[acpi][thread][pci]")
{
    constexpr u64 owner = 12;
    constexpr u64 boot_epoch = 21;
    constexpr u64 firmware_generation = 22;
    constexpr u64 aml_namespace_generation = 23;
    constexpr u64 pci_fabric_generation = 25;
    constexpr u64 platform_topology_generation = 24;

    pci::topology_lifecycle topology;
    REQUIRE(topology.initialize(boot_epoch, firmware_generation, pci_fabric_generation));
    REQUIRE(topology.bind_platform_generation(boot_epoch, firmware_generation, aml_namespace_generation,
                                              platform_topology_generation));
    REQUIRE(topology.bind_generation(boot_epoch, firmware_generation, aml_namespace_generation, pci_fabric_generation,
                                     platform_topology_generation));

    acpi::detail::executor_thread_binding binding{};
    REQUIRE(acpi::detail::bind_executor_thread(binding, owner, true, 4));
    bool topology_invalidated = false;

    const bool owner_revoked = acpi::detail::unbind_executor_owner(binding, owner);
    REQUIRE(owner_revoked);
    REQUIRE(acpi::detail::invalidate_topology_on_executor_owner_exit(
        owner_revoked, [&] { topology_invalidated = topology.invalidate(boot_epoch, firmware_generation); }));

    CHECK(topology_invalidated);
    CHECK_FALSE(topology.is_current(boot_epoch, firmware_generation, aml_namespace_generation, pci_fabric_generation,
                                    platform_topology_generation));
    CHECK_FALSE(acpi::detail::executor_owner_matches(binding, owner));
    CHECK_FALSE(acpi::detail::bind_executor_thread(binding, owner, true, 5));
}

TEST_CASE("ACPI executor thread binding rejects zero identities", "[acpi][thread]")
{
    acpi::detail::executor_thread_binding binding{};
    CHECK_FALSE(acpi::detail::bind_executor_thread(binding, 0, true, 1));
    CHECK_FALSE(acpi::detail::bind_executor_thread(binding, 1, true, 0));
    CHECK_FALSE(acpi::detail::bind_executor_thread(binding, 1, false, 1));
    CHECK_FALSE(acpi::detail::executor_thread_matches(binding, 0, 0));
}

TEST_CASE("ACPI platform generations are process-scoped and ordered", "[acpi][thread]")
{
    constexpr u64 owner = 12;
    constexpr u64 owner_prefix = owner << 32;

    CHECK(acpi::detail::platform_generations_match_owner(owner, owner_prefix | 23, owner_prefix | 24));
    CHECK_FALSE(acpi::detail::platform_generations_match_owner(0, owner_prefix | 23, owner_prefix | 24));
    CHECK_FALSE(
        acpi::detail::platform_generations_match_owner(owner, (owner_prefix + (1ULL << 32)) | 23, owner_prefix | 24));
    CHECK_FALSE(
        acpi::detail::platform_generations_match_owner(owner, owner_prefix | 23, (owner_prefix + (1ULL << 32)) | 24));
    CHECK_FALSE(acpi::detail::platform_generations_match_owner(owner, owner_prefix | 23, owner_prefix | 22));
    CHECK_FALSE(acpi::detail::platform_generations_match_owner(owner, owner_prefix, owner_prefix | 24));
}
