#pragma once

#include "kernel/arch/acpi/boot_tables.hpp"
#include "kernel/iommu/amd_vi_command.hpp"
#include "kernel/iommu/amd_vi_dte.hpp"
#include "kernel/iommu/amd_vi_interrupt.hpp"
#include "kernel/iommu/amd_vi_page_table.hpp"
#include "kernel/iommu/backend.hpp"
#include "kernel/iommu/firmware_inventory.hpp"
#include "kernel/types.hpp"
#include "naos/abi.h"

namespace lock
{
struct spinlock_t;
}

namespace iommu::amd_vi
{
enum class controller_error : u8
{
    none,
    invalid_inventory,
    unsafe_activation,
    reserved_memory_requires_domain,
    too_many_units,
    invalid_register_aperture,
    translation_pre_enabled,
    allocation_failed,
    invalid_device_table,
    cache_maintenance_unavailable,
    register_io_failed,
    hardware_timeout,
    invalid_command_pointer,
    invalid_event_pointer,
    event_log_overflow,
    domain_id_exhausted,
    device_domain_conflict,
    device_not_quiesced,
    invalid_page_table,
    page_table_operation_failed,
    interrupt_remapping_unsupported,
    interrupt_table_exhausted,
    invalid_interrupt_route,
};

na_status_t to_status(controller_error failure) noexcept;

bool physical_address_width_from_iommu_capability(const amd_ivhd_record &record, pci_config_read32_callback read_config,
                                                  void *context, u8 &physical_address_width);

struct event_record
{
    u64 address = 0;
    u64 raw[2]{};
    u16 device_id = 0;
    u16 domain_or_pasid = 0;
    u8 event_code = 0;
    bool is_translation = false;
    bool is_present = false;
    bool is_interrupt = false;
    bool is_write = false;
};

struct event_drain_result
{
    u32 count = 0;
    bool more = false;
    bool overflow = false;
};

struct controller_ops
{
    bool (*read64)(void *context, u64 register_base, u32 offset, u64 &value) = nullptr;
    bool (*write64)(void *context, u64 register_base, u32 offset, u64 value) = nullptr;
    u64 (*register_aperture_length)(void *context, u64 register_base) = nullptr;
    bool (*allocate_contiguous)(void *context, u32 bytes, void *&virtual_address, u64 &physical_address) = nullptr;
    void (*release_contiguous)(void *context, void *virtual_address, u32 bytes) = nullptr;
    bool (*flush_range)(void *context, void *virtual_address, u32 bytes) = nullptr;
    bool (*pci_bus_master_enabled)(void *context, u16 segment, u16 device_id, bool &enabled) = nullptr;
    bool (*pci_requester_present)(void *context, u16 segment, u16 device_id, bool &present) = nullptr;
    pci_config_read32_callback read_pci_config32 = nullptr;
    void *context = nullptr;
};

/// Build the kernel register and allocation access surface for selected IVHD
/// units. PCI quiescence callbacks remain supplied by the kernel PCI adapter.
class controller;
struct device_domain;
struct activation_preconditions;
page_table_ops kernel_page_table_ops() noexcept;
provider_initialization_report kernel_initialize(const firmware_inventory &inventory,
                                                 const pci_requester_platform_ops &platform_operations,
                                                 bool pci_bus_mastering_quiesced, const u8 *ioapic_enumeration_ids,
                                                 u32 ioapic_source_count, ioapic_match_counter count_ioapic_matches,
                                                 void *ioapic_context);
provider_ops kernel_provider_ops(dma_domain_creator create_dma_domain);
resolution_result kernel_resolve_pci_source(const firmware_inventory &inventory, const pci_function_location &function,
                                            u64 generation, device_source_ref &source);
resolution_result kernel_resolve_ioapic_source(const firmware_inventory &inventory, u8 enumeration_id,
                                               u32 madt_match_count, u64 generation, u8 &dte_setting,
                                               device_source_ref &source);
bool kernel_install_pci_msi_route(const device_source_ref &source, u64 firmware_generation, u32 destination_apic_id,
                                  u8 vector, interrupt_route_token &token, msi_message &message,
                                  bool &outcome_uncertain, controller_error &failure);
bool kernel_install_ioapic_route(const device_source_ref &source, u64 firmware_generation, u32 destination_apic_id,
                                 u8 vector, bool low_polarity, bool level_triggered, ioapic_route_encoding &route,
                                 controller_error &failure);
bool kernel_route_matches_pci(const interrupt_route_token &token, const device_source_ref &source) noexcept;
bool kernel_retire_route(const interrupt_route_token &token, u64 firmware_generation, controller_error &failure);
na_status_t kernel_map_dma_page(device_domain &domain, u64 iova, u64 physical_address, dma_access access,
                                bool &outcome_uncertain);
na_status_t kernel_unmap_dma_page(device_domain &domain, u64 iova, bool &outcome_uncertain);
na_status_t kernel_destroy_dma_domain(device_domain &domain, bool &outcome_uncertain);

struct ioapic_interrupt_source
{
    u32 ivhd_record_index = ~u32{0};
    u16 segment = 0;
    u16 device_id = 0;
    u8 ioapic_id = 0;
    u8 dte_setting = 0;
};

struct activation_preconditions
{
    bool pci_bus_mastering_quiesced = false;
    bool interrupt_sources_safe = false;
    const ioapic_interrupt_source *ioapic_sources = nullptr;
    u32 ioapic_source_count = 0;
};

struct unit_state
{
    void *device_table_virtual = nullptr;
    void *command_buffer_virtual = nullptr;
    void *event_log_virtual = nullptr;
    u64 device_table_physical = 0;
    u64 command_buffer_physical = 0;
    u64 event_log_physical = 0;
    u64 register_base = 0;
    u64 extended_features = 0;
    u32 ivhd_record_index = ~u32{0};
    u32 register_aperture_bytes = 0;
    u32 command_tail_offset = 0;
    u32 event_head_offset = 0;
    u16 segment = 0;
    u8 ivhd_type = 0;
    u8 ivhd_flags = 0;
    bool invalidate_all_supported = false;
    bool coherent = false;
    bool state_touched = false;
    bool state_uncertain = false;
    bool event_log_overflowed = false;
    bool default_deny_active = false;
    bool interrupt_remapping_supported = false;
    bool buffers_retained = false;
};

struct device_domain
{
    page_table translations{};
    u32 unit_index = ~u32{0};
    u16 device_id = 0;
    u16 domain_id = 0;
    bool valid = false;
    bool state_uncertain = false;
};

struct interrupt_route
{
    u32 unit_index = ~u32{0};
    u16 device_id = 0;
    u16 table_index = 0xffff;
    u32 cookie = 0;
    bool ioapic_source = false;

