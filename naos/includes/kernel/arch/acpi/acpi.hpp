#pragma once
#include "common.hpp"
namespace arch::ACPI
{
void init();
phy_addr_t get_io_apic_base();
phy_addr_t get_local_apic_base();
} // namespace arch::ACPI
