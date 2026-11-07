#include "kernel/iommu/amd_vi.hpp"

#include "kernel/arch/cpu_info.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/handle.hpp"
#include "kernel/iommu/amd_vi_fault_dispatch.hpp"
#include "kernel/lock.hpp"
#include "kernel/log.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/timer.hpp"
#include "kernel/ucontext.hpp"
#include "kernel_adapters.hpp"

KLOG_MODULE(acpi);

namespace iommu::amd_vi
{
namespace
{
controller kernel_backend{};
lock::spinlock_t backend_lock;
const firmware_inventory *kernel_inventory = nullptr;
controller_ops kernel_controller_operations{};
ioapic_match_counter kernel_ioapic_match_counter = nullptr;
void *kernel_ioapic_context = nullptr;
std::atomic_bool fault_report_busy{false};
std::atomic_bool fault_units_quarantined[max_hardware_units]{};
std::atomic_bool fault_poll_active{false};
fault_quiesce_ops fault_poll_quiesce_ops{};
constexpr u32 max_event_records = event_log_bytes / sizeof(command_entry);
event_record event_record_scratch[max_event_records]{};

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

const arch::acpi_boot::table_record *find_unique_ivrs_table(const arch::acpi_boot::snapshot &catalog, bool &duplicate)
{
    duplicate = false;
    const arch::acpi_boot::table_record *match = nullptr;
    const u32 signature = arch::acpi_boot::signature_value("IVRS");
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

void release_prepared_register_apertures(firmware_inventory &inventory, const provider_bootstrap_config &configuration)
{
    if (configuration.release_registers == nullptr)
        return;
    for (auto &record : inventory.amd_ivhd_records)
    {
        if (!record.selected)
            continue;
        if (record.resource_lease_id == 0 && record.kernel_virtual_base == 0)
            continue;
        if (record.resource_lease_id == 0 || record.kernel_virtual_base == 0 ||
            !configuration.release_registers(configuration.register_aperture_context, record.register_base,
                                             record.register_length, inventory.firmware_generation,
                                             record.resource_lease_id, record.kernel_virtual_base))
        {
            KLOG_ERROR("AMD-Vi register aperture rollback failed base={:x} lease={} virtual={:x}", record.register_base,
                       record.resource_lease_id, record.kernel_virtual_base);
            continue;
        }
        record.resource_lease_id = 0;
        record.kernel_virtual_base = 0;
    }
}

bool prepare_register_apertures(firmware_inventory &inventory, const provider_bootstrap_config &configuration)
{
    if (configuration.acquire_registers == nullptr || configuration.release_registers == nullptr)
        return false;
    u32 selected_count = 0;
    for (const auto &record : inventory.amd_ivhd_records)
    {
        if (!record.selected)
            continue;
        selected_count++;
        if (record.resource_lease_id != 0 || record.kernel_virtual_base != 0)
            return false;
    }
    if (selected_count == 0)
        return false;

    for (auto &record : inventory.amd_ivhd_records)
    {
        if (!record.selected)
            continue;
        u64 lease_id = 0;
        u64 kernel_virtual_base = 0;
        const bool acquired = configuration.acquire_registers(
            configuration.register_aperture_context, record.register_base, record.register_length,
            inventory.firmware_generation, lease_id, kernel_virtual_base);
        if (!acquired || lease_id == 0 || kernel_virtual_base == 0)
        {
            if (lease_id != 0 || kernel_virtual_base != 0)
            {
                record.resource_lease_id = lease_id;
                record.kernel_virtual_base = kernel_virtual_base;
            }
            release_prepared_register_apertures(inventory, configuration);
            return false;
        }
        record.resource_lease_id = lease_id;
        record.kernel_virtual_base = kernel_virtual_base;
    }
    return true;
}

bool push_registered_fault(const void *, const dma_fault_source_registration &entry, const pci::dma_fault_record &fault)
{
    handle_t<pci::dma_fault_source> source(entry.source_control);
    return source->push_fault(fault);
}

na_status_t disable_registered_requester(const fault_quiesce_ops &ops, const dma_fault_source_registration &entry)
{
    bool outcome_uncertain = false;
    const na_status_t status = ops.disable_bus_master == nullptr
                                   ? NA_STATUS_NOT_SUPPORTED
                                   : ops.disable_bus_master(ops.context, entry.identity, outcome_uncertain);
    if (status != NA_STATUS_OK || outcome_uncertain)
        KLOG_ERROR("AMD-Vi unable to stop requester segment={} requester={:04x} status={} uncertain={}", entry.segment,
                   entry.requester_id, status, outcome_uncertain);
    return status;
}

void quarantine_registered_requester(const void *context, const dma_fault_source_registration &entry,
                                     bool unit_overflow)
{
    const auto &ops = *static_cast<const fault_quiesce_ops *>(context);
    handle_t<pci::dma_fault_source> source(entry.source_control);
    if (unit_overflow)
        source->note_unit_overflow();
    else
        source->close();
    (void)disable_registered_requester(ops, entry);
}

void quarantine_overflowed_requester(const void *context, const dma_fault_source_registration &entry, bool)
{
    (void)disable_registered_requester(*static_cast<const fault_quiesce_ops *>(context), entry);
}

bool provider_fault_polling_active() { return fault_poll_active.load(std::memory_order_acquire); }

void amd_vi_fault_poll_tick(timeclock::microsecond_t) noexcept
{
    if (!fault_poll_active.load(std::memory_order_acquire))
        return;

    bool events_remain = false;
    const u32 unit_count = kernel_unit_count();
    for (u32 unit_index = 0; unit_index < unit_count; ++unit_index)
    {
        fault_drain_report report{};
        const bool more =
            drain_provider_fault_events(provider_kind::amd_vi, unit_index, fault_poll_quiesce_ops, false, report);
        events_remain = report.retry || more || events_remain;
        if (report.retry)
            continue;
        if (!report.drain_succeeded || report.event_log_overflow || report.unexpected_record_count != 0 ||
            report.source_overflow_count != 0)
            KLOG_WARN("AMD-Vi fault poll unit={} records={} unexpected={} queue_overflow={} overflow={} success={} "
                      "status={} quarantined={}",
                      unit_index, report.record_count, report.unexpected_record_count, report.source_overflow_count,
                      report.event_log_overflow, report.drain_succeeded, report.status_code, report.unit_quarantined);
    }

    if (!fault_poll_active.load(std::memory_order_acquire))
        return;
    const auto next_poll =
        timer::schedule_after(events_remain ? 1 : 1'000, timer::timer_handler::bind<&amd_vi_fault_poll_tick>());
    if (next_poll == timer::invalid_watcher_id)
    {
        fault_poll_active.store(false, std::memory_order_release);
        if (!quarantine_provider_fault_units(provider_kind::amd_vi, fault_poll_quiesce_ops))
            KLOG_ERROR("AMD-Vi fault polling stopped without quarantining any provider unit");
    }
}

bool start_amd_vi_fault_polling(const fault_quiesce_ops &quiesce_ops)
{
    if (!kernel_default_deny_ready())
        return false;
    if (fault_poll_active.load(std::memory_order_acquire))
        return true;

    fault_poll_quiesce_ops = quiesce_ops;
    fault_poll_active.store(true, std::memory_order_release);
    const auto watcher = timer::schedule_after(1'000, timer::timer_handler::bind<&amd_vi_fault_poll_tick>());
    if (watcher != timer::invalid_watcher_id)
    {
        KLOG_INFO("ACPI: AMD-Vi DMA provider ready; fault polling active interval_us=1000");
        return true;
    }

    fault_poll_active.store(false, std::memory_order_release);
    fault_poll_quiesce_ops = {};
    return false;
}

na_status_t create_dma_backend(const device_source_ref &source, const pci::function_identity &identity,
                               process_id owner, handle_t<pci::dma_fault_source> &fault_source, void *&backend_state,
                               u8 &address_width, u32 &unit_index, bool &outcome_uncertain)
{
    backend_state = nullptr;
    address_width = 0;
    unit_index = ~u32{0};
    outcome_uncertain = false;
    if (owner == 0 || identity.segment > 0xffff || identity.bus > 0xff || identity.device >= 32 ||
        identity.function >= 8 || !source.valid() || source.unit.provider != provider_kind::amd_vi ||
        source.unit.generation != identity.firmware_generation || source.segment != identity.segment ||
        source.unit.opaque[0] > ~u32{0} || source.unit.opaque[1] >= max_hardware_units ||
        source.device_id != static_cast<u16>((identity.bus << 8) | (identity.device << 3) | identity.function) ||
        !fault_source || fault_source->owner() != owner || kernel_inventory == nullptr ||
        kernel_controller_operations.read_pci_config32 == nullptr || !kernel_default_deny_ready())
        return NA_STATUS_INVALID_ARGUMENT;
    if (!provider_fault_polling_active())
        return NA_STATUS_NOT_SUPPORTED;
    if (memory::KernelCommonAllocatorV == nullptr)
        return NA_STATUS_RESOURCE_EXHAUSTED;

    const iommu::pci_function_location function{static_cast<u16>(identity.segment), static_cast<u8>(identity.bus),
                                                static_cast<u8>(identity.device), static_cast<u8>(identity.function)};
    device_source_ref resolved_source{};
    if (kernel_resolve_pci_source(*kernel_inventory, function, identity.firmware_generation, resolved_source) !=
            resolution_result::resolved ||
        resolved_source.unit.opaque[0] != source.unit.opaque[0] ||
        resolved_source.unit.opaque[1] != source.unit.opaque[1] || resolved_source.segment != source.segment ||
        resolved_source.device_id != source.device_id)
        return NA_STATUS_ACCESS_DENIED;

    const u32 record_index = static_cast<u32>(resolved_source.unit.opaque[0]);
    if (record_index >= kernel_inventory->amd_ivhd_records.size())
        return NA_STATUS_ACCESS_DENIED;
    const auto &ivhd_record = kernel_inventory->amd_ivhd_records.at(record_index);
    u8 iommu_physical_address_width = 0;
    if (!physical_address_width_from_iommu_capability(ivhd_record, kernel_controller_operations.read_pci_config32,
                                                      kernel_controller_operations.context,
                                                      iommu_physical_address_width))
        return NA_STATUS_NOT_SUPPORTED;
    const u8 cpu_physical_address_width =
        static_cast<u8>(arch::cpu_info::get_feature(arch::cpu_info::feature::max_phy_addr));
    const u8 maximum_address_width = cpu_physical_address_width < iommu_physical_address_width
                                         ? cpu_physical_address_width
                                         : iommu_physical_address_width;
    if (maximum_address_width < 12)
        return NA_STATUS_NOT_SUPPORTED;

    unit_index = static_cast<u32>(source.unit.opaque[1]);
    const u16 requester_id = source.device_id;
    void *storage =
        memory::KernelCommonAllocatorV->allocate(sizeof(dma_domain_backend_state), alignof(dma_domain_backend_state));
    if (storage == nullptr)
        return NA_STATUS_RESOURCE_EXHAUSTED;
    auto *state = new (storage) dma_domain_backend_state();
    state->identity = identity;
    state->unit_index = unit_index;
    state->requester_id = requester_id;
    state->fault_source = fault_source;

    controller_error failure = controller_error::none;
    const auto page_operations = kernel_page_table_ops();
    if (!kernel_create_device_domain(unit_index, requester_id, maximum_address_width, page_operations, state->domain,
                                     failure))
    {
        if (state->domain.valid || state->domain.translations.root_virtual != nullptr)
        {
            backend_state = state;
            outcome_uncertain = true;
            return NA_STATUS_IO_ERROR;
        }
        memory::Delete<>(memory::KernelCommonAllocatorV, state);
        return to_status(failure);
    }

    if (!register_dma_fault_source(provider_kind::amd_vi, unit_index, identity, requester_id, state->fault_source))
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

    address_width = state->domain.translations.physical_address_width;
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
    unregister_dma_fault_source(provider_kind::amd_vi, state->unit_index, state->identity, state->requester_id);
    memory::Delete<>(memory::KernelCommonAllocatorV, state);
    return NA_STATUS_OK;
}

bool find_kernel_register(const firmware_inventory *snapshot, u64 base, u32 offset, u32 width,
                          volatile byte *&register_address)
{
    if (snapshot == nullptr || width == 0)
        return false;
    for (const auto &unit : snapshot->amd_ivhd_records)
    {
        if (!unit.selected || unit.register_base != base || unit.resource_lease_id == 0 ||
            unit.kernel_virtual_base == 0 || offset > unit.register_length || width > unit.register_length - offset ||
            unit.kernel_virtual_base > ~u64{0} - offset)
            continue;
        register_address = reinterpret_cast<volatile byte *>(unit.kernel_virtual_base + offset);
        return true;
    }
    return false;
}

bool read_kernel_register64(void *context, u64 base, u32 offset, u64 &value)
{
    volatile byte *address = nullptr;
    if (!find_kernel_register(static_cast<const firmware_inventory *>(context), base, offset, sizeof(u64), address) ||
        (offset & (alignof(u64) - 1)) != 0)
        return false;
    _mfence();
    value = *reinterpret_cast<volatile u64 *>(address);
    _mfence();
    return true;
}

bool write_kernel_register64(void *context, u64 base, u32 offset, u64 value)
{
    volatile byte *address = nullptr;
    if (!find_kernel_register(static_cast<const firmware_inventory *>(context), base, offset, sizeof(u64), address) ||
        (offset & (alignof(u64) - 1)) != 0)
        return false;
    *reinterpret_cast<volatile u64 *>(address) = value;
    _mfence();
    return true;
}

u64 kernel_register_aperture_length(void *context, u64 base)
{
    const auto *snapshot = static_cast<const firmware_inventory *>(context);
    if (snapshot == nullptr)
        return 0;
    for (const auto &unit : snapshot->amd_ivhd_records)
    {
        if (unit.selected && unit.register_base == base && unit.resource_lease_id != 0 && unit.kernel_virtual_base != 0)
            return unit.register_length;
    }
    return 0;
}

bool allocate_kernel_contiguous(void *, u32 bytes, void *&virtual_address, u64 &physical_address)
{
    virtual_address = nullptr;
    physical_address = 0;
    if (memory::KernelBuddyAllocatorV == nullptr || bytes == 0 || bytes % memory::page_size != 0)
        return false;
    virtual_address = memory::KernelBuddyAllocatorV->try_allocate(bytes, memory::page_size);
    if (virtual_address == nullptr)
        return false;
    physical_address = reinterpret_cast<u64>(memory::va2pa(virtual_address)());
    if ((reinterpret_cast<u64>(virtual_address) & (memory::page_size - 1)) != 0 ||
        (physical_address & (memory::page_size - 1)) != 0)
    {
        memory::KernelBuddyAllocatorV->deallocate(virtual_address);
        virtual_address = nullptr;
        physical_address = 0;
        return false;
    }
    return true;
}

void release_kernel_contiguous(void *, void *virtual_address, u32)
{
    if (virtual_address != nullptr && memory::KernelBuddyAllocatorV != nullptr)
        memory::KernelBuddyAllocatorV->deallocate(virtual_address);
}

bool flush_kernel_range(void *, void *virtual_address, u32 bytes)
{
    constexpr u32 clflush_leaf_bit = 19;
    if (virtual_address == nullptr || bytes == 0)
        return false;
    const auto cpuid = arch::cpu_info::read_cpuid(1);
    if ((cpuid.edx & (1u << clflush_leaf_bit)) == 0)
        return false;
    const u32 cache_line_bytes = ((cpuid.ebx >> 8) & 0xff) * 8;
    if (cache_line_bytes == 0 || cache_line_bytes > memory::page_size ||
        (cache_line_bytes & (cache_line_bytes - 1)) != 0)
        return false;

    const u64 base = reinterpret_cast<u64>(virtual_address);
    if (base > ~u64{0} - bytes || base + bytes > ~u64{0} - (cache_line_bytes - 1))
        return false;
    const u64 first = base & ~(static_cast<u64>(cache_line_bytes) - 1);
    const u64 end = (base + bytes + cache_line_bytes - 1) & ~(static_cast<u64>(cache_line_bytes) - 1);
    for (u64 address = first; address < end; address += cache_line_bytes)
        __asm__ __volatile__("clflush (%0)" : : "r"(address) : "memory");
    _mfence();
    return true;
}

bool allocate_kernel_page(void *, void *&virtual_page, u64 &physical_address)
{
    virtual_page = memory::malloc_page();
    if (virtual_page == nullptr)
        return false;
    physical_address = reinterpret_cast<u64>(memory::va2pa(virtual_page)());
    return true;
}

void release_kernel_page(void *, void *virtual_page) { memory::free_page(virtual_page); }

void *kernel_page_physical_to_virtual(void *, u64 physical_address)
{
    if (physical_address == 0 || (physical_address & (memory::page_size - 1)) != 0)
        return nullptr;
    return memory::pa2va<void *>(phy_addr_t::from(physical_address));
}

bool flush_kernel_page(void *, void *virtual_page)
{
    return flush_kernel_range(nullptr, virtual_page, memory::page_size);
}

bool provider_install_pci_msi(void *, const device_source_ref &source, u32 destination_apic_id, u8 vector,
                              interrupt_route_token &token, msi_message &message, bool &outcome_uncertain)
{
    token = {};
    message = {};
    outcome_uncertain = false;
    if (kernel_inventory == nullptr)
        return false;
    controller_error failure = controller_error::none;
    if (kernel_install_pci_msi_route(source, kernel_inventory->firmware_generation, destination_apic_id, vector, token,
                                     message, outcome_uncertain, failure))
        return true;
    KLOG_WARN("AMD-Vi PCI MSI route installation failed segment={} requester={} unit={} error={} uncertain={}",
              source.segment, source.device_id, source.unit.opaque[1], static_cast<u32>(failure), outcome_uncertain);
    return false;
}

bool provider_install_ioapic(void *, const device_source_ref &source, u32 destination_apic_id, u8 vector,
                             bool low_polarity, bool level_triggered, ioapic_route_encoding &route)
{
    route = {};
    if (kernel_inventory == nullptr)
        return false;
    controller_error failure = controller_error::none;
    if (kernel_install_ioapic_route(source, kernel_inventory->firmware_generation, destination_apic_id, vector,
                                    low_polarity, level_triggered, route, failure))
        return true;
    KLOG_WARN("AMD-Vi IOAPIC route installation failed segment={} requester={} unit={} vector={} error={} uncertain={}",
              source.segment, source.device_id, source.unit.opaque[1], vector, static_cast<u32>(failure),
              kernel_unit_state_uncertain(static_cast<u32>(source.unit.opaque[1])));
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
    controller_error failure = controller_error::none;
    if (kernel_retire_route(token, kernel_inventory->firmware_generation, failure))
        return true;
    KLOG_ERROR("AMD-Vi interrupt route retirement failed error={}", static_cast<u32>(failure));
    return false;
}

bool provider_interrupt_remapping_uncertain(void *) { return kernel_state_uncertain(); }

resolution_result provider_resolve_pci(void *, const pci_function_location &function, u64 generation,
                                       device_source_ref &source)
{
    source = {};
    if (kernel_inventory == nullptr)
        return resolution_result::reject;
    return kernel_resolve_pci_source(*kernel_inventory, function, generation, source);
}

resolution_result provider_resolve_ioapic(void *, u8 enumeration_id, u64 generation, device_source_ref &source)
{
    source = {};
    if (kernel_inventory == nullptr || kernel_ioapic_match_counter == nullptr)
        return resolution_result::reject;
    const u32 match_count = kernel_ioapic_match_counter(kernel_ioapic_context, enumeration_id);
    u8 dte_setting = 0;
    return kernel_resolve_ioapic_source(*kernel_inventory, enumeration_id, match_count, generation, dte_setting,
                                        source);
}
} // namespace

static controller_ops make_kernel_controller_ops(const firmware_inventory *snapshot,
                                                 const pci_requester_platform_ops &platform_operations) noexcept
{
    if (snapshot == nullptr || platform_operations.bus_master_enabled == nullptr ||
        platform_operations.requester_present == nullptr || platform_operations.read_config32 == nullptr)
        return {};
    return {read_kernel_register64,
            write_kernel_register64,
            kernel_register_aperture_length,
            allocate_kernel_contiguous,
            release_kernel_contiguous,
            flush_kernel_range,
            platform_operations.bus_master_enabled,
            platform_operations.requester_present,
            platform_operations.read_config32,
            const_cast<firmware_inventory *>(snapshot)};
}

page_table_ops kernel_page_table_ops() noexcept
{
    return {allocate_kernel_page, release_kernel_page, kernel_page_physical_to_virtual, flush_kernel_page, nullptr};
}

provider_initialization_report kernel_initialize(const firmware_inventory &inventory,
                                                 const pci_requester_platform_ops &platform_operations,
                                                 bool pci_bus_mastering_quiesced, const u8 *ioapic_enumeration_ids,
                                                 u32 ioapic_source_count, ioapic_match_counter count_ioapic_matches,
                                                 void *ioapic_context)
{
    provider_initialization_report report{};
    const controller_ops operations = make_kernel_controller_ops(&inventory, platform_operations);
    if (operations.read64 == nullptr)
    {
        report.status = NA_STATUS_INVALID_ARGUMENT;
        return report;
    }
    if (count_ioapic_matches == nullptr || ioapic_source_count > max_ioapic_interrupt_sources ||
        (ioapic_source_count != 0 && ioapic_enumeration_ids == nullptr))
    {
        report.status = NA_STATUS_INVALID_ARGUMENT;
        return report;
    }

    ioapic_interrupt_source ioapic_sources[max_ioapic_interrupt_sources]{};
    for (u32 index = 0; index < ioapic_source_count; ++index)
    {
        const u8 enumeration_id = ioapic_enumeration_ids[index];
        const u32 match_count = count_ioapic_matches(ioapic_context, enumeration_id);
        u32 ivhd_record_index = ~u32{0};
        u16 segment = 0;
        u16 requester_id = 0;
        u8 dte_setting = 0;
        if (resolve_ioapic_requester(inventory, enumeration_id, match_count, ivhd_record_index, segment, requester_id,
                                     dte_setting) != resolution_result::resolved)
        {
            report.status = NA_STATUS_NOT_SUPPORTED;
            return report;
        }
        ioapic_sources[index] = {ivhd_record_index, segment, requester_id, enumeration_id, dte_setting};
    }

    const activation_preconditions preconditions{pci_bus_mastering_quiesced, true, ioapic_sources, ioapic_source_count};
    if (!register_provider_fault_adapter(provider_kind::amd_vi, kernel_drain_fault_events,
                                         kernel_quarantine_fault_units))
    {
        report.status = NA_STATUS_INVALID_ARGUMENT;
        return report;
    }
    if (!register_provider_fault_poll_adapter(provider_kind::amd_vi, start_amd_vi_fault_polling,
                                              provider_fault_polling_active))
    {
        report.status = NA_STATUS_INVALID_ARGUMENT;
        return report;
    }
    const provider_dma_domain_adapter_ops domain_ops{create_dma_backend,          map_dma_backend,
                                                     unmap_dma_backend,           destroy_dma_backend,
                                                     kernel_unit_state_uncertain, provider_fault_polling_active};
    if (!register_provider_dma_domain_adapter(provider_kind::amd_vi, domain_ops))
    {
        report.status = NA_STATUS_INVALID_ARGUMENT;
        return report;
    }
    uctx::RawSpinLockUninterruptibleContext guard(backend_lock);
    controller_error failure = controller_error::none;
    const bool initialized = kernel_backend.initialize(inventory, operations, preconditions, failure);
    report.status = initialized ? NA_STATUS_OK : to_status(failure);
    report.selected_units = kernel_backend.unit_count();
    report.active_units = kernel_backend.active_unit_count();
    report.invalidation_ready_units = report.active_units;
    report.interrupt_remapping_units = kernel_backend.interrupt_remapping_supported_unit_count();
    report.state_uncertain = kernel_backend.state_uncertain() || report.status == NA_STATUS_IO_ERROR;
    if (!initialized)
        return report;
    kernel_inventory = &inventory;
    kernel_controller_operations = operations;
    kernel_ioapic_match_counter = count_ioapic_matches;
    kernel_ioapic_context = ioapic_context;
    return report;
}

provider_ops kernel_provider_ops(dma_domain_creator create_dma_domain)
{
    provider_ops provider{};
    provider.kind = provider_kind::amd_vi;
    provider.resolve_pci = provider_resolve_pci;
    provider.resolve_ioapic = provider_resolve_ioapic;
    provider.install_pci_msi = provider_install_pci_msi;
    provider.install_ioapic = provider_install_ioapic;
    provider.route_matches_pci = provider_route_matches_pci;
    provider.retire_route = provider_retire_route;
    provider.interrupt_remapping_activation_uncertain = provider_interrupt_remapping_uncertain;
    provider.create_dma_domain = create_dma_domain;
    provider.activation_scope = interrupt_activation_scope::per_requester;
    return provider;
}

provider_initialization_report kernel_initialize_provider(const provider_bootstrap_config &configuration)
{
    provider_initialization_report report{};
    if (configuration.firmware_inventory == nullptr || configuration.create_dma_domain == nullptr ||
        configuration.validate_register_range == nullptr)
    {
        report.status = NA_STATUS_INVALID_ARGUMENT;
        return report;
    }
    const auto &catalog = *static_cast<const arch::acpi_boot::snapshot *>(configuration.firmware_inventory);
    bool duplicate_table = false;
    const auto *table = find_unique_ivrs_table(catalog, duplicate_table);
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
        KLOG_WARN("AMD-Vi IVRS inventory rejected error={} offset={}", static_cast<u32>(inventory->ivrs_error),
                  inventory->ivrs_error_offset);
        release_firmware_inventory(inventory);
        report.status = NA_STATUS_NOT_SUPPORTED;
        return report;
    }
    u32 selected_units = 0;
    for (const auto &record : inventory->amd_ivhd_records)
        selected_units += record.selected ? 1 : 0;
    report.selected_units = selected_units;
    if (selected_units == 0 || !configuration.pci_bus_mastering_quiesced || !inventory->amd_ivmd_records.empty())
    {
        release_firmware_inventory(inventory);
        report.status = NA_STATUS_NOT_SUPPORTED;
        return report;
    }
    if (!prepare_register_apertures(*inventory, configuration))
    {
        bool aperture_retained = false;
        for (const auto &record : inventory->amd_ivhd_records)
        {
            if (record.selected && (record.resource_lease_id != 0 || record.kernel_virtual_base != 0))
                aperture_retained = true;
        }
        if (aperture_retained)
            kernel_inventory = inventory;
        else
            release_firmware_inventory(inventory);
        report.status = NA_STATUS_RESOURCE_EXHAUSTED;
        return report;
    }
    KLOG_INFO("ACPI: AMD-Vi register apertures reserved and mapped selected_units={}", selected_units);

    kernel_inventory = inventory;
    report = kernel_initialize(*inventory, configuration.pci_requesters, configuration.pci_bus_mastering_quiesced,
                               configuration.ioapic_enumeration_ids, configuration.ioapic_source_count,
                               configuration.count_madt_ioapic_matches, configuration.platform_context);
    if (report.status != NA_STATUS_OK)
        return report;
    report.fault_delivery = fault_delivery_mode::polling;

    if (!kernel_provider_router().register_provider(kernel_provider_ops(configuration.create_dma_domain)))
        report.status = NA_STATUS_INVALID_ARGUMENT;
    if (report.status == NA_STATUS_OK)
    {
        KLOG_INFO("ACPI: AMD-Vi default-deny active units={} IOAPIC pass-through sources={} "
                  "DMA provider registered interrupt-remapping-capable-units={} (basic 512-entry mode); "
                  "fault polling deferred until timer initialization",
                  report.active_units, configuration.ioapic_source_count, report.interrupt_remapping_units);
    }
    else
    {
        KLOG_WARN("ACPI: AMD-Vi default-deny unavailable status={} selected_units={} active_units={} "
                  "state_uncertain={}",
                  report.status, report.selected_units, report.active_units, report.state_uncertain);
    }
    return report;
}

resolution_result kernel_resolve_pci_source(const firmware_inventory &inventory, const pci_function_location &function,
                                            u64 generation, device_source_ref &source)
{
    uctx::RawSpinLockUninterruptibleContext guard(backend_lock);
    return resolve_pci_source(inventory, kernel_backend, function, generation, source);
}

resolution_result kernel_resolve_ioapic_source(const firmware_inventory &inventory, u8 enumeration_id,
                                               u32 madt_match_count, u64 generation, u8 &dte_setting,
                                               device_source_ref &source)
{
    uctx::RawSpinLockUninterruptibleContext guard(backend_lock);
    return resolve_ioapic_source(inventory, kernel_backend, enumeration_id, madt_match_count, generation, dte_setting,
                                 source);
}

bool kernel_install_pci_msi_route(const device_source_ref &source, u64 firmware_generation, u32 destination_apic_id,
                                  u8 vector, interrupt_route_token &token, msi_message &message,
                                  bool &outcome_uncertain, controller_error &failure)
{
    uctx::RawSpinLockUninterruptibleContext guard(backend_lock);
    return install_pci_msi_route(kernel_backend, source, firmware_generation, destination_apic_id, vector, token,
                                 message, outcome_uncertain, failure);
}

bool kernel_install_ioapic_route(const device_source_ref &source, u64 firmware_generation, u32 destination_apic_id,
                                 u8 vector, bool low_polarity, bool level_triggered, ioapic_route_encoding &route,
                                 controller_error &failure)
{
    uctx::RawSpinLockUninterruptibleContext guard(backend_lock);
    return install_ioapic_route(kernel_backend, source, firmware_generation, destination_apic_id, vector, low_polarity,
                                level_triggered, route, failure);
}

bool kernel_route_matches_pci(const interrupt_route_token &token, const device_source_ref &source) noexcept
{
    uctx::RawSpinLockUninterruptibleContext guard(backend_lock);
    return route_matches_pci(kernel_backend, token, source);
}

bool kernel_retire_route(const interrupt_route_token &token, u64 firmware_generation, controller_error &failure)
{
    uctx::RawSpinLockUninterruptibleContext guard(backend_lock);
    return retire_route(kernel_backend, token, firmware_generation, failure);
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

bool kernel_create_device_domain(u32 unit_index, u16 requester_id, u8 physical_address_width,
                                 const page_table_ops &page_operations, device_domain &domain,
                                 controller_error &failure)
{
    uctx::RawSpinLockUninterruptibleContext guard(backend_lock);
    return kernel_backend.create_device_domain(unit_index, requester_id, physical_address_width, page_operations,
                                               domain, failure);
}

bool kernel_default_deny_ready()
{
    uctx::RawSpinLockUninterruptibleContext guard(backend_lock);
    return kernel_backend.default_deny_ready();
}

bool kernel_state_uncertain()
{
    uctx::RawSpinLockUninterruptibleContext guard(backend_lock);
    return kernel_backend.state_uncertain();
}

bool kernel_unit_state_uncertain(u32 unit_index)
{
    uctx::RawSpinLockUninterruptibleContext guard(backend_lock);
    const auto *unit = kernel_backend.unit(unit_index);
    return unit == nullptr || unit->state_uncertain;
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

u32 kernel_interrupt_remapping_supported_unit_count()
{
    uctx::RawSpinLockUninterruptibleContext guard(backend_lock);
    return kernel_backend.interrupt_remapping_supported_unit_count();
}

bool kernel_quarantine_unit(u32 unit_index)
{
    uctx::RawSpinLockUninterruptibleContext guard(backend_lock);
    if (kernel_backend.unit(unit_index) == nullptr)
        return false;
    kernel_backend.quarantine_unit(unit_index);
    return true;
}

bool kernel_drain_events(u32 unit_index, event_record *records, u32 capacity, event_drain_result &result,
                         controller_error &failure)
{
    uctx::RawSpinLockUninterruptibleContext guard(backend_lock);
    return kernel_backend.drain_events(unit_index, records, capacity, result, failure);
}

bool kernel_drain_fault_events(u32 unit_index, const fault_quiesce_ops &quiesce_ops, bool, fault_drain_report &report)
{
    report = {};
    fault_report_slot_lease report_slot(fault_report_busy);
    if (!report_slot)
    {
        report.retry = true;
        return true;
    }
    if (unit_index >= max_hardware_units || fault_units_quarantined[unit_index].load(std::memory_order_acquire))
    {
        report.unit_quarantined = true;
        return false;
    }

    event_drain_result drained{};
    controller_error failure = controller_error::none;
    report.drain_succeeded = kernel_drain_events(unit_index, event_record_scratch, max_event_records, drained, failure);
    report.record_count = drained.count;
    report.event_log_overflow = drained.overflow;
    report.more_records = drained.more;
    report.status_code = static_cast<u32>(failure);

    const auto dispatch = decode_fault_events(event_record_scratch, drained.count, [&](const dma_fault_event &event) {
        return dispatch_registered_dma_fault_events(provider_kind::amd_vi, unit_index, &event, 1, push_registered_fault,
                                                    quarantine_overflowed_requester, &quiesce_ops);
    });
    report.unexpected_record_count = dispatch.unexpected_event_count;
    report.source_overflow_count = dispatch.source_overflow_count;

    if (drained.count != 0)
    {
        const auto &first = event_record_scratch[0];
        KLOG_INFO("AMD-Vi event log drained unit={} records={} first_code={} first_requester={:04x} first_address={} "
                  "first_write={} first_interrupt={} first_raw={:016x}:{:016x}",
                  unit_index, drained.count, first.event_code, first.device_id, log::hex(first.address), first.is_write,
                  first.is_interrupt, first.raw[0], first.raw[1]);
    }

    const char *quarantine_reason = drained.overflow          ? "event-log overflow"
                                    : !report.drain_succeeded ? "event-log drain failure"
                                                              : "unexpected event record";
    report.unit_quarantined = quarantine_fault_poll_unit(
        unit_index, report.drain_succeeded, drained.overflow, dispatch,
        [&](u32 quarantine_unit_index, bool event_log_overflow) {
            fault_units_quarantined[quarantine_unit_index].store(true, std::memory_order_release);
            (void)kernel_quarantine_unit(quarantine_unit_index);
            (void)quarantine_registered_dma_fault_sources(provider_kind::amd_vi, quarantine_unit_index,
                                                          event_log_overflow, quarantine_registered_requester,
                                                          &quiesce_ops);
        });

    if (report.unexpected_record_count != 0 || report.source_overflow_count != 0 || report.unit_quarantined)
        KLOG_WARN("AMD-Vi fault drain unit={} records={} unexpected={} queue_overflow={} overflow={} success={} "
                  "error={} reason={}",
                  unit_index, drained.count, report.unexpected_record_count, report.source_overflow_count,
                  drained.overflow, report.drain_succeeded, static_cast<u32>(failure), quarantine_reason);
    return report.drain_succeeded && report.more_records && !report.unit_quarantined;
}

bool kernel_quarantine_fault_units(const fault_quiesce_ops &quiesce_ops)
{
    const u32 count = kernel_unit_count();
    for (u32 unit_index = 0; unit_index < count; ++unit_index)
    {
        fault_units_quarantined[unit_index].store(true, std::memory_order_release);
        (void)kernel_quarantine_unit(unit_index);
        (void)quarantine_registered_dma_fault_sources(provider_kind::amd_vi, unit_index, false,
                                                      quarantine_registered_requester, &quiesce_ops);
    }
    return count != 0;
}
} // namespace iommu::amd_vi