    constexpr bool valid() const noexcept
    {
        return unit_index != ~u32{0} && table_index < maximum_basic_interrupt_table_entries && cookie != 0;
    }
};

inline constexpr u32 max_hardware_units = 32;
inline constexpr u32 max_ioapic_interrupt_sources = max_ioapic_sources;
inline constexpr u32 max_interrupt_tables = 256;
inline constexpr u32 interrupt_table_bytes = 4096;
inline constexpr u32 interrupt_route_bitmap_words = maximum_basic_interrupt_table_entries / 64;
inline constexpr u32 command_buffer_bytes = 4096;
inline constexpr u32 event_log_bytes = 4096;
inline constexpr u32 command_buffer_slots = command_buffer_bytes / sizeof(command_entry);
inline constexpr u32 maximum_commands_per_batch = command_buffer_slots - 2;

/// Initializes selected IVHD units with a full valid default-deny DTE table,
/// command processing, and a bounded event log. Reserved-memory IVMD entries
/// remain unsupported and prevent activation. Hardware-touched buffers remain
/// retained after any ambiguous register/command outcome. The caller must
/// first quiesce PCI bus mastering and ensure existing interrupt sources are
/// safe. PCI MSI and IVRS-authorized IOAPIC routes use DTE-associated
/// per-requester tables; the platform interrupt adapter owns the IOAPIC RTE.
class controller
{
  public:
    bool initialize(const firmware_inventory &inventory, const controller_ops &operations,
                    activation_preconditions preconditions, controller_error &failure);
    bool default_deny_ready() const { return default_deny_ready_; }
    u32 unit_count() const { return unit_count_; }
    u32 active_unit_count() const { return active_unit_count_; }
    u32 interrupt_remapping_supported_unit_count() const
    {
        u32 supported = 0;
        for (u32 index = 0; index < unit_count_; ++index)
            supported += units_[index].interrupt_remapping_supported;
        return supported;
    }
    bool state_uncertain() const { return state_uncertain_; }
    void quarantine_unit(u32 index);
    const unit_state *unit(u32 index) const { return index < unit_count_ ? &units_[index] : nullptr; }
    bool find_unit_for_ivhd_record(u32 record_index, u32 &unit_index) const;
    bool drain_events(u32 unit_index, event_record *records, u32 capacity, event_drain_result &result,
                      controller_error &failure);
    bool create_device_domain(u32 unit_index, u16 device_id, u8 physical_address_width,
                              const page_table_ops &page_operations, device_domain &domain, controller_error &failure);
    /// The kernel DMA-domain owner must keep bus mastering disabled while it
    /// publishes or removes page mappings and retain backing memory until the
    /// completion wait succeeds.
    bool map_device_page(device_domain &domain, u64 iova, u64 physical_address, dma_access access,
                         controller_error &failure);
    bool unmap_device_page(device_domain &domain, u64 iova, controller_error &failure);
    bool destroy_device_domain(device_domain &domain, controller_error &failure);
    bool install_pci_interrupt_route(u32 unit_index, u16 device_id, u32 destination_apic_id, u8 vector,
                                     interrupt_delivery_mode delivery, bool suppress_fault_event,
                                     interrupt_route &route, controller_error &failure);
    bool install_ioapic_interrupt_route(u32 unit_index, u16 device_id, u32 destination_apic_id, u8 vector,
                                        interrupt_route &route, controller_error &failure);
    bool retire_interrupt_route(const interrupt_route &route, controller_error &failure);
    bool pci_interrupt_route_matches(const interrupt_route &route, u32 unit_index, u16 device_id) const;

