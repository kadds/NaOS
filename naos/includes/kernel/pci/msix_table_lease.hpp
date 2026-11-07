#pragma once

#include "kernel/pci/msi_capability.hpp"
#include "kernel/physical_resource.hpp"

namespace pci
{

enum class msix_table_lease_result : u8
{
    success,
    resource_unavailable,
    cleanup_failed,
};

/// Reserve the attested table bytes and their complete page cover for private
/// kernel MMIO access. This lease is intentionally not user-owned, so it must
/// not be bound through the user namespace/device lease APIs.
inline msix_table_lease_result reserve_and_map_kernel_msix_table(physical_resource::ledger &ledger,
                                                                 const physical_resource::root &authority,
                                                                 const msix_table_mapping_plan &plan,
                                                                 u64 firmware_generation,
                                                                 physical_resource::lease &result)
{
    result = {};
    if (authority.resources() != &ledger ||
        !ledger.reserve(authority, plan.table.base, plan.table.length, plan.page_cover.base, plan.page_cover.end,
                        physical_resource::kind::pci_bar, physical_resource::cache::uncacheable,
                        physical_resource::access::read_write, physical_resource::ledger::kernel_owner(),
                        physical_resource::sharing::exclusive, firmware_generation, result))
        return msix_table_lease_result::resource_unavailable;

    if (ledger.transition(result.id, physical_resource::state::reserved, physical_resource::state::leased) &&
        ledger.map(result.id, plan.table.base, plan.table.length, physical_resource::kind::pci_bar,
                   physical_resource::cache::uncacheable, physical_resource::access::read_write, firmware_generation,
                   physical_resource::ledger::kernel_owner()))
    {
        physical_resource::lease mapped{};
        if (ledger.lookup(result.id, mapped))
        {
            result = mapped;
            return msix_table_lease_result::success;
        }
    }

    if (!ledger.revoke_kernel(result.id))
        return msix_table_lease_result::cleanup_failed;
    result = {};
    return msix_table_lease_result::resource_unavailable;
}

} // namespace pci
