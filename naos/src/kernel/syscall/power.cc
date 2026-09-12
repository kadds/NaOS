#include "kernel/arch/acpi/acpi.hpp"
#include "kernel/syscall.hpp"
#include "naos/abi.h"
#include "naos/syscall.h"

namespace naos::syscall
{

// Power off the machine. NaOS has no boot-authority capability, so this is an
// unprivileged system-wide operation: it can only cause an availability loss,
// and the ACPI S5 path is the same one the power button already reaches.
na_status_t power_off()
{
    if (!arch::ACPI::has_init())
        return NA_STATUS_NOT_SUPPORTED;
    // arch::ACPI::shutdown() issues the platform power-off and does not return
    // once the firmware honours it; reaching the next line means the platform
    // refused the request.
    arch::ACPI::shutdown();
    return NA_STATUS_NOT_SUPPORTED;
}

BEGIN_SYSCALL
SYSCALL(NA_SYSCALL_POWER_OFF, power_off)
END_SYSCALL
} // namespace naos::syscall