  private:
    struct interrupt_table_state
    {
        void *virtual_address = nullptr;
        u64 physical_address = 0;
        u64 allocated_routes[interrupt_route_bitmap_words]{};
        u32 route_cookies[maximum_basic_interrupt_table_entries]{};
        u32 unit_index = ~u32{0};
        u16 device_id = 0;
        u16 route_count = 0;
        bool active = false;
        bool state_uncertain = false;
        bool ioapic_source = false;
    };

    struct ioapic_source_state
    {
        u32 unit_index = ~u32{0};
        u16 device_id = 0;
        u8 ioapic_id = 0;
        bool active = false;
    };

    static void reset_interrupt_table_state(interrupt_table_state &table);
    bool install_interrupt_route(u32 unit_index, u16 device_id, bool ioapic_source, u16 requested_table_index,
                                 u32 destination_apic_id, u8 vector, interrupt_delivery_mode delivery,
                                 bool suppress_fault_event, interrupt_route &route, controller_error &failure);
    bool is_authorized_ioapic_source(u32 unit_index, u16 device_id) const;

    unit_state units_[max_hardware_units]{};
    interrupt_table_state interrupt_tables_[max_interrupt_tables]{};
    ioapic_source_state ioapic_sources_[max_ioapic_interrupt_sources]{};
    command_entry command_scratch_[maximum_commands_per_batch]{};
    u64 allocated_domain_ids_[max_hardware_units][maximum_device_id_count / 64]{};
    u16 next_domain_id_[max_hardware_units]{};
    controller_ops operations_{};
    u32 unit_count_ = 0;
    u32 active_unit_count_ = 0;
    u32 ioapic_source_count_ = 0;
    u32 next_interrupt_route_cookie_ = 1;
    bool attempted_ = false;
    bool hardware_touched_ = false;
    bool default_deny_ready_ = false;
    bool state_uncertain_ = false;
};

interrupt_route_token encode_route_token(const interrupt_route &route, u64 generation) noexcept;
bool decode_route_token(const interrupt_route_token &token, u64 expected_generation, interrupt_route &route) noexcept;

/// Provider-level route operations. The kernel adapter must serialize calls
/// that touch controller state with its owning provider lock.
bool install_pci_msi_route(controller &backend, const device_source_ref &source, u64 firmware_generation,
                           u32 destination_apic_id, u8 vector, interrupt_route_token &token, msi_message &message,
                           bool &outcome_uncertain, controller_error &failure);
bool install_ioapic_route(controller &backend, const device_source_ref &source, u64 firmware_generation,
                          u32 destination_apic_id, u8 vector, bool low_polarity, bool level_triggered,
                          ioapic_route_encoding &route, controller_error &failure);
bool route_matches_pci(const controller &backend, const interrupt_route_token &token,
                       const device_source_ref &source) noexcept;
bool retire_route(controller &backend, const interrupt_route_token &token, u64 firmware_generation,
                  controller_error &failure);
na_status_t map_dma_page(controller &backend, device_domain &domain, u64 iova, u64 physical_address, dma_access access,
                         bool &outcome_uncertain);
na_status_t unmap_dma_page(controller &backend, device_domain &domain, u64 iova, bool &outcome_uncertain);
na_status_t destroy_dma_domain(controller &backend, device_domain &domain, bool &outcome_uncertain);
bool kernel_create_device_domain(u32 unit_index, u16 requester_id, u8 physical_address_width,
                                 const page_table_ops &page_operations, device_domain &domain,
                                 controller_error &failure);
bool kernel_default_deny_ready();
bool kernel_state_uncertain();
bool kernel_unit_state_uncertain(u32 unit_index);
u32 kernel_unit_count();
u32 kernel_active_unit_count();
u32 kernel_interrupt_remapping_supported_unit_count();
bool kernel_quarantine_unit(u32 unit_index);
bool kernel_drain_events(u32 unit_index, event_record *records, u32 capacity, event_drain_result &result,
                         controller_error &failure);

resolution_result resolve_pci_source(const firmware_inventory &inventory, const controller &backend,
                                     const pci_function_location &function, u64 generation, device_source_ref &source);
resolution_result resolve_ioapic_requester(const firmware_inventory &inventory, u8 enumeration_id, u32 madt_match_count,
                                           u32 &ivhd_record_index, u16 &segment, u16 &requester_id, u8 &dte_setting);
resolution_result resolve_ioapic_source(const firmware_inventory &inventory, const controller &backend,
                                        u8 enumeration_id, u32 madt_match_count, u64 generation, u8 &dte_setting,
                                        device_source_ref &source);
} // namespace iommu::amd_vi
