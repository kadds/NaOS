#include <catch2/catch_test_macros.hpp>

#include "kernel/pci/dma_authority.hpp"

TEST_CASE("PCI bus mastering needs a ready domain and explicit isolation policy", "[pci][dma]")
{
    using pci::dma_authority::bus_master_denial;
    using pci::dma_authority::bus_master_state;

    const bus_master_state iommu_domain{true, true, true, false, false};
    CHECK(pci::dma_authority::bus_master_enable_denial(iommu_domain) == bus_master_denial::none);

    auto no_domain = iommu_domain;
    no_domain.domain_present = false;
    CHECK(pci::dma_authority::bus_master_enable_denial(no_domain) == bus_master_denial::no_domain);

    auto not_ready = iommu_domain;
    not_ready.domain_ready = false;
    CHECK(pci::dma_authority::bus_master_enable_denial(not_ready) == bus_master_denial::domain_not_ready);

    auto uncertain = iommu_domain;
    uncertain.state_uncertain = true;
    CHECK(pci::dma_authority::bus_master_enable_denial(uncertain) == bus_master_denial::state_uncertain);
}

TEST_CASE("PCI bus mastering without an IOMMU requires an explicit trusted-driver override", "[pci][dma]")
{
    using pci::dma_authority::bus_master_denial;
    using pci::dma_authority::bus_master_state;

    const bus_master_state untrusted{true, true, false, false, false};
    CHECK(pci::dma_authority::bus_master_enable_denial(untrusted) == bus_master_denial::no_iommu);

    const bus_master_state trusted{true, true, false, true, false};
    CHECK(pci::dma_authority::bus_master_enable_denial(trusted) == bus_master_denial::none);

    auto unready_trusted = trusted;
    unready_trusted.domain_ready = false;
    CHECK(pci::dma_authority::bus_master_enable_denial(unready_trusted) == bus_master_denial::domain_not_ready);
}
