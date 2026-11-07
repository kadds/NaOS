#pragma once

#include "kernel/acpi/fixed_inventory.hpp"

namespace acpi
{

template <typename WriteRegister>
bool disable_acpi_event_sources(const fixed_inventory &inventory, WriteRegister write_register)
{
    const auto disable = [&write_register](const fixed_register &reg) {
        if (reg.address == 0 && reg.length == 0)
            return true;
        if (reg.address == 0 || reg.length == 0 || reg.length > sizeof(u64))
            return false;
        return write_register(reg, 0);
    };

    // Write all four enable registers even after one failure so the
    // remaining sources are still driven toward the fail-closed state.
    // PM1 status registers are write-one-to-clear and are never written here.
    const bool pm1_a_disabled = disable(inventory.pm1_event_enable);
    const bool pm1_b_disabled = disable(inventory.pm1_event_b_enable);
    const bool gpe0_disabled = disable(inventory.gpe0_enable);
    const bool gpe1_disabled = disable(inventory.gpe1_enable);
    return pm1_a_disabled && pm1_b_disabled && gpe0_disabled && gpe1_disabled;
}

} // namespace acpi
