#pragma once

#include "kernel/arch/acpi/boot_tables.hpp"
#include "kernel/iommu/backend.hpp"
#include "kernel/iommu/firmware_inventory.hpp"
#include "kernel/iommu/intel_vtd_page_table.hpp"
#include "kernel/types.hpp"
#include "naos/abi.h"

namespace lock
{
struct spinlock_t;
}

namespace iommu::intel_vtd
{

enum class error : u8
{
    none,
    invalid_register_aperture,
    register_io_failed,
    invalid_capability,
    unsupported_address_width,
    cache_maintenance_unavailable,
    translation_pre_enabled,
    allocation_failed,
    invalid_root_page,
    hardware_timeout,
    invalid_inventory,
    reserved_memory_requires_domain,
    too_many_units,
    invalid_device_id,
    device_scope_unresolved,
    context_already_present,
    context_not_present,
    invalid_page_table,
    unsupported_page_table_width,
    domain_id_exhausted,
    context_domain_mismatch,
    dma_page_already_mapped,
    dma_page_not_mapped,
    domain_state_uncertain,
    domain_has_mappings,
    domain_mapping_count_overflow,
    queued_invalidation_unsupported,
    queued_invalidation_pre_enabled,
    queued_invalidation_queue_busy,
    interrupt_remapping_unsupported,
    interrupt_remapping_pre_enabled,
    interrupt_remapping_table_full,
    interrupt_remapping_entry_not_present,
    invalid_interrupt_route,
    invalid_fault_record,
    fault_event_interrupt_pre_enabled,
};

na_status_t to_status(error failure) noexcept;

struct fault_record
{
    u64 address = 0;
    u16 source_id = 0;
    u8 reason = 0;
    // The FRCD T bit distinguishes request classes that do not map cleanly
    // to a device read/write direction (for example page requests and atomic
    // operations). Keep direction unknown until a reason-specific decoder can
    // prove it.
    bool direction_known = false;
    bool is_read = false;
};

struct fault_drain_result
{
    u32 status = 0;
    u32 count = 0;
    bool more = false;
    bool overflow = false;
};

struct register_ops
{
    bool (*read32)(void *context, u64 base, u32 offset, u32 &value);
    bool (*write32)(void *context, u64 base, u32 offset, u32 value);
    bool (*read64)(void *context, u64 base, u32 offset, u64 &value);
    bool (*write64)(void *context, u64 base, u32 offset, u64 value);
    bool (*allocate_page)(void *context, void *&virtual_page, u64 &physical_address);
    void (*release_page)(void *context, void *virtual_page);
    void *(*physical_to_virtual)(void *context, u64 physical_address);
    bool (*flush_page)(void *context, void *virtual_page);
    void *context;
};

/// Build the kernel register/page-table access surface for an already mapped
/// DMAR inventory. The ACPI layer owns firmware mapping and unmapping only.
class controller;
struct device_domain;
register_ops kernel_register_ops(const firmware_inventory *snapshot) noexcept;
provider_initialization_report kernel_initialize(const firmware_inventory &snapshot,
                                                 bridge_info_reader read_bridge_info, void *bridge_context,
                                                 ioapic_match_counter count_ioapic_matches, void *ioapic_context);
provider_ops kernel_provider_ops(dma_domain_creator create_dma_domain);
bool kernel_install_pci_msi_route(const device_source_ref &source, u64 firmware_generation, u32 destination_apic_id,
                                  u8 vector, interrupt_route_token &token, msi_message &message,
                                  bool &outcome_uncertain, error &failure);
bool kernel_install_ioapic_route(const device_source_ref &source, u64 firmware_generation, u32 destination_apic_id,
                                 u8 vector, bool low_polarity, bool level_triggered, ioapic_route_encoding &route,
                                 error &failure);
bool kernel_route_matches_pci(const interrupt_route_token &token, const device_source_ref &source) noexcept;
bool kernel_retire_route(const interrupt_route_token &token, u64 firmware_generation, error &failure);
bool kernel_interrupt_remapping_ready();
bool kernel_enable_interrupt_remapping(bool &outcome_uncertain, u8 &error_code);
bool kernel_interrupt_remapping_enabled();
bool kernel_interrupt_remapping_uncertain();
na_status_t kernel_map_dma_page(device_domain &domain, u64 iova, u64 physical_address, dma_access access,
                                bool &outcome_uncertain);
na_status_t kernel_unmap_dma_page(device_domain &domain, u64 iova, bool &outcome_uncertain);
na_status_t kernel_destroy_dma_domain(device_domain &domain, bool &outcome_uncertain);
bool kernel_create_device_domain(u32 unit_index, u16 segment, u8 bus, u8 device, u8 function, device_domain &domain,
                                 error &failure);
bool kernel_default_deny_ready();
u32 kernel_unit_count();
u32 kernel_active_unit_count();
u32 kernel_queued_invalidation_unit_count();
u32 kernel_interrupt_remapping_table_ready_unit_count();
bool kernel_configure_fault_event_interrupt(u32 unit_index, u32 destination_apic_id, u8 vector, error &failure);
bool kernel_fault_event_interrupt_enabled(u32 unit_index);
bool kernel_fault_event_interrupt_uncertain(u32 unit_index);
bool kernel_drain_fault_records(u32 unit_index, fault_record *records, u32 capacity, fault_drain_result &result,
                                error &failure);

/// A device-scoped second-level translation context. A caller must retain this
/// value until the device context has been removed and every mapping revoked.
struct device_domain
{
    second_level_table page_table{};
    u64 mapped_page_count = 0;
    u16 segment = 0;
    u16 domain_id = 0;
    u8 bus = 0;
    u8 device = 0;
    u8 function = 0;
    u8 unit_index = 0;
    bool valid = false;
    bool context_installed = false;
    bool state_uncertain = false;
};

/// Hardware-owned root-table storage must remain alive after RTADDR is written,
/// including when a later register handshake times out and its completion is
/// uncertain. Translation is considered active only after GSTS.TES is observed.
struct unit_state
{
    u64 root_table_physical = 0;
    void *root_table_virtual = nullptr;
    u64 queued_invalidation_queue_physical = 0;
    void *queued_invalidation_queue_virtual = nullptr;
    u64 queued_invalidation_status_physical = 0;
    void *queued_invalidation_status_virtual = nullptr;
    u64 interrupt_remapping_table_physical = 0;
    void *interrupt_remapping_table_virtual = nullptr;
    u32 global_command = 0;
    u32 queued_invalidation_tail_offset = 0;
    u32 queued_invalidation_status_sequence = 0;
    u16 next_interrupt_remapping_index = 0;
    u64 extended_capability = 0;
    u64 register_length = 0;
    u32 fault_record_offset = 0;
    u16 fault_record_count = 0;
    bool root_table_retained = false;
    bool root_pointer_installed = false;
    bool translation_enabled = false;
    bool queued_invalidation_enabled = false;
    bool queued_invalidation_state_uncertain = false;
    bool queued_invalidation_storage_retained = false;
    error queued_invalidation_failure = error::none;
    bool interrupt_remapping_table_retained = false;
    bool interrupt_remapping_table_ready = false;
    bool interrupt_remapping_enabled = false;
    bool interrupt_remapping_enable_uncertain = false;
    bool interrupt_remapping_state_uncertain = false;
    error interrupt_remapping_failure = error::none;
    bool state_uncertain = false;
    bool fault_event_interrupt_enabled = false;
    bool fault_event_interrupt_state_uncertain = false;
    bool coherent = false;
    bool include_all = false;
    u16 segment = 0;
    u8 host_address_width = 0;
    u8 maximum_guest_address_width = 0;
    u8 supported_adjusted_guest_address_widths = 0;
    u64 register_base = 0;
    u32 iotlb_register_offset = 0;
    register_ops operations{};
    void *context_tables[256]{};
    u64 context_table_physical[256]{};
};

inline constexpr u32 max_hardware_units = 32;
inline constexpr u32 max_fault_record_count = 256;

class controller
{
  public:
    bool initialize(const firmware_inventory &snapshot, const register_ops &operations, error &failure);
    bool default_deny_ready() const { return default_deny_ready_; }
    u32 unit_count() const { return unit_count_; }
    u32 active_unit_count() const { return active_unit_count_; }
    const unit_state *unit(u32 index) const { return index < unit_count_ ? &units_[index] : nullptr; }
    u32 queued_invalidation_unit_count() const { return queued_invalidation_unit_count_; }
    u32 interrupt_remapping_table_ready_unit_count() const { return interrupt_remapping_table_ready_unit_count_; }
    bool interrupt_remapping_ready() const;
    bool enable_interrupt_remapping(error &failure);
    bool interrupt_remapping_enabled();
    bool interrupt_remapping_activation_uncertain() const;
    bool install_ioapic_irte_for_unit(u32 unit_index, u16 segment, u16 source_id, u32 destination_apic_id, u8 vector,
                                      bool level_triggered, u16 &entry_index, error &failure);
    bool install_pci_msi_irte_for_unit(u32 unit_index, u16 segment, u16 requester_id, u32 destination_apic_id,
                                       u8 vector, u16 &entry_index, error &failure, bool &outcome_uncertain);
    bool disable_interrupt_remapping_entry_for_unit(u32 unit_index, u16 entry_index, error &failure);
    /// Calls must be serialized by the owning kernel lock. The device's BME
    /// must remain clear until installation succeeds. Domain IDs are unique
    /// and are not reused during this boot, even after context removal.
    bool install_device_context(u16 segment, u8 bus, u8 device, u8 function, u64 second_level_root,
                                u8 second_level_address_width, bool page_table_coherent, u16 &domain_id,
                                error &failure);
    bool install_device_context(u32 unit_index, u16 segment, u8 bus, u8 device, u8 function, u64 second_level_root,
                                u8 second_level_address_width, bool page_table_coherent, u16 &domain_id,
                                error &failure);
    /// The caller must clear and verify BME before removing the context. This
    /// operation invalidates translations but does not release DMA frames.
    bool remove_device_context(u16 segment, u8 bus, u8 device, u8 function, u16 domain_id, error &failure);
    bool remove_device_context(u32 unit_index, u16 segment, u8 bus, u8 device, u8 function, u16 domain_id,
                               error &failure);
    /// These domain operations require the caller to serialize access through
    /// the kernel PCI/IOMMU owner lock. Device bus mastering must remain off
    /// until creation succeeds; destroy requires it to be off and all pages
    /// unmapped.
    bool create_device_domain(u16 segment, u8 bus, u8 device, u8 function, device_domain &domain, error &failure);
    bool create_device_domain(u32 unit_index, u16 segment, u8 bus, u8 device, u8 function, device_domain &domain,
                              error &failure);
    error map_device_page(device_domain &domain, u64 iova, u64 physical_address, dma_access access);
    error unmap_device_page(device_domain &domain, u64 iova);
    bool destroy_device_domain(device_domain &domain, error &failure);
    bool drain_fault_records_for_unit(u32 unit_index, fault_record *records, u32 capacity, fault_drain_result &result,
                                      error &failure);
    bool configure_fault_event_interrupt_for_unit(u32 unit_index, u32 destination_apic_id, u8 vector, error &failure);
    bool fault_event_interrupt_enabled_for_unit(u32 unit_index) const;
    bool fault_event_interrupt_state_uncertain_for_unit(u32 unit_index) const;

