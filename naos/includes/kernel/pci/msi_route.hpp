#pragma once

#include "kernel/iommu/backend.hpp"
#include "kernel/pci/msi_config.hpp"
#include "kernel/pci/msi_route_lifecycle.hpp"

namespace pci
{

/// Provider-selected interrupt route owned by the PCI interrupt subsystem.
/// The route token is opaque outside the selected IOMMU provider.
struct msi_route
{
    iommu::interrupt_route_token token{};
    msi_message message{};
    u32 destination_apic_id = 0;
    u8 vector = 0;
};

} // namespace pci
