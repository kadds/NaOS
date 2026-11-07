#pragma once
#include "freelibcxx/hash_map.hpp"
#include "freelibcxx/optional.hpp"
#include "freelibcxx/vector.hpp"
#include "kernel/common.hpp"
#include "kernel/iommu/backend.hpp"
#include "kernel/types.hpp"
#include "naos/abi.h"

template <typename T> class handle_t;
namespace firmware
{
class root;
}
namespace acpi
{
class fixed_hardware;
class resource_authority;
} // namespace acpi

namespace arch::ACPI
{
void init();

bool has_init();

void enable();

/// Start provider-backed firmware services whose runtime dependencies become
/// available after ACPI inventory initialization.
void start_deferred_services();

struct io_apic_info
{
    phy_addr_t address{};
    u32 gsi_base = 0;
    u8 enumeration_id = 0;
};

freelibcxx::vector<io_apic_info> get_io_apic_list();

/// Resolve an IOAPIC's firmware-declared interrupt source to its exact PCI
/// requester. The function only reads validated ECAM bridge configuration and
/// fails closed.
struct ioapic_source_identity
{
    iommu::device_source_ref source{};
};

bool resolve_ioapic_interrupt_source(u8 enumeration_id, ioapic_source_identity &identity);
bool install_ioapic_interrupt_route(u8 enumeration_id, u32 destination_apic_id, u8 vector, bool low_polarity,
                                    bool level_triggered, iommu::ioapic_route_encoding &route);
bool disable_ioapic_interrupt_route(const iommu::interrupt_route_token &route);
bool interrupt_remapping_ready();
bool enable_interrupt_remapping();
bool interrupt_remapping_enabled();
bool interrupt_remapping_activation_uncertain();

struct lapic_info
{
    u32 processor_id;
    u32 apic_id;
    bool enabled;
};

freelibcxx::vector<lapic_info> get_local_apic_list();

struct override_irq
{
    u8 bus;
    u8 irq_source;
    u16 flags;
    u32 gsi;
};

freelibcxx::vector<override_irq> get_override_irq_list();

struct pm_info
{
    u32 block_base;
    void *xblock_base;
    bool use_io;
    bool bit32mode;
    u32 event_a;
    u32 xevent_a;
};

freelibcxx::optional<pm_info> get_acpipm_base();
bool is_8042_device_exists();

phy_addr_t get_local_apic_base();

freelibcxx::optional<phy_addr_t> get_hpet_base();

struct numa_info
{
    u32 domain;
    u32 apic_id;
    bool enabled;
};

freelibcxx::hash_map<u32, numa_info> get_numa_info();

handle_t<firmware::root> firmware_root();
handle_t<acpi::fixed_hardware> fixed_hardware();
handle_t<acpi::resource_authority> resource_authority();

/// Close a kernel-owned physical resource lease after the last capability
/// and VM mapping have gone away.
bool revoke_resource(u64 resource_id, u64 owner);
bool revoke_resource_kernel(u64 resource_id);
bool begin_map_resource_kernel(u64 resource_id, u64 base, u64 length);
bool end_map_resource_kernel(u64 resource_id);
bool unmap_resource_kernel(u64 resource_id);
bool revoke_owner_resources(process_id owner);
bool revoke_thread_resources(process_id owner, u64 thread_id);
/// Revoke ACPI state owned by an exiting process or thread.
void on_process_exit(process_id owner);
void on_thread_exit(process_id owner, u64 thread_id);

/// Validate that a physical range belongs to firmware/device-reserved memory,
/// not normal RAM. This is an admission helper for the scoped ACPI resource authority;
/// it does not expose a physical mapping ABI.
bool is_device_range(u64 address, u64 length);

} // namespace arch::ACPI