  private:
    unit_state units_[max_hardware_units]{};
    u32 unit_count_ = 0;
    u32 active_unit_count_ = 0;
    u32 queued_invalidation_unit_count_ = 0;
    u32 interrupt_remapping_table_ready_unit_count_ = 0;
    u32 next_domain_id_ = 2;
    bool attempted_ = false;
    bool default_deny_ready_ = false;
};

interrupt_route_token encode_route_token(u32 unit_index, u16 entry_index, u16 segment, u16 requester_id,
                                         u64 generation) noexcept;
bool decode_route_token(const interrupt_route_token &token, u64 expected_generation, u32 &unit_index, u16 &entry_index,
                        u16 &segment, u16 &requester_id) noexcept;
/// Provider-level route operations. The kernel adapter must serialize calls
/// that touch controller state with its owning provider lock.
bool install_pci_msi_route(controller &backend, const device_source_ref &source, u64 firmware_generation,
                           u32 destination_apic_id, u8 vector, interrupt_route_token &token, msi_message &message,
                           bool &outcome_uncertain, error &failure);
bool install_ioapic_route(controller &backend, const device_source_ref &source, u64 firmware_generation,
                          u32 destination_apic_id, u8 vector, bool low_polarity, bool level_triggered,
                          ioapic_route_encoding &route, error &failure);
bool route_matches_pci(const interrupt_route_token &token, const device_source_ref &source) noexcept;
bool retire_route(controller &backend, const interrupt_route_token &token, u64 firmware_generation, error &failure);
na_status_t map_dma_page(controller &backend, device_domain &domain, u64 iova, u64 physical_address, dma_access access,
                         bool &outcome_uncertain);
na_status_t unmap_dma_page(controller &backend, device_domain &domain, u64 iova, bool &outcome_uncertain);
na_status_t destroy_dma_domain(controller &backend, device_domain &domain, bool &outcome_uncertain);
resolution_result resolve_pci_source(const firmware_inventory &snapshot, const pci_function_location &function,
                                     u64 generation, bridge_info_reader read_bridge_info, void *context,
                                     device_source_ref &source);
resolution_result resolve_ioapic_source(const firmware_inventory &snapshot, u8 enumeration_id, u32 madt_match_count,
                                        u64 generation, bridge_info_reader read_bridge_info, void *context,
                                        device_source_ref &source);

/// Install an empty VT-d root table, invalidate stale translation caches, and
/// enable translation. Every context remains not-present, so DMA is denied
/// until a later DmaDomain implementation installs an authorized context.
bool enable_default_deny(const dmar_hardware_unit &unit, u8 host_address_width, const register_ops &operations,
                         unit_state &state, error &failure);

/// Copy and acknowledge the unit's primary fault records. The output capacity
/// must cover the advertised hardware record table; callers must serialize
/// register access through the owning controller lock.
bool drain_fault_records(unit_state &state, fault_record *records, u32 capacity, fault_drain_result &result,
                         error &failure);

/// Program the xAPIC fault-event message while delivery is still masked, then
/// unmask it. The caller must register a live IRQ handler before invoking this.
bool configure_fault_event_interrupt(unit_state &state, u32 destination_apic_id, u8 vector, error &failure);

/// Enable the QI ring for a translated unit. This does not enable interrupt
/// remapping or change any interrupt source routing. The owner must serialize
/// this operation with other VT-d register and queue access.
bool initialize_queued_invalidation(unit_state &state, error &failure);

/// Install an all-invalid IR table and invalidate the interrupt-entry cache.
/// This deliberately leaves IRE clear so existing IOAPIC routes remain active.
bool prepare_interrupt_remapping(unit_state &state, error &failure);

/// Enable IRE for a unit only after the caller has converted or masked every
/// active interrupt source handled by that unit. Failures with uncertain
/// completion require keeping all such sources masked.
bool enable_interrupt_remapping(unit_state &state, error &failure);

/// Allocate and publish a source-validated PCI MSI IRTE. The owning PCI path
/// must not enable device MSI until interrupt remapping is globally enabled
/// and all active IOAPIC sources in that unit are remapped or masked. Indices
/// are never reused during this boot, including after a failed operation.
/// outcome_uncertain is set only when this attempt leaves IR-table or
/// invalidation state uncertain; the caller must quarantine its vector/handler.
bool install_pci_msi_irte(unit_state &state, u16 requester_id, u32 destination_apic_id, u8 vector,
                          bool extended_interrupt_mode, u16 &index, error &failure, bool &outcome_uncertain);

/// Allocate and publish a source-validated fixed-delivery IOAPIC IRTE. The
/// supplied source ID must come from this IOAPIC's validated DMAR scope.
bool install_ioapic_irte(unit_state &state, u16 source_id, u32 destination_apic_id, u8 vector, bool level_triggered,
                         u16 &index, error &failure);

/// Clear an installed MSI IRTE and complete a global IEC invalidation. The
/// caller must first mask/disable the source and drain its interrupt handler.
/// The IRTE index remains retired and is not reused during this boot.
bool disable_interrupt_remapping_entry(unit_state &state, u16 index, error &failure);

} // namespace iommu::intel_vtd
