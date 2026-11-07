#include "kernel/iommu/intel_vtd.hpp"

#include "kernel/arch/cpu_info.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/handle.hpp"
#include "kernel/lock.hpp"
#include "kernel/log.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/ucontext.hpp"
#include "kernel_adapters.hpp"

KLOG_MODULE(acpi);

namespace iommu::intel_vtd
{
namespace
{
controller kernel_backend{};
lock::spinlock_t backend_lock;
const firmware_inventory *kernel_inventory = nullptr;
bridge_info_reader kernel_bridge_info_reader = nullptr;
void *kernel_bridge_info_context = nullptr;
ioapic_match_counter kernel_ioapic_match_counter = nullptr;
void *kernel_ioapic_context = nullptr;
std::atomic_bool fault_report_busy{false};
fault_record fault_record_scratch[max_fault_record_count]{};

struct dma_domain_backend_state
{
    pci::function_identity identity{};
    u32 unit_index = ~u32{0};
    u16 requester_id = 0;
    device_domain domain{};
    handle_t<pci::dma_fault_source> fault_source;
};

firmware_inventory *allocate_firmware_inventory()
{
    if (memory::KernelCommonAllocatorV == nullptr || memory::MemoryAllocatorV == nullptr)
        return nullptr;
    void *storage = memory::KernelCommonAllocatorV->allocate(sizeof(firmware_inventory), alignof(firmware_inventory));
    if (storage == nullptr)
        return nullptr;
    return new (storage) firmware_inventory(memory::MemoryAllocatorV);
}

void release_firmware_inventory(firmware_inventory *inventory)
{
    if (inventory != nullptr)
        memory::Delete<>(memory::KernelCommonAllocatorV, inventory);
}

const arch::acpi_boot::table_record *find_unique_dmar_table(const arch::acpi_boot::snapshot &catalog, bool &duplicate)
{
    duplicate = false;
    const arch::acpi_boot::table_record *match = nullptr;
    const u32 signature = arch::acpi_boot::signature_value("DMAR");
    for (const auto &table : catalog.tables)
    {
        if (table.signature != signature)
            continue;
        if (match != nullptr)
        {
            duplicate = true;
            return nullptr;
        }
        match = &table;
    }
    return match;
}

void release_prepared_register_apertures(firmware_inventory &snapshot, const provider_bootstrap_config &configuration)
{
    if (configuration.release_registers == nullptr)
        return;
    for (auto &unit : snapshot.dmar_units)
    {
        if (unit.resource_lease_id == 0 && unit.kernel_virtual_base == 0)
            continue;
        if (unit.resource_lease_id == 0 || unit.kernel_virtual_base == 0 ||
            !configuration.release_registers(configuration.register_aperture_context, unit.register_base,
                                             unit.register_length, snapshot.firmware_generation, unit.resource_lease_id,
                                             unit.kernel_virtual_base))
        {
            KLOG_ERROR("VT-d register aperture rollback failed base={:x} lease={} virtual={:x}", unit.register_base,
                       unit.resource_lease_id, unit.kernel_virtual_base);
            continue;
        }
        unit.resource_lease_id = 0;
        unit.kernel_virtual_base = 0;
    }
}

bool prepare_register_apertures(firmware_inventory &snapshot, const provider_bootstrap_config &configuration)
{
    if (configuration.acquire_registers == nullptr || configuration.release_registers == nullptr ||
        snapshot.dmar_units.empty())
        return false;
    for (const auto &unit : snapshot.dmar_units)
    {
        if (unit.resource_lease_id != 0 || unit.kernel_virtual_base != 0)
            return false;
    }
    for (auto &unit : snapshot.dmar_units)
    {
        u64 lease_id = 0;
        u64 kernel_virtual_base = 0;
        const bool acquired = configuration.acquire_registers(
            configuration.register_aperture_context, unit.register_base, unit.register_length,
            snapshot.firmware_generation, lease_id, kernel_virtual_base);
        if (!acquired || lease_id == 0 || kernel_virtual_base == 0)
        {
            if (lease_id != 0 || kernel_virtual_base != 0)
            {
                unit.resource_lease_id = lease_id;
                unit.kernel_virtual_base = kernel_virtual_base;
            }
            release_prepared_register_apertures(snapshot, configuration);
            return false;
        }
        unit.resource_lease_id = lease_id;
        unit.kernel_virtual_base = kernel_virtual_base;
    }
    return true;
}

na_status_t create_dma_backend(const device_source_ref &source, const pci::function_identity &identity,
                               process_id owner, handle_t<pci::dma_fault_source> &fault_source, void *&backend_state,
                               u8 &address_width, u32 &unit_index, bool &outcome_uncertain)
{
    backend_state = nullptr;
    address_width = 0;
    unit_index = ~u32{0};
    outcome_uncertain = false;
    const u16 requester_id = static_cast<u16>((identity.bus << 8) | (identity.device << 3) | identity.function);
    if (owner == 0 || identity.segment > 0xffff || identity.bus > 0xff || identity.device >= 32 ||
        identity.function >= 8 || !source.valid() || source.unit.provider != provider_kind::intel_vtd ||
        source.unit.generation != identity.firmware_generation || source.segment != identity.segment ||
        source.device_id != requester_id || source.unit.opaque[0] >= max_hardware_units || !fault_source ||
        fault_source->owner() != owner || !kernel_default_deny_ready())
        return NA_STATUS_INVALID_ARGUMENT;

    unit_index = static_cast<u32>(source.unit.opaque[0]);
    if (!kernel_fault_event_interrupt_enabled(unit_index))
        return NA_STATUS_NOT_SUPPORTED;
    void *storage =
        memory::KernelCommonAllocatorV->allocate(sizeof(dma_domain_backend_state), alignof(dma_domain_backend_state));
    if (storage == nullptr)
        return NA_STATUS_RESOURCE_EXHAUSTED;
    auto *state = new (storage) dma_domain_backend_state();
    state->identity = identity;
    state->unit_index = unit_index;
    state->requester_id = requester_id;
    state->fault_source = fault_source;

    error failure = error::none;
    if (!kernel_create_device_domain(state->unit_index, static_cast<u16>(identity.segment),
                                     static_cast<u8>(identity.bus), static_cast<u8>(identity.device),
                                     static_cast<u8>(identity.function), state->domain, failure))
    {
        if (state->domain.valid || state->domain.page_table.root_virtual != nullptr)
        {
            backend_state = state;
            outcome_uncertain = true;
            return NA_STATUS_IO_ERROR;
        }
        memory::Delete<>(memory::KernelCommonAllocatorV, state);
        return to_status(failure);
    }

    if (!register_dma_fault_source(provider_kind::intel_vtd, state->unit_index, identity, requester_id,
                                   state->fault_source))
    {
        bool destroy_uncertain = false;
        const na_status_t status = kernel_destroy_dma_domain(state->domain, destroy_uncertain);
        if (status != NA_STATUS_OK || destroy_uncertain)
        {
            backend_state = state;
            outcome_uncertain = true;
            return status == NA_STATUS_OK ? NA_STATUS_IO_ERROR : status;
        }
        memory::Delete<>(memory::KernelCommonAllocatorV, state);
        return NA_STATUS_RESOURCE_EXHAUSTED;
    }

    address_width = state->domain.page_table.address_width;
    backend_state = state;
    return NA_STATUS_OK;
}

na_status_t map_dma_backend(void *opaque_state, u64 iova, u64 physical_address,
                            naos::data_plane::dma_direction direction, bool &outcome_uncertain)
{
    auto *state = static_cast<dma_domain_backend_state *>(opaque_state);
    if (state == nullptr || (direction != naos::data_plane::dma_direction::device_reads &&
                             direction != naos::data_plane::dma_direction::device_writes &&
                             direction != naos::data_plane::dma_direction::device_reads_and_writes))
        return NA_STATUS_INVALID_ARGUMENT;
    const dma_access access = direction == naos::data_plane::dma_direction::device_reads ? dma_access::read
                              : direction == naos::data_plane::dma_direction::device_reads_and_writes
                                  ? dma_access::read_write
                                  : dma_access::write;
    return kernel_map_dma_page(state->domain, iova, physical_address, access, outcome_uncertain);
}

na_status_t unmap_dma_backend(void *opaque_state, u64 iova, bool &outcome_uncertain)
{
    auto *state = static_cast<dma_domain_backend_state *>(opaque_state);
    if (state == nullptr)
        return NA_STATUS_INVALID_HANDLE;
    return kernel_unmap_dma_page(state->domain, iova, outcome_uncertain);
}

na_status_t destroy_dma_backend(void *opaque_state, bool &outcome_uncertain)
{
    auto *state = static_cast<dma_domain_backend_state *>(opaque_state);
    if (state == nullptr)
        return NA_STATUS_INVALID_HANDLE;
    const na_status_t status = kernel_destroy_dma_domain(state->domain, outcome_uncertain);
    if (status != NA_STATUS_OK)
        return status;
    unregister_dma_fault_source(provider_kind::intel_vtd, state->unit_index, state->identity, state->requester_id);
    memory::Delete<>(memory::KernelCommonAllocatorV, state);
    return NA_STATUS_OK;
}

bool push_registered_fault(const void *, const dma_fault_source_registration &entry, const pci::dma_fault_record &fault)
{
    handle_t<pci::dma_fault_source> source(entry.source_control);
    return source->push_fault(fault);
}

void quiesce_registered_requester(const void *context, const dma_fault_source_registration &entry, bool unit_overflow)
{
    const auto &ops = *static_cast<const fault_quiesce_ops *>(context);
    handle_t<pci::dma_fault_source> source(entry.source_control);
    if (unit_overflow)
        source->note_unit_overflow();
    else
        source->close();

    bool outcome_uncertain = false;
    const na_status_t status = ops.disable_bus_master == nullptr
                                   ? NA_STATUS_NOT_SUPPORTED
                                   : ops.disable_bus_master(ops.context, entry.identity, outcome_uncertain);
    if (status != NA_STATUS_OK || outcome_uncertain)
        KLOG_ERROR("VT-d unable to stop requester after fault quarantine segment={} requester={:04x} status={} "
                   "uncertain={}",
                   entry.segment, entry.requester_id, status, outcome_uncertain);
}

bool find_kernel_register(const firmware_inventory *snapshot, u64 base, u32 offset, u32 width,
                          volatile byte *&register_address)
{
    if (snapshot == nullptr)
        return false;
    for (const auto &unit : snapshot->dmar_units)
    {
        if (unit.register_base != base || unit.kernel_virtual_base == 0 || offset > unit.register_length ||
            width > unit.register_length - offset || unit.kernel_virtual_base > ~u64{0} - offset)
            continue;
        register_address = reinterpret_cast<volatile byte *>(unit.kernel_virtual_base + offset);
        return true;
    }
    return false;
}

bool read_kernel_register32(void *context, u64 base, u32 offset, u32 &value)
{
    volatile byte *address = nullptr;
    if (!find_kernel_register(static_cast<const firmware_inventory *>(context), base, offset, sizeof(u32), address))
        return false;
    _mfence();
    value = *reinterpret_cast<volatile u32 *>(address);
    _mfence();
    return true;
}

bool write_kernel_register32(void *context, u64 base, u32 offset, u32 value)
{
    volatile byte *address = nullptr;
    if (!find_kernel_register(static_cast<const firmware_inventory *>(context), base, offset, sizeof(u32), address))
        return false;
    *reinterpret_cast<volatile u32 *>(address) = value;
    _mfence();
    return true;
}

bool read_kernel_register64(void *context, u64 base, u32 offset, u64 &value)
{
    volatile byte *address = nullptr;
    if (!find_kernel_register(static_cast<const firmware_inventory *>(context), base, offset, sizeof(u64), address))
        return false;
    _mfence();
    value = *reinterpret_cast<volatile u64 *>(address);
    _mfence();
    return true;
}

bool write_kernel_register64(void *context, u64 base, u32 offset, u64 value)
{
    volatile byte *address = nullptr;
    if (!find_kernel_register(static_cast<const firmware_inventory *>(context), base, offset, sizeof(u64), address))
        return false;
    *reinterpret_cast<volatile u64 *>(address) = value;
    _mfence();
    return true;
}

bool allocate_kernel_table_page(void *, void *&virtual_page, u64 &physical_address)
{
    virtual_page = memory::malloc_page();
    if (virtual_page == nullptr)
        return false;
    physical_address = reinterpret_cast<u64>(memory::va2pa(virtual_page)());
    return true;
}

void release_kernel_table_page(void *, void *virtual_page) { memory::free_page(virtual_page); }

void *kernel_physical_to_virtual(void *, u64 physical_address)
{
    if (physical_address == 0 || (physical_address & (memory::page_size - 1)) != 0)
        return nullptr;
    return memory::pa2va<void *>(phy_addr_t::from(physical_address));
}

bool flush_kernel_table_page(void *, void *virtual_page)
{
    constexpr u32 clflush_leaf_bit = 19;
    const auto cpuid = arch::cpu_info::read_cpuid(1);
    if ((cpuid.edx & (1u << clflush_leaf_bit)) == 0)
        return false;
    const u32 cache_line_bytes = ((cpuid.ebx >> 8) & 0xff) * 8;
    if (cache_line_bytes == 0 || cache_line_bytes > memory::page_size ||
        (cache_line_bytes & (cache_line_bytes - 1)) != 0)
        return false;

    auto *page = static_cast<byte *>(virtual_page);
    for (u64 offset = 0; offset < memory::page_size; offset += cache_line_bytes)
        __asm__ __volatile__("clflush (%0)" : : "r"(page + offset) : "memory");
    _mfence();
    return true;
}

resolution_result provider_resolve_pci(void *, const pci_function_location &function, u64 generation,
                                       device_source_ref &source)
{
    source = {};
    if (kernel_inventory == nullptr || kernel_bridge_info_reader == nullptr)
        return resolution_result::reject;
    return resolve_pci_source(*kernel_inventory, function, generation, kernel_bridge_info_reader,
                              kernel_bridge_info_context, source);
}

resolution_result provider_resolve_ioapic(void *, u8 enumeration_id, u64 generation, device_source_ref &source)
{
    source = {};
    if (kernel_inventory == nullptr || kernel_bridge_info_reader == nullptr || kernel_ioapic_match_counter == nullptr)
        return resolution_result::reject;
    const u32 match_count = kernel_ioapic_match_counter(kernel_ioapic_context, enumeration_id);
    return resolve_ioapic_source(*kernel_inventory, enumeration_id, match_count, generation, kernel_bridge_info_reader,
                                 kernel_bridge_info_context, source);
}

bool provider_install_pci_msi(void *, const device_source_ref &source, u32 destination_apic_id, u8 vector,
                              interrupt_route_token &token, msi_message &message, bool &outcome_uncertain)
{
    token = {};
    message = {};
    outcome_uncertain = false;
    if (kernel_inventory == nullptr)
        return false;
    error failure = error::none;
    if (kernel_install_pci_msi_route(source, kernel_inventory->firmware_generation, destination_apic_id, vector, token,
                                     message, outcome_uncertain, failure))
        return true;
    KLOG_WARN("VT-d PCI MSI route installation failed segment={} requester={:04x} unit={} error={} uncertain={}",
              source.segment, source.device_id, source.unit.opaque[0], static_cast<u32>(failure), outcome_uncertain);
    return false;
}

bool provider_install_ioapic(void *, const device_source_ref &source, u32 destination_apic_id, u8 vector,
                             bool low_polarity, bool level_triggered, ioapic_route_encoding &route)
{
    route = {};
    if (kernel_inventory == nullptr)
        return false;
    error failure = error::none;
    if (kernel_install_ioapic_route(source, kernel_inventory->firmware_generation, destination_apic_id, vector,
                                    low_polarity, level_triggered, route, failure))
        return true;
    KLOG_WARN("VT-d IOAPIC route installation failed segment={} requester={:04x} unit={} error={}", source.segment,
              source.device_id, source.unit.opaque[0], static_cast<u32>(failure));
    return false;
}

bool provider_route_matches_pci(void *, const interrupt_route_token &token, const device_source_ref &source)
{
    return kernel_route_matches_pci(token, source);
}

bool provider_retire_route(void *, const interrupt_route_token &token)
{
    if (kernel_inventory == nullptr)
        return false;
    error failure = error::none;
    if (kernel_retire_route(token, kernel_inventory->firmware_generation, failure))
        return true;
    KLOG_ERROR("VT-d interrupt route retirement failed error={}", static_cast<u32>(failure));
    return false;
}

bool provider_interrupt_remapping_ready(void *) { return kernel_interrupt_remapping_ready(); }

bool provider_enable_interrupt_remapping(void *, bool &outcome_uncertain)
{
    outcome_uncertain = false;
    u8 failure = 0;
    if (kernel_enable_interrupt_remapping(outcome_uncertain, failure))
        return true;
    KLOG_ERROR("Intel VT-d interrupt remapping enable failed error={} uncertain={}", failure, outcome_uncertain);
    return false;
}

bool provider_interrupt_remapping_enabled(void *) { return kernel_interrupt_remapping_enabled(); }
bool provider_interrupt_remapping_uncertain(void *) { return kernel_interrupt_remapping_uncertain(); }

na_status_t configure_fault_interrupt_adapter(u32 unit_index, u32 destination_apic_id, u8 vector,
                                              bool &outcome_uncertain)
{
    outcome_uncertain = false;
    error failure = error::none;
    if (kernel_configure_fault_event_interrupt(unit_index, destination_apic_id, vector, failure))
        return NA_STATUS_OK;
    outcome_uncertain = kernel_fault_event_interrupt_uncertain(unit_index);
    return to_status(failure);
}

bool kernel_unit_state_uncertain(u32 unit_index)
{
    const auto *unit = kernel_backend.unit(unit_index);
    return unit == nullptr || unit->state_uncertain || unit->fault_event_interrupt_state_uncertain ||
           unit->interrupt_remapping_state_uncertain;
}
} // namespace

register_ops kernel_register_ops(const firmware_inventory *snapshot) noexcept
{
    if (snapshot == nullptr)
        return {};
    return {read_kernel_register32,     write_kernel_register32,    read_kernel_register64,
            write_kernel_register64,    allocate_kernel_table_page, release_kernel_table_page,
            kernel_physical_to_virtual, flush_kernel_table_page,    const_cast<firmware_inventory *>(snapshot)};
}

provider_initialization_report kernel_initialize(const firmware_inventory &snapshot,
                                                 bridge_info_reader read_bridge_info, void *bridge_context,
                                                 ioapic_match_counter count_ioapic_matches, void *ioapic_context)
{
    provider_initialization_report report{};
    if (read_bridge_info == nullptr || count_ioapic_matches == nullptr)
    {
        report.status = NA_STATUS_INVALID_ARGUMENT;
        return report;
    }
    if (!register_provider_fault_adapter(provider_kind::intel_vtd, kernel_drain_fault_events, nullptr,
                                         configure_fault_interrupt_adapter))
    {
        report.status = NA_STATUS_INVALID_ARGUMENT;
        return report;
    }
    const provider_dma_domain_adapter_ops domain_ops{create_dma_backend, map_dma_backend, unmap_dma_backend,
                                                     destroy_dma_backend, kernel_unit_state_uncertain};
    if (!register_provider_dma_domain_adapter(provider_kind::intel_vtd, domain_ops))
    {
        report.status = NA_STATUS_INVALID_ARGUMENT;
        return report;
    }
    uctx::RawSpinLockUninterruptibleContext guard(backend_lock);
    error failure = error::none;
    const bool initialized = kernel_backend.initialize(snapshot, kernel_register_ops(&snapshot), failure);
    report.status = initialized ? NA_STATUS_OK : to_status(failure);
    report.selected_units = kernel_backend.unit_count();
    report.active_units = kernel_backend.active_unit_count();
    report.invalidation_ready_units = kernel_backend.queued_invalidation_unit_count();
    report.interrupt_remapping_units = kernel_backend.interrupt_remapping_table_ready_unit_count();
    report.state_uncertain =
        kernel_backend.interrupt_remapping_activation_uncertain() || report.status == NA_STATUS_IO_ERROR;
    for (u32 index = 0; index < report.selected_units; ++index)
    {
        const auto *unit = kernel_backend.unit(index);
        report.state_uncertain =
            report.state_uncertain ||
            (unit != nullptr && (unit->state_uncertain || unit->fault_event_interrupt_state_uncertain ||
                                 unit->interrupt_remapping_state_uncertain));
    }
    if (!initialized)
        return report;
    kernel_inventory = &snapshot;
    kernel_bridge_info_reader = read_bridge_info;
    kernel_bridge_info_context = bridge_context;
    kernel_ioapic_match_counter = count_ioapic_matches;
    kernel_ioapic_context = ioapic_context;
    return report;
}

provider_ops kernel_provider_ops(dma_domain_creator create_dma_domain)
{
    provider_ops operations{};
    operations.kind = provider_kind::intel_vtd;
    operations.resolve_pci = provider_resolve_pci;
    operations.resolve_ioapic = provider_resolve_ioapic;
    operations.install_pci_msi = provider_install_pci_msi;
    operations.install_ioapic = provider_install_ioapic;
    operations.route_matches_pci = provider_route_matches_pci;
    operations.retire_route = provider_retire_route;
    operations.interrupt_remapping_ready = provider_interrupt_remapping_ready;
    operations.enable_interrupt_remapping = provider_enable_interrupt_remapping;
    operations.interrupt_remapping_enabled = provider_interrupt_remapping_enabled;
    operations.interrupt_remapping_activation_uncertain = provider_interrupt_remapping_uncertain;
    operations.create_dma_domain = create_dma_domain;
    return operations;
}

provider_initialization_report kernel_initialize_provider(const provider_bootstrap_config &configuration)
{
    provider_initialization_report report{};
    if (configuration.firmware_inventory == nullptr || configuration.create_dma_domain == nullptr ||
        configuration.read_bridge_info == nullptr || configuration.count_madt_ioapic_matches == nullptr ||
        configuration.validate_device_range == nullptr || configuration.validate_register_range == nullptr)
    {
        report.status = NA_STATUS_INVALID_ARGUMENT;
        return report;
    }
    const auto &catalog = *static_cast<const arch::acpi_boot::snapshot *>(configuration.firmware_inventory);
    bool duplicate_table = false;
    const auto *table = find_unique_dmar_table(catalog, duplicate_table);
    if (duplicate_table || table == nullptr)
    {
        report.status = NA_STATUS_NOT_SUPPORTED;
        return report;
    }

    auto *inventory = allocate_firmware_inventory();
    if (inventory == nullptr)
    {
        report.status = NA_STATUS_RESOURCE_EXHAUSTED;
        return report;
    }
    inventory->firmware_generation = catalog.firmware_generation;
    const arch::acpi_boot::physical_source validation_source{nullptr, nullptr, configuration.validate_device_range,
                                                             configuration.validate_register_range,
                                                             configuration.range_validation_context};
    arch::acpi_boot::error parse_error = arch::acpi_boot::error::none;
    if (!parse_firmware_inventory(validation_source, *table, *inventory, parse_error))
    {
        KLOG_WARN("VT-d DMAR inventory rejected error={} offset={} address={:x} length={}",
                  static_cast<u32>(inventory->dmar_error), inventory->dmar_error_offset, inventory->dmar_error_address,
                  inventory->dmar_error_length);
        release_firmware_inventory(inventory);
        report.status = NA_STATUS_NOT_SUPPORTED;
        return report;
    }
    report.selected_units = static_cast<u32>(inventory->dmar_units.size());
    if (!configuration.pci_bus_mastering_quiesced || !inventory->dmar_reserved_regions.empty())
    {
        release_firmware_inventory(inventory);
        report.status = NA_STATUS_NOT_SUPPORTED;
        return report;
    }
    if (!prepare_register_apertures(*inventory, configuration))
    {
        const bool aperture_retained = [&inventory]() {
            for (const auto &unit : inventory->dmar_units)
            {
                if (unit.resource_lease_id != 0 || unit.kernel_virtual_base != 0)
                    return true;
            }
            return false;
        }();
        if (aperture_retained)
            kernel_inventory = inventory;
        else
            release_firmware_inventory(inventory);
        report.status = NA_STATUS_RESOURCE_EXHAUSTED;
        return report;
    }

    // The controller keeps register callbacks that point into this inventory;
    // retain it even if a later hardware operation reports uncertain state.
    kernel_inventory = inventory;
    report = kernel_initialize(*inventory, configuration.read_bridge_info, configuration.platform_context,
                               configuration.count_madt_ioapic_matches, configuration.platform_context);
    if (report.status != NA_STATUS_OK)
        return report;
    report.fault_delivery = fault_delivery_mode::interrupt;

    if (!kernel_provider_router().register_provider(kernel_provider_ops(configuration.create_dma_domain)))
        report.status = NA_STATUS_INVALID_ARGUMENT;
    if (report.status == NA_STATUS_OK)
    {
        KLOG_INFO("ACPI: VT-d default-deny translation active units={} QI-ready-units={} IR-table-ready-units={}",
                  report.active_units, report.invalidation_ready_units, report.interrupt_remapping_units);
    }
    else
    {
        KLOG_WARN("ACPI: VT-d translation unavailable status={} selected_units={} active_units={} state_uncertain={}",
                  report.status, report.selected_units, report.active_units, report.state_uncertain);
    }
    return report;
}

bool kernel_install_pci_msi_route(const device_source_ref &source, u64 firmware_generation, u32 destination_apic_id,
                                  u8 vector, interrupt_route_token &token, msi_message &message,
                                  bool &outcome_uncertain, error &failure)
{
    uctx::RawSpinLockUninterruptibleContext guard(backend_lock);
    return install_pci_msi_route(kernel_backend, source, firmware_generation, destination_apic_id, vector, token,
                                 message, outcome_uncertain, failure);
}

bool kernel_install_ioapic_route(const device_source_ref &source, u64 firmware_generation, u32 destination_apic_id,
                                 u8 vector, bool low_polarity, bool level_triggered, ioapic_route_encoding &route,
                                 error &failure)
{
    uctx::RawSpinLockUninterruptibleContext guard(backend_lock);
    return install_ioapic_route(kernel_backend, source, firmware_generation, destination_apic_id, vector, low_polarity,
                                level_triggered, route, failure);
}

bool kernel_route_matches_pci(const interrupt_route_token &token, const device_source_ref &source) noexcept
{
    uctx::RawSpinLockUninterruptibleContext guard(backend_lock);
    return route_matches_pci(token, source);
}

bool kernel_retire_route(const interrupt_route_token &token, u64 firmware_generation, error &failure)
{
    uctx::RawSpinLockUninterruptibleContext guard(backend_lock);
    return retire_route(kernel_backend, token, firmware_generation, failure);
}

bool kernel_interrupt_remapping_ready()
{
    uctx::RawSpinLockUninterruptibleContext guard(backend_lock);
    return kernel_backend.interrupt_remapping_ready();
}

bool kernel_enable_interrupt_remapping(bool &outcome_uncertain, u8 &error_code)
{
    uctx::RawSpinLockUninterruptibleContext guard(backend_lock);
    error failure = error::none;
    const bool enabled = kernel_backend.enable_interrupt_remapping(failure);
    outcome_uncertain = kernel_backend.interrupt_remapping_activation_uncertain();
    error_code = static_cast<u8>(failure);
    return enabled;
}

bool kernel_interrupt_remapping_enabled()
{
    uctx::RawSpinLockUninterruptibleContext guard(backend_lock);
    return kernel_backend.interrupt_remapping_enabled();
}

bool kernel_interrupt_remapping_uncertain()
{
    uctx::RawSpinLockUninterruptibleContext guard(backend_lock);
    return kernel_backend.interrupt_remapping_activation_uncertain();
}

na_status_t kernel_map_dma_page(device_domain &domain, u64 iova, u64 physical_address, dma_access access,
                                bool &outcome_uncertain)
{
    uctx::RawSpinLockUninterruptibleContext guard(backend_lock);
    return map_dma_page(kernel_backend, domain, iova, physical_address, access, outcome_uncertain);
}

na_status_t kernel_unmap_dma_page(device_domain &domain, u64 iova, bool &outcome_uncertain)
{
    uctx::RawSpinLockUninterruptibleContext guard(backend_lock);
    return unmap_dma_page(kernel_backend, domain, iova, outcome_uncertain);
}

na_status_t kernel_destroy_dma_domain(device_domain &domain, bool &outcome_uncertain)
{
    uctx::RawSpinLockUninterruptibleContext guard(backend_lock);
    return destroy_dma_domain(kernel_backend, domain, outcome_uncertain);
}

bool kernel_create_device_domain(u32 unit_index, u16 segment, u8 bus, u8 device, u8 function, device_domain &domain,
                                 error &failure)
{
    uctx::RawSpinLockUninterruptibleContext guard(backend_lock);
    return kernel_backend.create_device_domain(unit_index, segment, bus, device, function, domain, failure);
}

bool kernel_default_deny_ready()
{
    uctx::RawSpinLockUninterruptibleContext guard(backend_lock);
    return kernel_backend.default_deny_ready();
}

u32 kernel_unit_count()
{
    uctx::RawSpinLockUninterruptibleContext guard(backend_lock);
    return kernel_backend.unit_count();
}

u32 kernel_active_unit_count()
{
    uctx::RawSpinLockUninterruptibleContext guard(backend_lock);
    return kernel_backend.active_unit_count();
}

u32 kernel_queued_invalidation_unit_count()
{
    uctx::RawSpinLockUninterruptibleContext guard(backend_lock);
    return kernel_backend.queued_invalidation_unit_count();
}

u32 kernel_interrupt_remapping_table_ready_unit_count()
{
    uctx::RawSpinLockUninterruptibleContext guard(backend_lock);
    return kernel_backend.interrupt_remapping_table_ready_unit_count();
}

bool kernel_configure_fault_event_interrupt(u32 unit_index, u32 destination_apic_id, u8 vector, error &failure)
{
    uctx::RawSpinLockUninterruptibleContext guard(backend_lock);
    return kernel_backend.configure_fault_event_interrupt_for_unit(unit_index, destination_apic_id, vector, failure);
}

bool kernel_fault_event_interrupt_enabled(u32 unit_index)
{
    uctx::RawSpinLockUninterruptibleContext guard(backend_lock);
    return kernel_backend.fault_event_interrupt_enabled_for_unit(unit_index);
}

bool kernel_fault_event_interrupt_uncertain(u32 unit_index)
{
    uctx::RawSpinLockUninterruptibleContext guard(backend_lock);
    return kernel_backend.fault_event_interrupt_state_uncertain_for_unit(unit_index);
}

bool kernel_drain_fault_records(u32 unit_index, fault_record *records, u32 capacity, fault_drain_result &result,
                                error &failure)
{
    uctx::RawSpinLockUninterruptibleContext guard(backend_lock);
    return kernel_backend.drain_fault_records_for_unit(unit_index, records, capacity, result, failure);
}

bool kernel_drain_fault_events(u32 unit_index, const fault_quiesce_ops &quiesce_ops, bool log_records,
                               fault_drain_report &report)
{
    report = {};
    fault_report_slot_lease report_slot(fault_report_busy);
    if (!report_slot)
    {
        report.retry = true;
        return true;
    }

    fault_drain_result drained{};
    error failure = error::none;
    report.drain_succeeded =
        kernel_drain_fault_records(unit_index, fault_record_scratch, max_fault_record_count, drained, failure);
    report.record_count = drained.count;
    report.event_log_overflow = drained.overflow;
    report.more_records = drained.more;
    report.status_code = static_cast<u32>(failure);

    if (drained.overflow)
    {
        KLOG_WARN("VT-d fault record overflow unit={} status={:08x}", unit_index, drained.status);
        (void)quarantine_registered_dma_fault_sources(provider_kind::intel_vtd, unit_index, true,
                                                      quiesce_registered_requester, &quiesce_ops);
    }
    if (!report.drain_succeeded)
    {
        KLOG_WARN("VT-d fault record drain failed unit={} error={} status={:08x} pending={}", unit_index,
                  static_cast<u8>(failure), drained.status, drained.more);
        if (!drained.overflow)
            (void)quarantine_registered_dma_fault_sources(provider_kind::intel_vtd, unit_index, false,
                                                          quiesce_registered_requester, &quiesce_ops);
        if (drained.count == 0)
            return false;
    }

    constexpr u32 max_logged_fault_records = 16;
    const u32 records_to_log =
        log_records ? (drained.count < max_logged_fault_records ? drained.count : max_logged_fault_records) : 0;
    for (u32 index = 0; index < records_to_log; ++index)
    {
        const auto &record = fault_record_scratch[index];
        KLOG_WARN("VT-d DMA fault unit={} requester={:04x} iova={:x} direction={} reason={:02x}", unit_index,
                  record.source_id, record.address,
                  !record.direction_known ? "unknown" : (record.is_read ? "read" : "write"), record.reason);
    }

    for (u32 index = 0; index < drained.count; ++index)
    {
        const auto &record = fault_record_scratch[index];
        const auto direction = !record.direction_known ? pci::dma_fault_direction::unknown
                               : record.is_read        ? pci::dma_fault_direction::device_read
                                                       : pci::dma_fault_direction::device_write;
        const dma_fault_event event{record.address, record.source_id, record.reason, direction};
        const auto dispatched =
            dispatch_registered_dma_fault_events(provider_kind::intel_vtd, unit_index, &event, 1, push_registered_fault,
                                                 quiesce_registered_requester, &quiesce_ops);
        report.source_overflow_count += dispatched.source_overflow_count;
    }
    if (report.source_overflow_count != 0)
        KLOG_WARN("VT-d fault source queues overflowed unit={} sources={}", unit_index, report.source_overflow_count);
    if (log_records && drained.count > records_to_log)
        KLOG_WARN("VT-d DMA fault records suppressed unit={} count={}", unit_index, drained.count - records_to_log);
    if (drained.more && log_records)
        KLOG_WARN("VT-d DMA fault records remain pending unit={} status={:08x}", unit_index, drained.status);
    return report.drain_succeeded && report.more_records;
}
} // namespace iommu::intel_vtd
