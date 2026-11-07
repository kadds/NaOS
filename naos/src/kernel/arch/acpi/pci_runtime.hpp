#pragma once

#include "kernel/arch/acpi/boot_tables.hpp"
#include "kernel/pci/dma_quiesce.hpp"
#include "kernel/physical_resource.hpp"

template <typename T> class handle_t;

namespace arch::ACPI
{
namespace detail
{
extern acpi_boot::snapshot *firmware_snapshot;
extern physical_resource::ledger *hardware_ledger;
extern handle_t<physical_resource::root> *hardware_resource_root_object;

physical_resource::root *hardware_resource_root_authority();
} // namespace detail

namespace pci_runtime
{
using range_validator = bool (*)(void *, u64, u64);

void reset() noexcept;
void activate() noexcept;
bool prepare_firmware_inventory(acpi_boot::snapshot &snapshot, physical_resource::root &resource_root);
bool create_discovery_fabric(physical_resource::root &resource_root);
bool quiesce_dma(acpi_boot::snapshot &snapshot, u32 &functions_seen, u32 &bus_masters_disabled,
                 pci::dma_quiesce::error &failure);
bool initialize_providers(acpi_boot::snapshot &snapshot, bool pci_dma_quiesced, physical_resource::root &resource_root,
                          range_validator validate_device_range, range_validator validate_register_range);
void enable_fault_interrupts();
void start_deferred_services();
} // namespace pci_runtime
} // namespace arch::ACPI
