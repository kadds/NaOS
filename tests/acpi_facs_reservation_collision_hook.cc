#include "kernel/arch/acpi/facs_reservation.hpp"
#include "kernel/arch/acpi/facs_reservation_boot_test_hook.hpp"
#include "kernel/log.hpp"
#include "kernel/mm/memory.hpp"

KLOG_MODULE(acpi);

namespace arch::acpi_boot
{

bool apply_facs_reservation_boot_test_hook(const snapshot &firmware_snapshot,
                                           const physical_resource::root &resource_root)
{
    for (const auto &table : firmware_snapshot.tables)
    {
        if (table.signature != signature_value("FACS"))
            continue;

        facs_page_envelope envelope{};
        physical_resource::lease injected_conflict{};
        auto *ledger = resource_root.resources();
        if (ledger == nullptr ||
            !get_facs_page_envelope(table.physical_address, table.length, memory::page_size, envelope) ||
            !ledger->reserve(resource_root, table.physical_address, table.length, envelope.base, envelope.end,
                             physical_resource::kind::device_mmio, physical_resource::cache::uncacheable,
                             physical_resource::access::read, firmware_snapshot.firmware_generation, injected_conflict))
        {
            KLOG_ERROR("ACPI TEST: unable to inject FACS reservation collision");
            return false;
        }

        KLOG_INFO("ACPI TEST FACS reservation collision injected");
        return true;
    }

    KLOG_ERROR("ACPI TEST: no FACS range available for reservation collision injection");
    return false;
}

} // namespace arch::acpi_boot
