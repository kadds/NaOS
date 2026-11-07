#pragma once

#include "kernel/types.hpp"

namespace pci::dma_authority
{
enum class bus_master_denial : u8
{
    none,
    no_domain,
    domain_not_ready,
    no_iommu,
    state_uncertain,
};

struct bus_master_state
{
    bool domain_present = false;
    bool domain_ready = false;
    bool iommu_attached = false;
    bool trusted_driver_override = false;
    bool state_uncertain = false;
};

/// Decide whether a domain may enable PCI bus mastering. Disabling BME is
/// intentionally outside this policy and must remain possible on failure.
constexpr bus_master_denial bus_master_enable_denial(const bus_master_state &state) noexcept
{
    if (state.state_uncertain)
        return bus_master_denial::state_uncertain;
    if (!state.domain_present)
        return bus_master_denial::no_domain;
    if (!state.domain_ready)
        return bus_master_denial::domain_not_ready;
    if (!state.iommu_attached && !state.trusted_driver_override)
        return bus_master_denial::no_iommu;
    return bus_master_denial::none;
}
} // namespace pci::dma_authority
