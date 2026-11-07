#pragma once

#include "kernel/arch/acpi/boot_tables.hpp"
#include "kernel/physical_resource.hpp"

namespace arch::acpi_boot
{

/// Test-build hook called immediately before the production FACS reservation
/// transaction. The normal kernel links a no-op implementation.
bool apply_facs_reservation_boot_test_hook(const snapshot &snapshot, const physical_resource::root &resource_root);

} // namespace arch::acpi_boot
