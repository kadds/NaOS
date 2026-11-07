#include "kernel/arch/acpi/boot_tables.hpp"
#include "kernel/iommu/backend.hpp"
#include "kernel_adapters.hpp"

namespace iommu
{
provider_initialization_report initialize_provider_backend(provider_kind provider,
                                                           const provider_bootstrap_config &configuration)
{
    switch (provider)
    {
        case provider_kind::intel_vtd:
            return intel_vtd::kernel_initialize_provider(configuration);
        case provider_kind::amd_vi:
            return amd_vi::kernel_initialize_provider(configuration);
        case provider_kind::none:
            break;
    }
    return {NA_STATUS_INVALID_ARGUMENT, false};
}

provider_bootstrap_report initialize_firmware_provider_backends(const provider_bootstrap_config &configuration)
{
    provider_bootstrap_report result{};
    if (configuration.firmware_inventory == nullptr)
    {
        result.providers[static_cast<u32>(provider_kind::intel_vtd)].status = NA_STATUS_INVALID_ARGUMENT;
        result.providers[static_cast<u32>(provider_kind::amd_vi)].status = NA_STATUS_INVALID_ARGUMENT;
        return result;
    }

    const auto &catalog = *static_cast<const arch::acpi_boot::snapshot *>(configuration.firmware_inventory);
    bool has_dmar = false;
    bool has_ivrs = false;
    const u32 dmar_signature = arch::acpi_boot::signature_value("DMAR");
    const u32 ivrs_signature = arch::acpi_boot::signature_value("IVRS");
    for (const auto &table : catalog.tables)
    {
        has_dmar = has_dmar || table.signature == dmar_signature;
        has_ivrs = has_ivrs || table.signature == ivrs_signature;
    }

    if (has_dmar)
        result.providers[static_cast<u32>(provider_kind::intel_vtd)] =
            initialize_provider_backend(provider_kind::intel_vtd, configuration);
    if (has_ivrs)
        result.providers[static_cast<u32>(provider_kind::amd_vi)] =
            initialize_provider_backend(provider_kind::amd_vi, configuration);
    return result;
}
} // namespace iommu
