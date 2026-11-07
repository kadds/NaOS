#include "kernel/arch/acpi/facs_reservation_boot_test_hook.hpp"

namespace arch::acpi_boot
{

bool apply_facs_reservation_boot_test_hook(const snapshot &, const physical_resource::root &) { return true; }

} // namespace arch::acpi_boot
