#pragma once

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

namespace naos_test::iommu_conformance
{
enum class dma_access : unsigned char
{
    read = 1,
    write = 2,
    read_write = 3,
};

/// Each fixture owns one real vendor controller wired to its fake register,
/// memory, and cache-maintenance callbacks. The shared assertions deliberately
/// exercise only the provider-neutral behavior expected from both backends.
template <typename Fixture> void run_backend_conformance()
{
    SECTION("default deny, DMA permissions, address width, and page lifecycle")
    {
        Fixture fixture(false, false);
        REQUIRE(fixture.initialized());
        REQUIRE(fixture.default_deny_ready());
        CHECK(fixture.unowned_requester_is_denied());

        using domain_type = typename Fixture::domain_type;
        domain_type domain{};
        const auto before_create = fixture.invalidation_count();
        REQUIRE(fixture.create_domain(domain));
        CHECK(fixture.invalidation_count() > before_create);

        const auto before_invalid = fixture.invalidation_count();
        CHECK_FALSE(fixture.map(domain, 0x1000, 0x02000000, static_cast<dma_access>(0)));
        CHECK(fixture.invalidation_count() == before_invalid);
        CHECK_FALSE(fixture.map(domain, 0x1000, std::uint64_t{1} << 52, dma_access::read_write));
        CHECK(fixture.invalidation_count() == before_invalid);

        REQUIRE(fixture.map(domain, 0x4000, 0x04000000, dma_access::read));
        CHECK(fixture.read_write_permissions(domain, 0x4000) == 1);
        const auto after_read_map = fixture.invalidation_count();
        CHECK(after_read_map > before_invalid);
        CHECK_FALSE(fixture.map(domain, 0x4000, 0x04000000, dma_access::write));
        CHECK(fixture.invalidation_count() == after_read_map);

        REQUIRE(fixture.map(domain, 0x8000, 0x05000000, dma_access::write));
        CHECK(fixture.read_write_permissions(domain, 0x8000) == 2);
        const auto after_write_map = fixture.invalidation_count();
        CHECK(after_write_map > after_read_map);

        REQUIRE(fixture.unmap(domain, 0x4000));
        CHECK(fixture.invalidation_count() > after_write_map);
        const auto after_first_unmap = fixture.invalidation_count();
        REQUIRE(fixture.unmap(domain, 0x8000));
        CHECK(fixture.invalidation_count() > after_first_unmap);

        const auto before_destroy = fixture.invalidation_count();
        REQUIRE(fixture.destroy_domain(domain));
        CHECK_FALSE(fixture.domain_valid(domain));
        CHECK(fixture.invalidation_count() > before_destroy);
    }

    SECTION("fault-log overflow is surfaced to the common fault owner")
    {
        Fixture fixture(false, true);
        REQUIRE(fixture.initialized());
        CHECK(fixture.drain_injected_fault_overflow());
        CHECK(fixture.fault_overflow_reported());
    }

    SECTION("interrupt remapping installs and retires a source route")
    {
        Fixture fixture(true, false);
        REQUIRE(fixture.initialized());
        REQUIRE(fixture.interrupt_remapping_ready());
        CHECK_FALSE(fixture.interrupt_route_active());

        const auto before_install = fixture.interrupt_invalidation_count();
        REQUIRE(fixture.install_interrupt_route());
        CHECK(fixture.interrupt_route_active());
        CHECK(fixture.interrupt_remapping_active_after_install());
        CHECK(fixture.interrupt_invalidation_count() > before_install);

        const auto before_retire = fixture.interrupt_invalidation_count();
        REQUIRE(fixture.retire_interrupt_route());
        CHECK_FALSE(fixture.interrupt_route_active());
        CHECK(fixture.interrupt_invalidation_count() > before_retire);
    }

    SECTION("unknown invalidation completion quarantines and retains DMA state")
    {
        Fixture fixture(false, false);
        REQUIRE(fixture.initialized());
        using domain_type = typename Fixture::domain_type;
        domain_type domain{};
        REQUIRE(fixture.create_domain(domain));
        REQUIRE(fixture.map(domain, 0x4000, 0x04000000, dma_access::write));

        const auto releases_before_unknown = fixture.released_page_count();
        fixture.make_next_invalidation_unknown();
        CHECK_FALSE(fixture.unmap(domain, 0x4000));
        CHECK(fixture.domain_quarantined(domain));
        CHECK(fixture.released_page_count() == releases_before_unknown);
        CHECK_FALSE(fixture.map(domain, 0x8000, 0x05000000, dma_access::read_write));
        CHECK_FALSE(fixture.destroy_domain(domain));
        CHECK(fixture.released_page_count() == releases_before_unknown);
    }
}
} // namespace naos_test::iommu_conformance
