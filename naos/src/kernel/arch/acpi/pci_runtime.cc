#include "pci_runtime.hpp"
#include "freelibcxx/hash_map.hpp"
#include "freelibcxx/optional.hpp"
#include "freelibcxx/vector.hpp"
#include "kernel/acpi/resource_authority.hpp"
#include "kernel/acpi/fixed_hardware.hpp"
#include "kernel/arch/acpi/acpi.hpp"
#include "kernel/arch/acpi/boot_tables.hpp"
#include "kernel/arch/acpi/facs_reservation.hpp"
#include "kernel/arch/acpi/physical_access.hpp"
#include "kernel/arch/cpu.hpp"
#include "kernel/arch/cpu_info.hpp"
#include "kernel/arch/io_apic.hpp"
#include "kernel/arch/local_apic.hpp"
#include "kernel/arch/paging.hpp"
#include "kernel/firmware_root.hpp"
#include "kernel/iommu/fault_dispatch.hpp"
#include "kernel/irq.hpp"
#include "kernel/kernel.hpp"
#include "kernel/log.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/mm/vm.hpp"
#include "kernel/pci/discovery_fabric.hpp"
#include "kernel/pci/dma_domain.hpp"
#include "kernel/pci/dma_fault_source.hpp"
#include "kernel/pci/dma_quiesce.hpp"
#include "kernel/pci/msi_config.hpp"
#include "kernel/pci/msi_route.hpp"
#include "kernel/pci/msi_route_lifecycle.hpp"
#include "kernel/pci/msix_table_lease.hpp"
#include "kernel/pci/platform.hpp"
#include "kernel/pci/topology_lifecycle.hpp"
#include "kernel/physical_resource.hpp"
#include "kernel/smp.hpp"
#include "kernel/ucontext.hpp"
#include <atomic>
#include <new>

KLOG_MODULE(acpi);

namespace arch::ACPI
{
namespace pci_runtime
{
using pci_msi_route = pci::msi_route;

static bool install_pci_msi_route(const pci::function_identity &identity, u8 vector, pci_msi_route &route,
                                  bool &outcome_uncertain);
static bool retire_pci_msi_route(const pci_msi_route &route);
static bool prepare_pci_msi_route(const pci::function &function, u8 vector, pci_msi_route &route,
                                  pci::interrupt_route_config_snapshot &previous, pci::msi_config_error &failure,
                                  bool &outcome_uncertain);
static bool set_pci_msi_route_enabled(const pci::function_identity &identity, const pci_msi_route &route,
                                      const pci::interrupt_route_config_snapshot &previous, bool enabled,
                                      pci::msi_config_error &failure, bool &outcome_uncertain);
static bool restore_pci_msi_route_configuration(const pci::function_identity &identity, const pci_msi_route &route,
                                                const pci::interrupt_route_config_snapshot &previous,
                                                pci::msi_config_error &failure, bool &outcome_uncertain);
static pci::msi_route_teardown_result shutdown_pci_msi_route(const pci::function_identity &identity,
                                                             const pci_msi_route &route,
                                                             const pci::interrupt_route_config_snapshot &previous);
static pci::dma_operation_result create_pci_dma_domain(const pci::function_identity &identity, process_id owner,
                                                       handle_t<pci::dma_domain> &result,
                                                       handle_t<pci::dma_fault_source> &fault_source);
static bool read_pci_config(const pci::root_binding &binding, const pci::function_identity &identity, u16 offset,
                            u8 width, u32 &value);
static handle_t<pci::discovery_fabric> get_pci_discovery_fabric();

namespace
{
using acpi_boot::table_record;
using detail::firmware_snapshot;
using detail::hardware_ledger;
using detail::hardware_resource_root_authority;
using detail::hardware_resource_root_object;

pci::firmware_inventory pci_inventory{};
handle_t<pci::discovery_fabric> *pci_discovery_object = nullptr;
bool pci_runtime_active = false;
struct provider_fault_irq_slot
{
    iommu::provider_kind provider = iommu::provider_kind::none;
    u32 unit_index = 0;
    std::atomic_bool pending{false};
    irq::msi_vector_lease vector;
    irq::registration registration;

    void schedule_soft_drain() noexcept
    {
        pending.store(true, std::memory_order_release);
        irq::raise_soft_irq(irq::soft_vector::iommu_fault);
    }

    irq::request_result on_interrupt(const irq::interrupt_info *, u64) noexcept
    {
        schedule_soft_drain();
        return irq::request_result::ok;
    }
};
struct provider_fault_irq_state
{
    provider_fault_irq_slot *slots = nullptr;
    u32 slot_count = 0;
    bool setup_attempted = false;
};
provider_fault_irq_state provider_fault_irq_states[3]{};
irq::registration *provider_fault_soft_irq_registration = nullptr;
bool provider_fault_soft_irq_setup_attempted = false;
struct pci_dma_domain_backend_state
{
    pci::function_identity identity{};
    iommu::provider_kind provider = iommu::provider_kind::none;
    u32 unit_index = ~u32{0};
    u16 requester_id = 0;
    void *backend_state = nullptr;
};
lock::spinlock_t pci_dma_config_lock;
bool pci_dma_quiescence_verified = false;
u64 pci_dma_config_virtual_base = 0;
iommu::provider_initialization_report provider_initialization_reports[3]{};
const pci::platform_operations pci_platform_operations{
    get_pci_discovery_fabric, read_pci_config,           create_pci_dma_domain,
    prepare_pci_msi_route,    set_pci_msi_route_enabled, shutdown_pci_msi_route,
};

bool pci_requester_bus_master_enabled(void *context, u16 segment, u16 requester_id, bool &enabled);
bool pci_requester_present(void *context, u16 segment, u16 requester_id, bool &present);
bool report_provider_fault_records(iommu::provider_kind provider, u32 unit_index, bool log_records);
na_status_t set_pci_dma_bus_master(const pci::function_identity &identity, bool enabled,
                                   const pci::bus_master_enable_authority *authority, bool &outcome_uncertain);
iommu::dma_domain_create_result create_pci_dma_domain(void *context, const iommu::device_source_ref &source,
                                                      const pci::function_identity &identity, process_id owner,
                                                      handle_t<pci::dma_domain> &domain,
                                                      handle_t<pci::dma_fault_source> &fault_source);

iommu::fault_delivery_mode provider_fault_delivery(iommu::provider_kind provider)
{
    const u32 provider_index = static_cast<u32>(provider);
    if (provider_index >= sizeof(provider_initialization_reports) / sizeof(provider_initialization_reports[0]))
        return iommu::fault_delivery_mode::none;
    return provider_initialization_reports[provider_index].fault_delivery;
}

void schedule_provider_fault_drain(iommu::provider_kind provider, u32 unit_index) noexcept
{
    const u32 provider_index = static_cast<u32>(provider);
    if (provider_index >= sizeof(provider_fault_irq_states) / sizeof(provider_fault_irq_states[0]))
        return;
    auto &state = provider_fault_irq_states[provider_index];
    for (u32 index = 0; index < state.slot_count; ++index)
    {
        if (state.slots[index].unit_index == unit_index)
        {
            state.slots[index].schedule_soft_drain();
            return;
        }
    }
}

u16 pci_requester_id(const pci::function_identity &identity)
{
    return static_cast<u16>((static_cast<u16>(identity.bus) << 8) | (static_cast<u16>(identity.device) << 3) |
                            static_cast<u16>(identity.function));
}

bool pci_dma_fault_source_quarantined(const pci_dma_domain_backend_state &state)
{
    return iommu::dma_fault_source_quarantined(state.provider, state.unit_index, state.identity, state.requester_id);
}

na_status_t map_pci_dma_domain_page(void *, void *opaque_state, u64 iova, u64 physical_address,
                                    naos::data_plane::dma_direction direction, bool &outcome_uncertain)
{
    auto *state = static_cast<pci_dma_domain_backend_state *>(opaque_state);
    if (state == nullptr || state->backend_state == nullptr)
        return NA_STATUS_INVALID_HANDLE;
    return iommu::map_provider_dma_domain_page(state->provider, state->backend_state, iova, physical_address, direction,
                                               outcome_uncertain);
}

na_status_t unmap_pci_dma_domain_page(void *, void *opaque_state, u64 iova, bool &outcome_uncertain)
{
    auto *state = static_cast<pci_dma_domain_backend_state *>(opaque_state);
    if (state == nullptr || state->backend_state == nullptr)
        return NA_STATUS_INVALID_HANDLE;
    return iommu::unmap_provider_dma_domain_page(state->provider, state->backend_state, iova, outcome_uncertain);
}

na_status_t set_pci_dma_domain_bus_master(void *, void *opaque_state, bool enabled,
                                          const pci::bus_master_enable_authority *authority, bool &outcome_uncertain)
{
    outcome_uncertain = false;
    auto *state = static_cast<pci_dma_domain_backend_state *>(opaque_state);
    if (state == nullptr || state->backend_state == nullptr)
        return NA_STATUS_INVALID_HANDLE;
    if (enabled && !iommu::provider_dma_domain_ready_for_bus_master(state->provider))
        return NA_STATUS_NOT_SUPPORTED;
    if (enabled && iommu::provider_unit_state_uncertain(state->provider, state->unit_index))
    {
        outcome_uncertain = true;
        return NA_STATUS_IO_ERROR;
    }
    if (enabled && pci_dma_fault_source_quarantined(*state))
        return NA_STATUS_ACCESS_DENIED;

    const na_status_t status = set_pci_dma_bus_master(state->identity, enabled, authority, outcome_uncertain);
    if (status == NA_STATUS_OK && !enabled)
    {
        const bool interrupt_delivery =
            provider_fault_delivery(state->provider) == iommu::fault_delivery_mode::interrupt;
        if (report_provider_fault_records(state->provider, state->unit_index, interrupt_delivery) && interrupt_delivery)
            schedule_provider_fault_drain(state->provider, state->unit_index);
    }
    return status;
}

na_status_t destroy_pci_dma_domain_backend(void *, void *opaque_state, bool &outcome_uncertain)
{
    auto *state = static_cast<pci_dma_domain_backend_state *>(opaque_state);
    if (state == nullptr || state->backend_state == nullptr)
        return NA_STATUS_INVALID_HANDLE;
    const na_status_t status =
        iommu::destroy_provider_dma_domain_backend(state->provider, state->backend_state, outcome_uncertain);
    if (status != NA_STATUS_OK || outcome_uncertain)
        return status == NA_STATUS_OK ? NA_STATUS_IO_ERROR : status;
    memory::Delete<>(memory::KernelCommonAllocatorV, state);
    return NA_STATUS_OK;
}

bool map_uncacheable_window(u64 virtual_base, u64 physical_base, size_t page_count)
{
    if (virtual_base == 0 || page_count == 0 || memory::kernel_vm_info == nullptr)
        return false;
    auto &paging = memory::kernel_vm_info->paging();
    paging.map_to(
        reinterpret_cast<void *>(virtual_base), page_count, phy_addr_t::from(physical_base),
        arch::paging::flags::writable | arch::paging::flags::cache_disable | arch::paging::flags::write_through, 0);
    paging.reload();
    return true;
}

void unmap_window(u64 virtual_base, size_t page_count)
{
    if (virtual_base == 0 || page_count == 0 || memory::kernel_vm_info == nullptr)
        return;
    auto &paging = memory::kernel_vm_info->paging();
    paging.unmap(reinterpret_cast<void *>(virtual_base), page_count, false);
    paging.reload();
}

struct ecam_quiesce_mapping
{
    const acpi_boot::mcfg_region *region = nullptr;
    u64 virtual_base = 0;
    u32 mapped_bus = 0x100;
    bool mapped = false;
};

bool map_ecam_quiesce_bus(ecam_quiesce_mapping &mapping, u8 bus)
{
    constexpr u64 ecam_bus_bytes = 1ULL << 20;
    constexpr size_t ecam_bus_pages = ecam_bus_bytes / memory::page_size;
    if (mapping.region == nullptr || bus < mapping.region->start_bus || bus > mapping.region->end_bus ||
        memory::kernel_vm_info == nullptr)
        return false;
    if (mapping.mapped && mapping.mapped_bus == bus)
        return true;

    const u64 bus_offset = static_cast<u64>(bus - mapping.region->start_bus) * ecam_bus_bytes;
    if (mapping.region->base > ~u64(0) - bus_offset)
        return false;
    if (mapping.mapped)
    {
        unmap_window(mapping.virtual_base, ecam_bus_pages);
        mapping.mapped = false;
    }
    if (!map_uncacheable_window(mapping.virtual_base, mapping.region->base + bus_offset, ecam_bus_pages))
        return false;
    mapping.mapped_bus = bus;
    mapping.mapped = true;
    return true;
}

void unmap_ecam_quiesce_bus(ecam_quiesce_mapping &mapping)
{
    constexpr size_t ecam_bus_pages = (1ULL << 20) / memory::page_size;
    if (!mapping.mapped || memory::kernel_vm_info == nullptr)
        return;
    unmap_window(mapping.virtual_base, ecam_bus_pages);
    mapping.mapped = false;
}

bool find_ecam_config_register(void *context, u8 bus, u8 device, u8 function, u32 offset, u32 width,
                               volatile byte *&register_address)
{
    constexpr u64 ecam_device_bytes = 1ULL << 15;
    constexpr u64 ecam_function_bytes = 1ULL << 12;
    const auto *mapping = static_cast<const ecam_quiesce_mapping *>(context);
    if (mapping == nullptr || mapping->region == nullptr || !mapping->mapped || mapping->mapped_bus != bus ||
        device >= 32 || function >= 8 || width == 0 || offset > ecam_function_bytes ||
        width > ecam_function_bytes - offset)
        return false;

    const u64 device_offset = static_cast<u64>(device) * ecam_device_bytes;
    const u64 function_offset = static_cast<u64>(function) * ecam_function_bytes;
    if (device_offset > ~u64(0) - function_offset || device_offset + function_offset > ~u64(0) - offset)
        return false;
    const u64 relative = device_offset + function_offset + offset;
    constexpr u64 ecam_bus_bytes = 1ULL << 20;
    if (relative > ecam_bus_bytes || width > ecam_bus_bytes - relative || mapping->virtual_base > ~u64(0) - relative)
        return false;
    register_address = reinterpret_cast<volatile byte *>(mapping->virtual_base + relative);
    return true;
}

class ecam_bus_session
{
  public:
    ecam_bus_session() = default;
    ecam_bus_session(const ecam_bus_session &) = delete;
    ecam_bus_session &operator=(const ecam_bus_session &) = delete;

    ~ecam_bus_session() { (void)close(); }

    bool open(const acpi_boot::mcfg_region &region, u8 bus)
    {
        constexpr u64 ecam_bus_bytes = 1ULL << 20;
        constexpr size_t ecam_bus_pages = ecam_bus_bytes / memory::page_size;
        if (mapping_lease_.id != 0 || pci_inventory.mcfg_regions == nullptr || hardware_ledger == nullptr ||
            memory::kernel_vm_info == nullptr || region.resource_lease_id == 0 || bus < region.start_bus ||
            bus > region.end_bus || region.start_bus > region.end_bus)
            return false;

        const u64 region_bus_count = static_cast<u64>(region.end_bus) - region.start_bus + 1;
        if (region_bus_count > (~u64{0} >> 20) || region.base > ~u64{0} - (region_bus_count << 20))
            return false;

        physical_resource::lease ecam_lease{};
        if (!hardware_ledger->lookup(region.resource_lease_id, ecam_lease) ||
            ecam_lease.resource_kind != physical_resource::kind::pci_ecam ||
            ecam_lease.mode != physical_resource::access::read_write ||
            ecam_lease.sharing_policy != physical_resource::sharing::exclusive ||
            ecam_lease.owner != physical_resource::ledger::kernel_owner() ||
            ecam_lease.lifecycle != physical_resource::state::leased ||
            ecam_lease.generation != pci_inventory.firmware_generation || ecam_lease.base != region.base ||
            ecam_lease.length != (region_bus_count << 20))
            return false;

        const u64 bus_offset = static_cast<u64>(bus - region.start_bus) * ecam_bus_bytes;
        if (region.base > ~u64{0} - bus_offset || region.base + bus_offset > ~u64{0} - ecam_bus_bytes)
            return false;
        const u64 bus_base = region.base + bus_offset;
        if (!hardware_ledger->reserve(region.resource_lease_id, bus_base, ecam_bus_bytes, bus_base,
                                      bus_base + ecam_bus_bytes, physical_resource::kind::pci_ecam,
                                      physical_resource::cache::uncacheable, physical_resource::access::read_write,
                                      physical_resource::ledger::kernel_owner(), physical_resource::sharing::exclusive,
                                      pci_inventory.firmware_generation, mapping_lease_))
            return false;
        if (!hardware_ledger->transition(mapping_lease_.id, physical_resource::state::reserved,
                                         physical_resource::state::leased) ||
            !hardware_ledger->map(mapping_lease_.id, bus_base, ecam_bus_bytes, physical_resource::kind::pci_ecam,
                                  physical_resource::cache::uncacheable, physical_resource::access::read_write,
                                  pci_inventory.firmware_generation))
        {
            (void)close();
            return false;
        }
        lease_mapped_ = true;

        if (pci_dma_config_virtual_base == 0)
        {
            const u64 virtual_base = memory::alloc_io_mmap_address(ecam_bus_bytes, memory::page_size);
            if (virtual_base >= memory::io_map_bottom_address && virtual_base <= memory::io_map_top_address &&
                ecam_bus_bytes <= memory::io_map_top_address - virtual_base)
                pci_dma_config_virtual_base = virtual_base;
        }
        if (pci_dma_config_virtual_base < memory::io_map_bottom_address ||
            pci_dma_config_virtual_base > memory::io_map_top_address ||
            ecam_bus_bytes > memory::io_map_top_address - pci_dma_config_virtual_base)
        {
            (void)close();
            return false;
        }

        if (!map_uncacheable_window(pci_dma_config_virtual_base, bus_base, ecam_bus_pages))
        {
            (void)close();
            return false;
        }
        ecam_mapped_ = true;
        mapping_ = {&region, pci_dma_config_virtual_base, bus, true};
        return true;
    }

    ecam_quiesce_mapping &mapping() { return mapping_; }

    bool close()
    {
        constexpr u64 ecam_bus_bytes = 1ULL << 20;
        constexpr size_t ecam_bus_pages = ecam_bus_bytes / memory::page_size;
        if (ecam_mapped_ && memory::kernel_vm_info != nullptr)
        {
            unmap_window(mapping_.virtual_base, ecam_bus_pages);
            ecam_mapped_ = false;
            mapping_.mapped = false;
        }
        if (mapping_lease_.id == 0)
            return !ecam_mapped_;
        if (hardware_ledger == nullptr)
            return false;
        const bool released = lease_mapped_ ? hardware_ledger->unmap_kernel(mapping_lease_.id)
                                            : hardware_ledger->revoke_kernel(mapping_lease_.id);
        if (!released)
            return false;
        mapping_lease_ = {};
        lease_mapped_ = false;
        mapping_ = {};
        return true;
    }

  private:
    physical_resource::lease mapping_lease_{};
    ecam_quiesce_mapping mapping_{};
    bool lease_mapped_ = false;
    bool ecam_mapped_ = false;
};

struct ecam_msi_config_context
{
    ecam_quiesce_mapping *mapping = nullptr;
    u8 bus = 0;
    u8 device = 0;
    u8 function = 0;
    pci::msix_table_mapping msix_table{};
};

bool find_function_ecam_region(const pci::function_identity &identity, const acpi_boot::mcfg_region *&region)
{
    region = nullptr;
    if (pci_inventory.mcfg_regions == nullptr || identity.segment > 0xffff || identity.bus > 0xff ||
        identity.device >= 32 || identity.function >= 8)
        return false;
    return pci::find_ecam_region_for_requester(pci_inventory.mcfg_regions, pci_inventory.mcfg_region_count,
                                               static_cast<u16>(identity.segment), static_cast<u8>(identity.bus),
                                               region) &&
           region->resource_lease_id != 0;
}

bool read_ecam_msi_config32(void *opaque_context, u16 offset, u32 &value)
{
    auto *context = static_cast<ecam_msi_config_context *>(opaque_context);
    volatile byte *address = nullptr;
    if (context == nullptr || context->mapping == nullptr || (offset & 3) != 0 || offset > 0xfc ||
        !find_ecam_config_register(context->mapping, context->bus, context->device, context->function, offset,
                                   sizeof(u32), address))
        return false;
    _mfence();
    value = *reinterpret_cast<volatile u32 *>(address);
    _mfence();
    return true;
}

bool write_ecam_msi_config32(void *opaque_context, u16 offset, u32 value)
{
    auto *context = static_cast<ecam_msi_config_context *>(opaque_context);
    volatile byte *address = nullptr;
    if (context == nullptr || context->mapping == nullptr || (offset & 3) != 0 || offset > 0xfc ||
        !find_ecam_config_register(context->mapping, context->bus, context->device, context->function, offset,
                                   sizeof(u32), address))
        return false;
    *reinterpret_cast<volatile u32 *>(address) = value;
    _mfence();
    return true;
}

bool read_ecam_msix_table32(void *opaque_context, u64 offset, u32 &value)
{
    const auto *context = static_cast<const ecam_msi_config_context *>(opaque_context);
    if (context == nullptr || context->msix_table.virtual_base == 0 || (offset & 3) != 0 ||
        offset > context->msix_table.table_bytes || sizeof(u32) > context->msix_table.table_bytes - offset ||
        context->msix_table.virtual_base > ~u64{0} - context->msix_table.table_offset ||
        context->msix_table.virtual_base + context->msix_table.table_offset > ~u64{0} - offset)
        return false;
    const u64 address = context->msix_table.virtual_base + context->msix_table.table_offset + offset;
    _mfence();
    value = *reinterpret_cast<volatile u32 *>(address);
    _mfence();
    return true;
}

bool write_ecam_msix_table32(void *opaque_context, u64 offset, u32 value)
{
    const auto *context = static_cast<const ecam_msi_config_context *>(opaque_context);
    if (context == nullptr || context->msix_table.virtual_base == 0 || (offset & 3) != 0 ||
        offset > context->msix_table.table_bytes || sizeof(u32) > context->msix_table.table_bytes - offset ||
        context->msix_table.virtual_base > ~u64{0} - context->msix_table.table_offset ||
        context->msix_table.virtual_base + context->msix_table.table_offset > ~u64{0} - offset)
        return false;
    const u64 address = context->msix_table.virtual_base + context->msix_table.table_offset + offset;
    *reinterpret_cast<volatile u32 *>(address) = value;
    _mfence();
    return true;
}

bool map_msix_table_for_function(const pci::function &pci_function, const acpi_boot::mcfg_region &region,
                                 const pci::msix_capability &capability, ecam_msi_config_context &config_context,
                                 pci::msix_table_mapping &mapping, pci::msi_config_error &failure)
{
    mapping = {};
    failure = pci::msi_config_error::resource_unavailable;
    const auto *inventory = pci_function.inventory();
    const auto *device_resources = pci_function.device_resources();
    const auto &identity = pci_function.identity();
    const auto &binding = pci_function.binding();
    auto *ledger = pci_function.ledger();
    auto *resource_root = pci_function.resource_root();
    if (inventory == nullptr || inventory != &pci_inventory || ledger == nullptr || ledger != hardware_ledger ||
        resource_root == nullptr || resource_root != hardware_resource_root_authority() ||
        resource_root->resources() != ledger || binding.region_index >= inventory->mcfg_region_count ||
        binding.boot_epoch != identity.boot_epoch || binding.firmware_generation != identity.firmware_generation ||
        binding.pci_fabric_generation != identity.pci_fabric_generation ||
        binding.platform_root_id != identity.platform_root_id || binding.segment != identity.segment ||
        binding.ecam.start_bus > identity.bus || binding.ecam.end_bus < identity.bus || device_resources == nullptr)
        return false;

    const auto &bound_region = inventory->mcfg_regions[binding.region_index];
    if (bound_region.base != region.base || bound_region.segment != region.segment ||
        bound_region.start_bus != region.start_bus || bound_region.end_bus != region.end_bus ||
        identity.segment != region.segment || device_resources->device_id == 0 || device_resources->path_length == 0 ||
        device_resources->path_length > pci::max_pci_device_function_path ||
        device_resources->memory_window_count == 0 ||
        device_resources->memory_window_count > pci::max_device_resource_windows ||
        device_resources->root_id != identity.platform_root_id || device_resources->segment != identity.segment ||
        device_resources->boot_epoch != identity.boot_epoch ||
        device_resources->firmware_generation != identity.firmware_generation ||
        device_resources->aml_namespace_generation != identity.aml_namespace_generation ||
        device_resources->platform_topology_generation != identity.platform_topology_generation ||
        device_resources->pci_fabric_generation != identity.pci_fabric_generation ||
        device_resources->start_bus != binding.resources.start_bus ||
        device_resources->end_bus != binding.resources.end_bus ||
        device_resources->compatibility_profile_hash != binding.resources.compatibility_profile_hash ||
        binding.resources.memory_window_count == 0 ||
        binding.resources.memory_window_count > pci::max_root_resource_windows)
    {
        failure = pci::msi_config_error::stale_identity;
        return false;
    }

    u32 header = 0;
    if (!read_ecam_msi_config32(&config_context, 0x0c, header))
    {
        failure = pci::msi_config_error::config_read_failed;
        return false;
    }
    const u32 header_type = (header >> 16) & 0x7f;
    const u8 bar_count = header_type == 0 ? 6 : header_type == 1 ? 2 : 0;
    if (bar_count == 0 || capability.table_bar >= bar_count || capability.pba_bar >= bar_count)
    {
        failure = pci::msi_config_error::invalid_capability;
        return false;
    }

    std::array<pci::msix_bar_aperture, pci::bar_register_count> apertures{};
    const pci::device_resource_window *table_window = nullptr;
    const pci::device_resource_window *pba_window = nullptr;
    for (u8 index = 0; index < bar_count;)
    {
        u32 low = 0;
        const u16 offset = static_cast<u16>(0x10u + static_cast<u32>(index) * sizeof(u32));
        if (!read_ecam_msi_config32(&config_context, offset, low))
        {
            failure = pci::msi_config_error::config_read_failed;
            return false;
        }
        const bool is_64_bit = (low & (3u << 1)) == (2u << 1);
        const bool table_bar = index == capability.table_bar;
        const bool pba_bar = index == capability.pba_bar;
        if (table_bar || pba_bar)
        {
            bool upper_available = false;
            u32 high = 0;
            if (is_64_bit)
            {
                if (index + 1 >= bar_count || capability.table_bar == index + 1 || capability.pba_bar == index + 1 ||
                    !read_ecam_msi_config32(&config_context, offset + sizeof(u32), high))
                {
                    failure = pci::msi_config_error::invalid_capability;
                    return false;
                }
                upper_available = true;
            }

            pci::bar_descriptor bar{};
            if (pci::decode_bar(bar_count, index, low, upper_available, high, bar) != pci::bar_decode_error::none)
            {
                failure = pci::msi_config_error::resource_unavailable;
                return false;
            }
            const pci::device_resource_window *window = nullptr;
            for (u32 window_index = 0; window_index < device_resources->memory_window_count; ++window_index)
            {
                const auto &candidate = device_resources->memory_windows[window_index];
                if (candidate.base != bar.base)
                    continue;
                if (window != nullptr)
                {
                    failure = pci::msi_config_error::invalid_capability;
                    return false;
                }
                window = &candidate;
            }
            if (window == nullptr || (window->flags & ~1u) != 0 || !pci::valid_bar_aperture(bar.base, window->length))
            {
                KLOG_WARN("ACPI: PCI MSI-X BAR is not covered by device _CRS segment={} bus={} device={} function={} "
                          "bar={} base={:x} attested_windows={}",
                          identity.segment, identity.bus, identity.device, identity.function, index, bar.base,
                          device_resources->memory_window_count);
                failure = pci::msi_config_error::resource_unavailable;
                return false;
            }
            bool within_root = false;
            for (u32 root_index = 0; root_index < binding.resources.memory_window_count; ++root_index)
            {
                const auto &root_window = binding.resources.memory_windows[root_index];
                if (pci::bar_range_within_window(bar.base, window->length, root_window.base, root_window.length))
                {
                    within_root = true;
                    break;
                }
            }
            if (!within_root)
            {
                failure = pci::msi_config_error::resource_unavailable;
                return false;
            }
            apertures[index] = {bar.base, window->length};
            if (table_bar)
                table_window = window;
            if (pba_bar)
                pba_window = window;
        }

        if (is_64_bit)
        {
            if (index + 1 >= bar_count)
            {
                failure = pci::msi_config_error::invalid_capability;
                return false;
            }
            index = static_cast<u8>(index + 2);
        }
        else
        {
            ++index;
        }
    }
    if (table_window == nullptr || pba_window == nullptr || (table_window->flags & 1u) == 0)
    {
        failure = pci::msi_config_error::resource_unavailable;
        return false;
    }

    pci::msix_region table{};
    pci::msix_region pba{};
    pci::msix_capability_error capability_failure = pci::msix_capability_error::none;
    if (!pci::resolve_msix_regions(capability, apertures, table, pba, capability_failure))
    {
        failure = pci::msi_config_error::invalid_capability;
        return false;
    }
    if (!pci::bar_range_within_window(table.base, table.length, table_window->base, table_window->length) ||
        !pci::bar_range_within_window(pba.base, pba.length, pba_window->base, pba_window->length))
    {
        failure = pci::msi_config_error::resource_unavailable;
        return false;
    }

    pci::msix_table_mapping_plan plan{};
    const auto plan_error = pci::plan_msix_table_mapping(table, apertures[capability.table_bar], table_window->flags,
                                                         binding.resources.memory_windows.data(),
                                                         binding.resources.memory_window_count, plan);
    if (plan_error != pci::msix_table_mapping_error::none || memory::kernel_vm_info == nullptr ||
        plan.page_count == 0 || plan.page_count > ~u64{0} / memory::page_size)
    {
        failure = pci::msi_config_error::resource_unavailable;
        return false;
    }
    const u64 map_bytes = plan.page_count * memory::page_size;

    physical_resource::lease lease{};
    const auto lease_result =
        pci::reserve_and_map_kernel_msix_table(*ledger, *resource_root, plan, identity.firmware_generation, lease);
    if (lease_result != pci::msix_table_lease_result::success)
    {
        failure = lease_result == pci::msix_table_lease_result::cleanup_failed
                      ? pci::msi_config_error::hardware_outcome_unknown
                      : pci::msi_config_error::resource_unavailable;
        return false;
    }

    const u64 virtual_base = memory::alloc_io_mmap_address(map_bytes, memory::page_size);
    if (virtual_base < memory::io_map_bottom_address || virtual_base > memory::io_map_top_address ||
        map_bytes > memory::io_map_top_address - virtual_base)
    {
        failure = ledger->revoke_kernel(lease.id) ? pci::msi_config_error::resource_unavailable
                                                  : pci::msi_config_error::hardware_outcome_unknown;
        return false;
    }
    if (!map_uncacheable_window(virtual_base, plan.page_cover.base, static_cast<size_t>(plan.page_count)))
    {
        failure = ledger->revoke_kernel(lease.id) ? pci::msi_config_error::resource_unavailable
                                                  : pci::msi_config_error::hardware_outcome_unknown;
        return false;
    }
    mapping = {virtual_base, plan.page_count, table.length, plan.table_offset, lease.id};
    failure = pci::msi_config_error::none;
    return true;
}

bool unmap_msix_table(const pci::msix_table_mapping &mapping)
{
    if (mapping.virtual_base == 0 || mapping.page_count == 0 || mapping.resource_lease_id == 0 ||
        memory::kernel_vm_info == nullptr || hardware_ledger == nullptr)
        return false;
    unmap_window(mapping.virtual_base, static_cast<size_t>(mapping.page_count));
    return hardware_ledger->unmap_kernel(mapping.resource_lease_id) &&
           hardware_ledger->revoke_kernel(mapping.resource_lease_id);
}

bool read_iommu_bridge_info(void *, u16 segment, u8 bus, u8 device, u8 function, u8 &header_type, u8 &primary_bus,
                            u8 &secondary_bus, u8 &subordinate_bus)
{
    if (pci_inventory.mcfg_regions == nullptr)
        return false;
    uctx::RawSpinLockUninterruptibleContext config_guard(pci_dma_config_lock);

    const acpi_boot::mcfg_region *region = nullptr;
    if (!pci::find_ecam_region_for_requester(pci_inventory.mcfg_regions, pci_inventory.mcfg_region_count, segment, bus,
                                             region) ||
        region->resource_lease_id == 0)
        return false;

    ecam_bus_session session;
    if (!session.open(*region, bus))
        return false;
    bool config_read = false;
    auto &mapping = session.mapping();
    volatile byte *header_address = nullptr;
    volatile byte *bus_address = nullptr;
    if (find_ecam_config_register(&mapping, bus, device, function, 0x0c, sizeof(u32), header_address) &&
        find_ecam_config_register(&mapping, bus, device, function, 0x18, sizeof(u32), bus_address))
    {
        _mfence();
        const u32 header = *reinterpret_cast<volatile u32 *>(header_address);
        const u32 buses = *reinterpret_cast<volatile u32 *>(bus_address);
        _mfence();
        header_type = static_cast<u8>((header >> 16) & 0xff);
        primary_bus = static_cast<u8>(buses & 0xff);
        secondary_bus = static_cast<u8>((buses >> 8) & 0xff);
        subordinate_bus = static_cast<u8>((buses >> 16) & 0xff);
        config_read = true;
    }
    return config_read && session.close();
}

u32 count_madt_ioapic_matches(void *, u8 enumeration_id)
{
    u32 madt_matches = 0;
    for (const auto &io_apic : get_io_apic_list())
        madt_matches += io_apic.enumeration_id == enumeration_id;
    return madt_matches;
}

bool validate_msi_route_binding(const pci::function_identity &identity, const pci_msi_route &location,
                                const pci::interrupt_route_config_snapshot &previous, pci::msi_config_error &failure,
                                bool require_live_route = true)
{
    const iommu::pci_function_location function{static_cast<u16>(identity.segment), static_cast<u8>(identity.bus),
                                                static_cast<u8>(identity.device), static_cast<u8>(identity.function)};
    iommu::device_source_ref source{};
    const bool resolved = iommu::kernel_provider_router().resolve_pci(function, identity.firmware_generation, source);
    const bool token_matches =
        resolved && (require_live_route ? iommu::kernel_provider_router().route_matches_pci(location.token, source)
                                        : location.token.valid() && location.token.provider == source.unit.provider &&
                                              location.token.generation == identity.firmware_generation);
    if (!token_matches)
    {
        failure = pci::msi_config_error::stale_identity;
        return false;
    }
    bool valid_msix_mapping = false;
    if (previous.mode == pci::interrupt_route_config_mode::msix && previous.msix.valid &&
        previous.table_mapping.virtual_base != 0 && previous.table_mapping.page_count != 0 &&
        previous.table_mapping.resource_lease_id != 0 &&
        previous.table_mapping.page_count <= ~u64{0} / memory::page_size)
    {
        const u64 map_bytes = previous.table_mapping.page_count * memory::page_size;
        const u64 expected_table_bytes =
            static_cast<u64>(previous.msix.capability.table_size) * pci::detail::msix_entry_bytes;
        valid_msix_mapping = previous.table_mapping.table_bytes == expected_table_bytes &&
                             previous.table_mapping.table_offset <= map_bytes &&
                             previous.table_mapping.table_bytes <= map_bytes - previous.table_mapping.table_offset;
    }
    const bool valid_config = previous.mode == pci::interrupt_route_config_mode::conventional_msi
                                  ? previous.msi.capability.offset != 0
                                  : previous.mode == pci::interrupt_route_config_mode::msix && valid_msix_mapping;
    if (location.vector < 32 || !valid_config)
    {
        failure = pci::msi_config_error::invalid_capability;
        return false;
    }

    const pci::msi_message programmed = previous.mode == pci::interrupt_route_config_mode::conventional_msi
                                            ? previous.msi.programmed_message
                                            : previous.msix.programmed_message;
    if (programmed.address != location.message.address || programmed.data != location.message.data)
    {
        failure = pci::msi_config_error::invalid_message;
        return false;
    }
    return true;
}

struct msi_route_teardown_context
{
    const pci::function_identity *identity = nullptr;
    const pci_msi_route *location = nullptr;
    const pci::interrupt_route_config_snapshot *previous = nullptr;
    u32 destination_cpu = ~u32{0};
};

bool disable_msi_route_step(void *opaque_context)
{
    auto *context = static_cast<msi_route_teardown_context *>(opaque_context);
    pci::msi_config_error failure = pci::msi_config_error::none;
    bool outcome_uncertain = false;
    return context != nullptr &&
           set_pci_msi_route_enabled(*context->identity, *context->location, *context->previous, false, failure,
                                     outcome_uncertain) &&
           !outcome_uncertain;
}

bool drain_msi_route_step(void *opaque_context)
{
    const auto *context = static_cast<msi_route_teardown_context *>(opaque_context);
    return context != nullptr && SMP::drain_msi_vector(context->destination_cpu, context->location->vector);
}

bool retire_msi_route_step(void *opaque_context)
{
    const auto *context = static_cast<msi_route_teardown_context *>(opaque_context);
    return context != nullptr && retire_pci_msi_route(*context->location);
}

bool restore_msi_route_step(void *opaque_context)
{
    auto *context = static_cast<msi_route_teardown_context *>(opaque_context);
    pci::msi_config_error failure = pci::msi_config_error::none;
    bool outcome_uncertain = false;
    return context != nullptr &&
           restore_pci_msi_route_configuration(*context->identity, *context->location, *context->previous, failure,
                                               outcome_uncertain) &&
           !outcome_uncertain;
}

bool read_quiesce_identity(void *context, u8 bus, u8 device, u8 function, u16 &vendor, u8 &header_type)
{
    volatile byte *identity_address = nullptr;
    volatile byte *header_address = nullptr;
    auto *mapping = static_cast<ecam_quiesce_mapping *>(context);
    if (mapping == nullptr || !map_ecam_quiesce_bus(*mapping, bus) ||
        !find_ecam_config_register(context, bus, device, function, 0, sizeof(u32), identity_address) ||
        !find_ecam_config_register(context, bus, device, function, 0x0c, sizeof(u32), header_address))
        return false;
    _mfence();
    const u32 identity = *reinterpret_cast<volatile u32 *>(identity_address);
    const u32 header = *reinterpret_cast<volatile u32 *>(header_address);
    _mfence();
    vendor = static_cast<u16>(identity & 0xffff);
    header_type = static_cast<u8>((header >> 16) & 0xff);
    return true;
}

bool read_quiesce_command(void *context, u8 bus, u8 device, u8 function, u16 &command)
{
    volatile byte *address = nullptr;
    if (!find_ecam_config_register(context, bus, device, function, 0x04, sizeof(u16), address))
        return false;
    _mfence();
    command = *reinterpret_cast<volatile u16 *>(address);
    _mfence();
    return true;
}

bool write_quiesce_command(void *context, u8 bus, u8 device, u8 function, u16 command)
{
    volatile byte *address = nullptr;
    if (!find_ecam_config_register(context, bus, device, function, 0x04, sizeof(u16), address))
        return false;
    *reinterpret_cast<volatile u16 *>(address) = command;
    _mfence();
    return true;
}

bool pci_requester_present(void *, u16 segment, u16 requester_id, bool &present)
{
    present = false;
    const u8 bus = static_cast<u8>(requester_id >> 8);
    const u8 device = static_cast<u8>((requester_id >> 3) & 0x1f);
    const u8 function = static_cast<u8>(requester_id & 0x07);
    const acpi_boot::mcfg_region *region = nullptr;
    if (pci_inventory.mcfg_regions == nullptr ||
        !pci::find_ecam_region_for_requester(pci_inventory.mcfg_regions, pci_inventory.mcfg_region_count, segment, bus,
                                             region) ||
        region->resource_lease_id == 0)
        return false;

    uctx::RawSpinLockUninterruptibleContext guard(pci_dma_config_lock);
    ecam_bus_session session{};
    if (!session.open(*region, bus))
        return false;
    auto &mapping = session.mapping();
    u16 vendor = 0xffff;
    u8 header_type = 0;
    const bool read = read_quiesce_identity(&mapping, bus, device, function, vendor, header_type);
    const bool closed = session.close();
    if (!read || !closed)
        return false;
    (void)header_type;
    present = vendor != 0 && vendor != 0xffff;
    return true;
}

bool pci_requester_bus_master_enabled(void *, u16 segment, u16 requester_id, bool &enabled)
{
    enabled = true;
    const u8 bus = static_cast<u8>(requester_id >> 8);
    const u8 device = static_cast<u8>((requester_id >> 3) & 0x1f);
    const u8 function = static_cast<u8>(requester_id & 0x07);
    const acpi_boot::mcfg_region *region = nullptr;
    if (pci_inventory.mcfg_regions == nullptr ||
        !pci::find_ecam_region_for_requester(pci_inventory.mcfg_regions, pci_inventory.mcfg_region_count, segment, bus,
                                             region) ||
        region->resource_lease_id == 0)
        return false;

    uctx::RawSpinLockUninterruptibleContext guard(pci_dma_config_lock);
    ecam_bus_session session{};
    if (!session.open(*region, bus))
        return false;
    auto &mapping = session.mapping();
    u16 command = 0;
    const bool read = read_quiesce_command(&mapping, bus, device, function, command);
    const bool closed = session.close();
    if (!read || !closed)
        return false;
    enabled = (command & (1u << 2)) != 0;
    return true;
}

bool quiesce_pci_dma_region(acpi_boot::mcfg_region &region, pci::dma_quiesce::result &scan_result,
                            pci::dma_quiesce::error &failure)
{
    failure = pci::dma_quiesce::error::mapping_unavailable;
    const u64 bus_count = static_cast<u64>(region.end_bus) - region.start_bus + 1;
    if (region.start_bus > region.end_bus || region.resource_lease_id == 0 || bus_count > (~u64(0) >> 20))
        return false;
    const u64 bytes = bus_count << 20;
    if (region.base > ~u64(0) - bytes || bytes % memory::page_size != 0 || memory::kernel_vm_info == nullptr)
        return false;

    physical_resource::lease ecam_lease{};
    if (hardware_ledger == nullptr || !hardware_ledger->lookup(region.resource_lease_id, ecam_lease) ||
        ecam_lease.resource_kind != physical_resource::kind::pci_ecam ||
        ecam_lease.mode != physical_resource::access::read_write ||
        ecam_lease.sharing_policy != physical_resource::sharing::exclusive ||
        ecam_lease.owner != physical_resource::ledger::kernel_owner() ||
        ecam_lease.lifecycle != physical_resource::state::leased ||
        ecam_lease.generation != pci_inventory.firmware_generation || ecam_lease.base != region.base ||
        ecam_lease.length != bytes)
        return false;

    physical_resource::lease mapping_lease{};
    if (!hardware_ledger->reserve(region.resource_lease_id, region.base, bytes, region.base, region.base + bytes,
                                  physical_resource::kind::pci_ecam, physical_resource::cache::uncacheable,
                                  physical_resource::access::read_write, physical_resource::ledger::kernel_owner(),
                                  physical_resource::sharing::exclusive, pci_inventory.firmware_generation,
                                  mapping_lease))
        return false;
    if (!hardware_ledger->transition(mapping_lease.id, physical_resource::state::reserved,
                                     physical_resource::state::leased) ||
        !hardware_ledger->map(mapping_lease.id, region.base, bytes, physical_resource::kind::pci_ecam,
                              physical_resource::cache::uncacheable, physical_resource::access::read_write,
                              pci_inventory.firmware_generation))
    {
        (void)hardware_ledger->revoke_kernel(mapping_lease.id);
        return false;
    }

    const auto release_mapping_lease = [&]() { return hardware_ledger->unmap_kernel(mapping_lease.id); };

    constexpr u64 ecam_bus_bytes = 1ULL << 20;
    const u64 virtual_base = memory::alloc_io_mmap_address(ecam_bus_bytes, memory::page_size);
    if (virtual_base < memory::io_map_bottom_address || virtual_base > memory::io_map_top_address ||
        ecam_bus_bytes > memory::io_map_top_address - virtual_base)
    {
        (void)release_mapping_lease();
        return false;
    }

    ecam_quiesce_mapping mapping{&region, virtual_base};
    const pci::dma_quiesce::config_ops operations{read_quiesce_identity, read_quiesce_command, write_quiesce_command,
                                                  &mapping, region.segment};
    failure = pci::dma_quiesce::quiesce_region(region, operations, scan_result);

    unmap_ecam_quiesce_bus(mapping);
    if (!release_mapping_lease())
    {
        failure = pci::dma_quiesce::error::mapping_unavailable;
        return false;
    }
    return failure == pci::dma_quiesce::error::none;
}

bool quiesce_pci_dma(acpi_boot::snapshot &snapshot, u32 &functions_seen, u32 &bus_masters_disabled,
                     pci::dma_quiesce::error &failure)
{
    functions_seen = 0;
    bus_masters_disabled = 0;
    failure = pci::dma_quiesce::error::invalid_region;
    if (snapshot.mcfg_regions.empty())
        return false;
    for (auto &region : snapshot.mcfg_regions)
    {
        pci::dma_quiesce::result result{};
        if (!quiesce_pci_dma_region(region, result, failure))
        {
            functions_seen += result.functions_seen;
            bus_masters_disabled += result.bus_masters_disabled;
            return false;
        }
        functions_seen += result.functions_seen;
        bus_masters_disabled += result.bus_masters_disabled;
    }
    return true;
}

bool read_pci_config32(void *, u16 segment, u8 bus, u8 device, u8 function, u16 offset, u32 &value)
{
    value = 0;
    if (pci_inventory.mcfg_regions == nullptr || hardware_ledger == nullptr || device >= 32 || function >= 8 ||
        offset > 0xffc || (offset & (alignof(u32) - 1)) != 0)
        return false;

    const acpi_boot::mcfg_region *region = nullptr;
    if (!pci::find_ecam_region_for_requester(pci_inventory.mcfg_regions, pci_inventory.mcfg_region_count, segment, bus,
                                             region) ||
        region->resource_lease_id == 0)
        return false;
    physical_resource::lease ecam_lease{};
    if (!hardware_ledger->lookup(region->resource_lease_id, ecam_lease) ||
        ecam_lease.resource_kind != physical_resource::kind::pci_ecam ||
        ecam_lease.owner != physical_resource::ledger::kernel_owner() ||
        ecam_lease.lifecycle != physical_resource::state::leased ||
        ecam_lease.generation != pci_inventory.firmware_generation)
        return false;

    uctx::RawSpinLockUninterruptibleContext guard(pci_dma_config_lock);
    ecam_bus_session session{};
    if (!session.open(*region, bus))
        return false;

    auto &mapping = session.mapping();
    volatile byte *config_address = nullptr;
    const bool mapped = find_ecam_config_register(&mapping, bus, device, function, offset, sizeof(u32), config_address);
    const bool read = mapped;
    if (read)
    {
        _mfence();
        value = *reinterpret_cast<volatile u32 *>(config_address);
        _mfence();
    }
    const bool closed = session.close();
    return read && closed;
}

na_status_t translate_pci_config_error(pci::dma_quiesce::error failure, bool &outcome_uncertain)
{
    using pci::dma_quiesce::error;
    switch (failure)
    {
        case error::none:
            return NA_STATUS_OK;
        case error::invalid_device_id:
        case error::invalid_region:
            return NA_STATUS_INVALID_ARGUMENT;
        case error::invalid_operations:
        case error::mapping_unavailable:
            return NA_STATUS_NOT_SUPPORTED;
        case error::config_read_failed:
        case error::command_read_failed:
        case error::command_write_failed:
        case error::bus_master_still_enabled:
        case error::bus_master_still_disabled:
            outcome_uncertain = true;
            return NA_STATUS_IO_ERROR;
        case error::domain_required:
        case error::domain_not_ready:
        case error::no_iommu_policy:
            return NA_STATUS_ACCESS_DENIED;
        case error::domain_state_uncertain:
            outcome_uncertain = true;
            return NA_STATUS_IO_ERROR;
    }
    outcome_uncertain = true;
    return NA_STATUS_IO_ERROR;
}

na_status_t set_pci_dma_bus_master(const pci::function_identity &identity, bool enabled,
                                   const pci::bus_master_enable_authority *authority, bool &outcome_uncertain)
{
    outcome_uncertain = false;
    if (pci_inventory.mcfg_regions == nullptr || hardware_ledger == nullptr || memory::kernel_vm_info == nullptr ||
        identity.segment > 0xffff || identity.bus > 0xff || identity.device >= 32 || identity.function >= 8)
        return NA_STATUS_INVALID_ARGUMENT;
    if ((enabled && authority == nullptr) || (!enabled && authority != nullptr))
        return NA_STATUS_ACCESS_DENIED;
    if (enabled && (authority->segment() != identity.segment || authority->bus() != identity.bus ||
                    authority->device() != identity.device || authority->function() != identity.function))
        return NA_STATUS_ACCESS_DENIED;

    uctx::RawSpinLockUninterruptibleContext config_guard(pci_dma_config_lock);
    const acpi_boot::mcfg_region *region = nullptr;
    if (!pci::find_ecam_region_for_requester(pci_inventory.mcfg_regions, pci_inventory.mcfg_region_count,
                                             static_cast<u16>(identity.segment), static_cast<u8>(identity.bus),
                                             region) ||
        region->resource_lease_id == 0)
        return NA_STATUS_NOT_SUPPORTED;
    ecam_bus_session session{};
    if (!session.open(*region, static_cast<u8>(identity.bus)))
        return NA_STATUS_RESOURCE_EXHAUSTED;

    auto &mapping = session.mapping();
    const pci::dma_quiesce::config_ops operations{nullptr, read_quiesce_command, write_quiesce_command, &mapping,
                                                  identity.segment};
    const auto config_error = enabled ? pci::dma_quiesce::set_bus_master(operations, *authority)
                                      : pci::dma_quiesce::set_bus_master(operations, static_cast<u8>(identity.bus),
                                                                         static_cast<u8>(identity.device),
                                                                         static_cast<u8>(identity.function), false);
    na_status_t status = translate_pci_config_error(config_error, outcome_uncertain);
    if (!session.close())
    {
        outcome_uncertain = true;
        status = NA_STATUS_IO_ERROR;
    }
    return status;
}

na_status_t disable_pci_requester_bus_master(void *, const pci::function_identity &identity, bool &outcome_uncertain)
{
    return set_pci_dma_bus_master(identity, false, nullptr, outcome_uncertain);
}

bool report_provider_fault_records(iommu::provider_kind provider, u32 unit_index, bool log_records)
{
    const iommu::fault_quiesce_ops quiesce_ops{nullptr, disable_pci_requester_bus_master};
    iommu::fault_drain_report report{};
    const bool more = iommu::drain_provider_fault_events(provider, unit_index, quiesce_ops, log_records, report);
    if (report.retry)
        return true;
    if (!report.drain_succeeded)
        KLOG_WARN("ACPI: IOMMU fault drain failed provider={} unit={} status={} records={}", static_cast<u8>(provider),
                  unit_index, report.status_code, report.record_count);
    if (report.event_log_overflow)
        KLOG_WARN("ACPI: IOMMU fault event source overflow provider={} unit={} records={}", static_cast<u8>(provider),
                  unit_index, report.record_count);
    if (report.unexpected_record_count != 0)
        KLOG_WARN("ACPI: IOMMU fault drain found unexpected records provider={} unit={} count={}",
                  static_cast<u8>(provider), unit_index, report.unexpected_record_count);
    if (report.source_overflow_count != 0)
        KLOG_WARN("ACPI: DMA fault source queues overflowed provider={} unit={} sources={}", static_cast<u8>(provider),
                  unit_index, report.source_overflow_count);
    return more;
}

void drain_pending_provider_fault_interrupts(u64) noexcept
{
    for (u32 provider_index = static_cast<u32>(iommu::provider_kind::intel_vtd);
         provider_index <= static_cast<u32>(iommu::provider_kind::amd_vi); ++provider_index)
    {
        auto &state = provider_fault_irq_states[provider_index];
        for (u32 index = 0; index < state.slot_count; ++index)
        {
            auto &slot = state.slots[index];
            if (slot.pending.exchange(false, std::memory_order_acq_rel) &&
                report_provider_fault_records(slot.provider, slot.unit_index, false))
                slot.schedule_soft_drain();
        }
    }
}

void initialize_provider_fault_interrupts(iommu::provider_kind provider)
{
    const u32 provider_index = static_cast<u32>(provider);
    if (provider_index >= sizeof(provider_fault_irq_states) / sizeof(provider_fault_irq_states[0]))
        return;
    auto &state = provider_fault_irq_states[provider_index];
    const auto &initialization = provider_initialization_reports[provider_index];
    if (state.setup_attempted || initialization.status != NA_STATUS_OK ||
        initialization.fault_delivery != iommu::fault_delivery_mode::interrupt)
        return;
    state.setup_attempted = true;
    const u32 unit_count = initialization.selected_units;
    if (unit_count == 0 || memory::KernelCommonAllocatorV == nullptr)
        return;

    const u64 apic_id = arch::APIC::local_ID();
    if (apic_id > 0xff)
    {
        KLOG_WARN("ACPI: IOMMU fault-event IRQs unavailable for xAPIC destination APIC ID {} provider={}", apic_id,
                  provider_index);
        return;
    }

    auto *allocator = memory::KernelCommonAllocatorV;
    void *slot_storage =
        allocator->allocate(sizeof(provider_fault_irq_slot) * unit_count, alignof(provider_fault_irq_slot));
    if (slot_storage == nullptr)
    {
        KLOG_WARN("ACPI: unable to allocate IOMMU fault-event IRQ state provider={}", provider_index);
        return;
    }
    auto *slots = static_cast<provider_fault_irq_slot *>(slot_storage);
    for (u32 index = 0; index < unit_count; ++index)
    {
        new (slots + index) provider_fault_irq_slot();
        slots[index].provider = provider;
        slots[index].unit_index = index;
    }
    state.slots = slots;
    state.slot_count = unit_count;

    if (!provider_fault_soft_irq_setup_attempted)
    {
        provider_fault_soft_irq_setup_attempted = true;
        void *soft_registration_storage = allocator->allocate(sizeof(irq::registration), alignof(irq::registration));
        if (soft_registration_storage == nullptr)
        {
            KLOG_WARN("ACPI: unable to allocate IOMMU fault-event soft IRQ registration");
            return;
        }
        auto *soft_registration = new (soft_registration_storage) irq::registration(irq::register_soft_handler(
            irq::soft_vector::iommu_fault, irq::soft_handler::bind<&drain_pending_provider_fault_interrupts>()));
        if (!*soft_registration)
        {
            KLOG_WARN("ACPI: unable to register IOMMU fault-event soft IRQ handler");
            soft_registration->~registration();
            allocator->deallocate(soft_registration_storage);
            return;
        }
        provider_fault_soft_irq_registration = soft_registration;
    }

    u32 configured_units = 0;
    for (u32 index = 0; index < unit_count; ++index)
    {
        auto &slot = slots[index];
        slot.vector = irq::allocate_msi_vector();
        if (!slot.vector)
        {
            KLOG_WARN("ACPI: no vector available for IOMMU fault-event provider={} unit={}", provider_index, index);
            continue;
        }
        slot.registration = irq::register_handler(
            slot.vector.vector(), irq::hard_handler::bind<&provider_fault_irq_slot::on_interrupt>(slot));
        if (!slot.registration)
        {
            slot.vector.reset();
            KLOG_WARN("ACPI: unable to register IOMMU fault-event IRQ provider={} unit={}", provider_index, index);
            continue;
        }

        bool outcome_uncertain = false;
        const na_status_t status = iommu::configure_provider_fault_interrupt(provider, index, static_cast<u32>(apic_id),
                                                                             slot.vector.vector(), outcome_uncertain);
        if (status != NA_STATUS_OK)
        {
            if (outcome_uncertain)
            {
                // Keep the vector and handler if the provider may have unmasked the route.
                KLOG_WARN("ACPI: IOMMU fault-event IRQ route uncertain provider={} unit={} status={}", provider_index,
                          index, status);
                continue;
            }
            slot.registration.reset();
            slot.vector.reset();
            KLOG_WARN("ACPI: IOMMU fault-event IRQ unavailable provider={} unit={} status={}", provider_index, index,
                      status);
            continue;
        }
        configured_units++;
    }
    KLOG_INFO("ACPI: IOMMU fault-event IRQs configured provider={} units={} total={}", provider_index, configured_units,
              unit_count);
}

iommu::dma_domain_create_result create_pci_dma_domain(void *, const iommu::device_source_ref &source,
                                                      const pci::function_identity &identity, process_id owner,
                                                      handle_t<pci::dma_domain> &result,
                                                      handle_t<pci::dma_fault_source> &fault_source)
{
    result.reset();
    fault_source.reset();
    if (owner == 0)
        return {NA_STATUS_INVALID_ARGUMENT, false};
    if (!pci_dma_quiescence_verified || pci_inventory.mcfg_regions == nullptr || hardware_ledger == nullptr ||
        hardware_resource_root_authority() == nullptr || memory::KernelCommonAllocatorV == nullptr)
        return {NA_STATUS_NOT_SUPPORTED, false};
    if (identity.platform_root_id == 0 || identity.boot_epoch != pci_inventory.boot_epoch ||
        identity.firmware_generation != pci_inventory.firmware_generation ||
        identity.pci_fabric_generation != pci_inventory.pci_fabric_generation ||
        identity.aml_namespace_generation == 0 || identity.platform_topology_generation == 0 ||
        identity.segment > 0xffff || identity.bus > 0xff || identity.device >= 32 || identity.function >= 8)
        return {NA_STATUS_INVALID_ARGUMENT, false};
    if (!pci::topology_lifetime().is_current(identity.boot_epoch, identity.firmware_generation,
                                             identity.aml_namespace_generation, identity.pci_fabric_generation,
                                             identity.platform_topology_generation))
        return {NA_STATUS_OBJECT_REVOKED, false};

    const iommu::provider_kind provider = source.unit.provider;
    if (!source.valid() || source.unit.generation != identity.firmware_generation ||
        source.segment != identity.segment || source.device_id != pci_requester_id(identity))
        return {NA_STATUS_NOT_SUPPORTED, false};
    if (!iommu::provider_dma_domain_ready_for_bus_master(provider))
        return {NA_STATUS_NOT_SUPPORTED, false};

    const acpi_boot::mcfg_region *region = nullptr;
    if (!find_function_ecam_region(identity, region))
        return {NA_STATUS_NOT_SUPPORTED, false};
    physical_resource::lease ecam_lease{};
    if (!hardware_ledger->lookup(region->resource_lease_id, ecam_lease) ||
        ecam_lease.resource_kind != physical_resource::kind::pci_ecam ||
        ecam_lease.owner != physical_resource::ledger::kernel_owner() ||
        ecam_lease.lifecycle != physical_resource::state::leased ||
        ecam_lease.generation != pci_inventory.firmware_generation)
        return {NA_STATUS_ACCESS_DENIED, false};

    if (memory::KernelCommonAllocatorV == nullptr)
        return {NA_STATUS_RESOURCE_EXHAUSTED, false};
    auto fault_handle = handle_t<pci::dma_fault_source>::make(owner, identity);
    if (!fault_handle)
        return {NA_STATUS_RESOURCE_EXHAUSTED, false};

    void *storage = memory::KernelCommonAllocatorV->allocate(sizeof(pci_dma_domain_backend_state),
                                                             alignof(pci_dma_domain_backend_state));
    if (storage == nullptr)
        return {NA_STATUS_RESOURCE_EXHAUSTED, false};
    auto *state = new (storage) pci_dma_domain_backend_state();
    state->identity = identity;
    state->provider = provider;
    state->requester_id = pci_requester_id(identity);
    bool outcome_uncertain = false;
    u8 address_width = 0;
    u32 unit_index = ~u32{0};
    const na_status_t create_status =
        iommu::create_provider_dma_domain_backend(provider, source, identity, owner, fault_handle, state->backend_state,
                                                  address_width, unit_index, outcome_uncertain);
    state->unit_index = unit_index;
    if (create_status != NA_STATUS_OK)
    {
        // The provider retains any partially initialized hardware state through
        // its opaque pointer. The generic wrapper is unused without a domain.
        outcome_uncertain = outcome_uncertain || state->backend_state != nullptr;
        memory::Delete<>(memory::KernelCommonAllocatorV, state);
        return {create_status, outcome_uncertain};
    }
    if (state->backend_state == nullptr || unit_index == ~u32{0} || address_width < 12 || address_width > 52)
    {
        if (state->backend_state == nullptr)
        {
            memory::Delete<>(memory::KernelCommonAllocatorV, state);
            return {NA_STATUS_IO_ERROR, true};
        }
        bool destroy_uncertain = false;
        const na_status_t destroy_status =
            iommu::destroy_provider_dma_domain_backend(provider, state->backend_state, destroy_uncertain);
        if (destroy_status == NA_STATUS_OK && !destroy_uncertain)
            memory::Delete<>(memory::KernelCommonAllocatorV, state);
        return {NA_STATUS_IO_ERROR, destroy_uncertain || destroy_status != NA_STATUS_OK};
    }

    const pci::dma_domain::backend_ops operations{map_pci_dma_domain_page, unmap_pci_dma_domain_page,
                                                  set_pci_dma_domain_bus_master, destroy_pci_dma_domain_backend};
    const pci::dma_domain::configuration config{true, false, true, false, address_width};
    result = handle_t<pci::dma_domain>::make(owner, identity, operations, nullptr, state, config,
                                             hardware_resource_root_authority());
    if (result)
    {
        fault_source = std::move(fault_handle);
        return {NA_STATUS_OK, false};
    }

    bool destroy_uncertain = false;
    const na_status_t destroy_status = destroy_pci_dma_domain_backend(nullptr, state, destroy_uncertain);
    if (destroy_status != NA_STATUS_OK || destroy_uncertain)
        return {destroy_status == NA_STATUS_OK ? NA_STATUS_IO_ERROR : destroy_status, true};
    return {NA_STATUS_RESOURCE_EXHAUSTED, false};
}

bool acquire_iommu_register_aperture(void *context, u64 physical_base, u64 length, u64 generation, u64 &lease_id,
                                     u64 &kernel_virtual_base)
{
    lease_id = 0;
    kernel_virtual_base = 0;
    auto *resource_root = static_cast<physical_resource::root *>(context);
    if (resource_root == nullptr || resource_root != hardware_resource_root_authority() || hardware_ledger == nullptr ||
        memory::kernel_vm_info == nullptr || physical_base == 0 || generation == 0 || length == 0 ||
        (physical_base & (memory::page_size - 1)) != 0 || length % memory::page_size != 0 ||
        physical_base > ~u64{0} - length || length / memory::page_size > static_cast<u64>(~size_t{0}))
        return false;

    physical_resource::lease lease{};
    if (!hardware_ledger->reserve(*resource_root, physical_base, length, physical_base, physical_base + length,
                                  physical_resource::kind::iommu_register, physical_resource::cache::uncacheable,
                                  physical_resource::access::read_write, generation, lease))
        return false;
    if (!hardware_ledger->transition(lease.id, physical_resource::state::reserved, physical_resource::state::leased) ||
        !hardware_ledger->map(lease.id, physical_base, length, physical_resource::kind::iommu_register,
                              physical_resource::cache::uncacheable, physical_resource::access::read_write, generation))
    {
        (void)hardware_ledger->revoke_kernel(lease.id);
        return false;
    }

    const u64 virtual_base = memory::alloc_io_mmap_address(length, memory::page_size);
    if (virtual_base < memory::io_map_bottom_address || virtual_base > memory::io_map_top_address ||
        length > memory::io_map_top_address - virtual_base)
    {
        (void)hardware_ledger->unmap_kernel(lease.id);
        return false;
    }
    memory::kernel_vm_info->paging().map_to(
        reinterpret_cast<void *>(virtual_base), static_cast<size_t>(length / memory::page_size),
        phy_addr_t::from(physical_base),
        arch::paging::flags::writable | arch::paging::flags::cache_disable | arch::paging::flags::write_through, 0);
    memory::kernel_vm_info->paging().reload();
    lease_id = lease.id;
    kernel_virtual_base = virtual_base;
    return true;
}

bool release_iommu_register_aperture(void *context, u64 physical_base, u64 length, u64 generation, u64 lease_id,
                                     u64 kernel_virtual_base)
{
    auto *resource_root = static_cast<physical_resource::root *>(context);
    if (resource_root == nullptr || resource_root != hardware_resource_root_authority() || hardware_ledger == nullptr ||
        memory::kernel_vm_info == nullptr || lease_id == 0 || kernel_virtual_base == 0 || length == 0 ||
        length % memory::page_size != 0)
        return false;

    physical_resource::lease lease{};
    if (!hardware_ledger->lookup(lease_id, lease) || lease.resource_kind != physical_resource::kind::iommu_register ||
        lease.base != physical_base || lease.length != length || lease.generation != generation ||
        lease.lifecycle != physical_resource::state::mapped)
        return false;

    memory::kernel_vm_info->paging().unmap(reinterpret_cast<void *>(kernel_virtual_base),
                                           static_cast<size_t>(length / memory::page_size), false);
    memory::kernel_vm_info->paging().reload();
    return hardware_ledger->unmap_kernel(lease_id);
}

void initialize_iommu_providers_impl(acpi_boot::snapshot &inventory, bool pci_dma_quiesced,
                                     physical_resource::root *resource_root,
                                     pci_runtime::range_validator validate_device_range,
                                     pci_runtime::range_validator validate_register_range)
{
    const auto ioapics = get_io_apic_list();
    u8 ioapic_enumeration_ids[iommu::max_ioapic_sources]{};
    const u32 copied_ioapic_count =
        static_cast<u32>(ioapics.size() < iommu::max_ioapic_sources ? ioapics.size() : iommu::max_ioapic_sources);
    for (u32 index = 0; index < copied_ioapic_count; ++index)
        ioapic_enumeration_ids[index] = ioapics.at(index).enumeration_id;
    const u32 ioapic_source_count =
        ioapics.size() > iommu::max_ioapic_sources ? iommu::max_ioapic_sources + 1 : static_cast<u32>(ioapics.size());
    const iommu::pci_requester_platform_ops requester_operations{pci_requester_bus_master_enabled,
                                                                 pci_requester_present, read_pci_config32};
    iommu::provider_bootstrap_config bootstrap{};
    bootstrap.firmware_inventory = &inventory;
    bootstrap.pci_bus_mastering_quiesced = pci_dma_quiesced;
    bootstrap.ioapic_enumeration_ids = ioapic_enumeration_ids;
    bootstrap.ioapic_source_count = ioapic_source_count;
    bootstrap.pci_requesters = requester_operations;
    bootstrap.count_madt_ioapic_matches = count_madt_ioapic_matches;
    bootstrap.read_bridge_info = read_iommu_bridge_info;
    bootstrap.create_dma_domain = create_pci_dma_domain;
    bootstrap.acquire_registers = acquire_iommu_register_aperture;
    bootstrap.release_registers = release_iommu_register_aperture;
    bootstrap.register_aperture_context = resource_root;
    bootstrap.validate_device_range = validate_device_range;
    bootstrap.validate_register_range = validate_register_range;
    bootstrap.range_validation_context = nullptr;
    const auto results = iommu::initialize_firmware_provider_backends(bootstrap);
    for (u32 provider_index = static_cast<u32>(iommu::provider_kind::intel_vtd);
         provider_index <= static_cast<u32>(iommu::provider_kind::amd_vi); ++provider_index)
        provider_initialization_reports[provider_index] = results.providers[provider_index];
}

} // namespace

bool resolve_ioapic_interrupt_source(u8 enumeration_id, ioapic_source_identity &identity)
{
    identity = {};
    if (firmware_snapshot == nullptr)
        return false;
    return iommu::kernel_provider_router().resolve_ioapic(enumeration_id, firmware_snapshot->firmware_generation,
                                                          identity.source);
}

bool install_ioapic_interrupt_route(u8 enumeration_id, u32 destination_apic_id, u8 vector, bool low_polarity,
                                    bool level_triggered, iommu::ioapic_route_encoding &route)
{
    route = {};
    if (firmware_snapshot == nullptr)
        return false;
    return iommu::kernel_provider_router().install_ioapic(enumeration_id, firmware_snapshot->firmware_generation,
                                                          destination_apic_id, vector, low_polarity, level_triggered,
                                                          route);
}

bool disable_ioapic_interrupt_route(const iommu::interrupt_route_token &route)
{
    return iommu::kernel_provider_router().retire(route);
}

static bool install_pci_msi_route(const pci::function_identity &identity, u8 vector, pci_msi_route &location,
                                  bool &outcome_uncertain)
{
    location = {};
    outcome_uncertain = false;
    if (pci_inventory.mcfg_regions == nullptr ||
        !pci::topology_lifetime().is_current(identity.boot_epoch, identity.firmware_generation,
                                             identity.aml_namespace_generation, identity.pci_fabric_generation,
                                             identity.platform_topology_generation))
        return false;

    const u32 destination_apic_id = static_cast<u32>(arch::APIC::local_ID());
    if (destination_apic_id > 0xff)
        return false;

    const iommu::pci_function_location pci_location{static_cast<u16>(identity.segment), static_cast<u8>(identity.bus),
                                                    static_cast<u8>(identity.device),
                                                    static_cast<u8>(identity.function)};
    if (!iommu::kernel_provider_router().install_pci_msi(pci_location, identity.firmware_generation,
                                                         destination_apic_id, vector, location.token, location.message,
                                                         outcome_uncertain))
        return false;
    location.destination_apic_id = destination_apic_id;
    location.vector = vector;
    return true;
}

static bool retire_pci_msi_route(const pci_msi_route &location)
{
    return location.vector != 0 && iommu::kernel_provider_router().retire(location.token);
}

static bool prepare_pci_msi_route(const pci::function &function, u8 vector, pci_msi_route &location,
                                  pci::interrupt_route_config_snapshot &previous, pci::msi_config_error &failure,
                                  bool &outcome_uncertain)
{
    const auto &identity = function.identity();
    const auto *device_resources = function.device_resources();
    location = {};
    previous = {};
    previous.mode = pci::interrupt_route_config_mode::conventional_msi;
    failure = pci::msi_config_error::none;
    outcome_uncertain = false;
    if (!pci::topology_lifetime().is_current(identity.boot_epoch, identity.firmware_generation,
                                             identity.aml_namespace_generation, identity.pci_fabric_generation,
                                             identity.platform_topology_generation))
    {
        failure = pci::msi_config_error::stale_identity;
        return false;
    }

    bool route_outcome_uncertain = false;
    if (!install_pci_msi_route(identity, vector, location, route_outcome_uncertain))
    {
        outcome_uncertain = route_outcome_uncertain;
        failure = outcome_uncertain ? pci::msi_config_error::hardware_outcome_unknown
                                    : pci::msi_config_error::route_unavailable;
        return false;
    }

    const acpi_boot::mcfg_region *region = nullptr;
    const pci::msi_message message = location.message;
    if (!pci::detail::is_valid_msi_message(message) || !find_function_ecam_region(identity, region))
        failure = pci::msi_config_error::resource_unavailable;
    else
    {
        uctx::RawSpinLockUninterruptibleContext config_guard(pci_dma_config_lock);
        ecam_bus_session session;
        if (!session.open(*region, static_cast<u8>(identity.bus)))
            failure = pci::msi_config_error::resource_unavailable;
        else
        {
            ecam_msi_config_context context{&session.mapping(), static_cast<u8>(identity.bus),
                                            static_cast<u8>(identity.device), static_cast<u8>(identity.function)};
            pci::msix_capability msix_capability{};
            pci::msix_capability_error msix_failure = pci::msix_capability_error::none;
            if (pci::find_msix_capability(read_ecam_msi_config32, &context, msix_capability, msix_failure))
            {
                previous.mode = pci::interrupt_route_config_mode::msix;
                const bool table_mapped = map_msix_table_for_function(function, *region, msix_capability, context,
                                                                      previous.table_mapping, failure);
                if (table_mapped)
                {
                    context.msix_table = previous.table_mapping;
                    const pci::msix_config_ops operations{read_ecam_msi_config32,
                                                          write_ecam_msi_config32,
                                                          read_ecam_msix_table32,
                                                          write_ecam_msix_table32,
                                                          &context,
                                                          previous.table_mapping.table_bytes};
                    if (!pci::prepare_remappable_msix(operations, message, previous.msix, failure) &&
                        failure != pci::msi_config_error::hardware_outcome_unknown)
                    {
                        if (!unmap_msix_table(previous.table_mapping))
                        {
                            failure = pci::msi_config_error::hardware_outcome_unknown;
                            outcome_uncertain = true;
                        }
                        else
                        {
                            previous.table_mapping = {};
                        }
                    }
                }
                else
                {
                    KLOG_WARN("ACPI: PCI MSI-X table mapping unavailable segment={} bus={} device={} function={} "
                              "table_bar={} table_offset={:x} table_size={} pba_bar={} pba_offset={:x} "
                              "resource_windows={} failure={}",
                              identity.segment, identity.bus, identity.device, identity.function,
                              msix_capability.table_bar, msix_capability.table_offset, msix_capability.table_size,
                              msix_capability.pba_bar, msix_capability.pba_offset,
                              device_resources == nullptr ? 0 : device_resources->memory_window_count,
                              static_cast<u32>(failure));
                }
            }
            else if (msix_failure == pci::msix_capability_error::capability_not_found)
            {
                previous.mode = pci::interrupt_route_config_mode::conventional_msi;
                const pci::msi_config_ops operations{read_ecam_msi_config32, write_ecam_msi_config32, &context};
                pci::msi_capability capability{};
                pci::msi_capability_error capability_failure = pci::msi_capability_error::none;
                if (!pci::find_msi_capability(read_ecam_msi_config32, &context, capability, capability_failure))
                {
                    failure = capability_failure == pci::msi_capability_error::config_read_failed
                                  ? pci::msi_config_error::config_read_failed
                                  : pci::msi_config_error::invalid_capability;
                }
                else
                {
                    (void)pci::prepare_remappable_msi(operations, capability, message, previous.msi, failure);
                }
            }
            else
            {
                failure = msix_failure == pci::msix_capability_error::config_read_failed
                              ? pci::msi_config_error::config_read_failed
                              : pci::msi_config_error::invalid_capability;
            }

            if (!session.close())
            {
                failure = pci::msi_config_error::hardware_outcome_unknown;
                outcome_uncertain = true;
            }
        }
    }

    if (failure == pci::msi_config_error::none)
        return true;
    if (outcome_uncertain || failure == pci::msi_config_error::hardware_outcome_unknown)
    {
        outcome_uncertain = true;
        return false;
    }
    if (!retire_pci_msi_route(location))
    {
        failure = pci::msi_config_error::hardware_outcome_unknown;
        outcome_uncertain = true;
        return false;
    }
    location = {};
    previous = {};
    return false;
}

static bool set_pci_msi_route_enabled(const pci::function_identity &identity, const pci_msi_route &location,
                                      const pci::interrupt_route_config_snapshot &previous, bool enabled,
                                      pci::msi_config_error &failure, bool &outcome_uncertain)
{
    failure = pci::msi_config_error::none;
    outcome_uncertain = false;
    if (!validate_msi_route_binding(identity, location, previous, failure))
        return false;

    const acpi_boot::mcfg_region *region = nullptr;
    if (!find_function_ecam_region(identity, region))
    {
        failure = pci::msi_config_error::resource_unavailable;
        return false;
    }

    uctx::RawSpinLockUninterruptibleContext config_guard(pci_dma_config_lock);
    ecam_bus_session session;
    if (!session.open(*region, static_cast<u8>(identity.bus)))
    {
        failure = pci::msi_config_error::resource_unavailable;
        return false;
    }
    ecam_msi_config_context context{&session.mapping(), static_cast<u8>(identity.bus), static_cast<u8>(identity.device),
                                    static_cast<u8>(identity.function)};
    bool configured = false;
    if (previous.mode == pci::interrupt_route_config_mode::conventional_msi)
    {
        const pci::msi_config_ops operations{read_ecam_msi_config32, write_ecam_msi_config32, &context};
        configured = pci::set_remappable_msi_enabled(operations, previous.msi, enabled, failure);
    }
    else
    {
        context.msix_table = previous.table_mapping;
        const pci::msix_config_ops operations{read_ecam_msi_config32,
                                              write_ecam_msi_config32,
                                              read_ecam_msix_table32,
                                              write_ecam_msix_table32,
                                              &context,
                                              previous.table_mapping.table_bytes};
        configured = pci::set_remappable_msix_enabled(operations, previous.msix, enabled, failure);
    }
    if (!session.close())
    {
        failure = pci::msi_config_error::hardware_outcome_unknown;
        outcome_uncertain = true;
        return false;
    }
    outcome_uncertain = failure == pci::msi_config_error::hardware_outcome_unknown;
    return configured;
}

static bool restore_pci_msi_route_configuration(const pci::function_identity &identity, const pci_msi_route &location,
                                                const pci::interrupt_route_config_snapshot &previous,
                                                pci::msi_config_error &failure, bool &outcome_uncertain)
{
    failure = pci::msi_config_error::none;
    outcome_uncertain = false;
    if (!validate_msi_route_binding(identity, location, previous, failure, false))
        return false;

    const acpi_boot::mcfg_region *region = nullptr;
    if (!find_function_ecam_region(identity, region))
    {
        failure = pci::msi_config_error::resource_unavailable;
        return false;
    }

    uctx::RawSpinLockUninterruptibleContext config_guard(pci_dma_config_lock);
    ecam_bus_session session;
    if (!session.open(*region, static_cast<u8>(identity.bus)))
    {
        failure = pci::msi_config_error::resource_unavailable;
        return false;
    }
    ecam_msi_config_context context{&session.mapping(), static_cast<u8>(identity.bus), static_cast<u8>(identity.device),
                                    static_cast<u8>(identity.function)};
    bool restored = false;
    if (previous.mode == pci::interrupt_route_config_mode::conventional_msi)
    {
        const pci::msi_config_ops operations{read_ecam_msi_config32, write_ecam_msi_config32, &context};
        restored = pci::restore_msi_configuration(operations, previous.msi, failure);
    }
    else
    {
        context.msix_table = previous.table_mapping;
        const pci::msix_config_ops operations{read_ecam_msi_config32,
                                              write_ecam_msi_config32,
                                              read_ecam_msix_table32,
                                              write_ecam_msix_table32,
                                              &context,
                                              previous.table_mapping.table_bytes};
        restored = pci::restore_msix_configuration(operations, previous.msix, failure);
    }
    if (!session.close())
    {
        failure = pci::msi_config_error::hardware_outcome_unknown;
        outcome_uncertain = true;
        return false;
    }
    if (restored && previous.mode == pci::interrupt_route_config_mode::msix &&
        !unmap_msix_table(previous.table_mapping))
    {
        failure = pci::msi_config_error::hardware_outcome_unknown;
        outcome_uncertain = true;
        return false;
    }
    outcome_uncertain = failure == pci::msi_config_error::hardware_outcome_unknown;
    return restored;
}

static pci::msi_route_teardown_result shutdown_pci_msi_route(const pci::function_identity &identity,
                                                             const pci_msi_route &location,
                                                             const pci::interrupt_route_config_snapshot &previous)
{
    pci::msi_config_error binding_failure = pci::msi_config_error::none;
    if (!validate_msi_route_binding(identity, location, previous, binding_failure))
        return {pci::msi_route_teardown_failure::route_binding_invalid, true};

    u32 destination_cpu = ~u32{0};
    for (u32 cpu_id = 0; cpu_id < arch::cpu::count(); ++cpu_id)
    {
        if (arch::cpu::get(cpu_id).get_apic_id() != location.destination_apic_id)
            continue;
        if (destination_cpu != ~u32{0})
            return {pci::msi_route_teardown_failure::destination_cpu_unavailable, true};
        destination_cpu = cpu_id;
    }
    if (destination_cpu == ~u32{0})
        return {pci::msi_route_teardown_failure::destination_cpu_unavailable, true};

    msi_route_teardown_context context{&identity, &location, &previous, destination_cpu};
    const pci::msi_route_teardown_ops operations{disable_msi_route_step, drain_msi_route_step, retire_msi_route_step,
                                                 restore_msi_route_step, &context};
    return pci::teardown_msi_route(operations);
}

bool interrupt_remapping_ready() { return iommu::kernel_provider_router().interrupt_remapping_ready(); }

bool enable_interrupt_remapping()
{
    if (!iommu::kernel_provider_router().enable_interrupt_remapping())
        return false;
    KLOG_INFO("ACPI: IOMMU interrupt remapping enabled");
    return true;
}

bool interrupt_remapping_enabled() { return iommu::kernel_provider_router().interrupt_remapping_enabled(); }

bool interrupt_remapping_activation_uncertain()
{
    return iommu::kernel_provider_router().interrupt_remapping_activation_uncertain();
}

static handle_t<pci::discovery_fabric> get_pci_discovery_fabric()
{
    return !pci_runtime_active || pci_discovery_object == nullptr ? handle_t<pci::discovery_fabric>()
                                                                  : *pci_discovery_object;
}

static pci::dma_operation_result create_pci_dma_domain(const pci::function_identity &identity, process_id owner,
                                                       handle_t<pci::dma_domain> &result,
                                                       handle_t<pci::dma_fault_source> &fault_source)
{
    result.reset();
    fault_source.reset();
    if (owner == 0)
        return NA_STATUS_INVALID_ARGUMENT;
    if (pci_inventory.mcfg_regions == nullptr || identity.platform_root_id == 0 ||
        identity.boot_epoch != pci_inventory.boot_epoch ||
        identity.firmware_generation != pci_inventory.firmware_generation ||
        identity.pci_fabric_generation != pci_inventory.pci_fabric_generation ||
        identity.aml_namespace_generation == 0 || identity.platform_topology_generation == 0 ||
        identity.segment > 0xffff || identity.bus > 0xff || identity.device >= 32 || identity.function >= 8)
        return NA_STATUS_INVALID_ARGUMENT;
    if (!pci::topology_lifetime().is_current(identity.boot_epoch, identity.firmware_generation,
                                             identity.aml_namespace_generation, identity.pci_fabric_generation,
                                             identity.platform_topology_generation))
        return NA_STATUS_OBJECT_REVOKED;

    const iommu::pci_function_location function{static_cast<u16>(identity.segment), static_cast<u8>(identity.bus),
                                                static_cast<u8>(identity.device), static_cast<u8>(identity.function)};
    const auto creation = iommu::kernel_provider_router().create_dma_domain(function, identity.firmware_generation,
                                                                            identity, owner, result, fault_source);
    return {creation.status, creation.outcome_uncertain};
}

static bool read_pci_config(const pci::root_binding &binding, const pci::function_identity &identity, u16 offset,
                            u8 width, u32 &value)
{
    const auto *inventory = &pci_inventory;
    if (inventory->mcfg_regions == nullptr || hardware_ledger == nullptr || memory::kernel_vm_info == nullptr ||
        pci::validate_config_read(offset, width) != pci::config_read_error::none ||
        binding.region_index >= inventory->mcfg_region_count || binding.platform_root_id == 0 ||
        binding.boot_epoch != inventory->boot_epoch || binding.firmware_generation != inventory->firmware_generation ||
        binding.pci_fabric_generation != inventory->pci_fabric_generation ||
        identity.platform_root_id != binding.platform_root_id || identity.segment != binding.segment ||
        identity.boot_epoch != binding.boot_epoch || identity.firmware_generation != binding.firmware_generation ||
        identity.aml_namespace_generation != binding.aml_namespace_generation ||
        identity.platform_topology_generation != binding.platform_topology_generation ||
        identity.pci_fabric_generation != binding.pci_fabric_generation || identity.bus > 0xff ||
        identity.device >= 32 || identity.function >= 8)
        return false;

    const auto &region = inventory->mcfg_regions[binding.region_index];
    pci::ecam_scope bound_scope{};
    pci::ecam_scope function_page{};
    if (region.segment != binding.segment || region.segment != identity.segment || region.resource_lease_id == 0 ||
        !pci::scope_ecam_region(region, binding.ecam.start_bus, binding.ecam.end_bus, bound_scope) ||
        bound_scope.base != binding.ecam.base || bound_scope.length != binding.ecam.length ||
        !pci::scope_ecam_function_page(region, binding.ecam.start_bus, binding.ecam.end_bus, identity.bus,
                                       identity.device, identity.function, function_page))
        return false;

    uctx::RawSpinLockUninterruptibleContext guard(pci_dma_config_lock);
    ecam_bus_session session;
    if (!session.open(region, static_cast<u8>(identity.bus)))
        return false;

    volatile byte *address = nullptr;
    if (!find_ecam_config_register(&session.mapping(), static_cast<u8>(identity.bus), static_cast<u8>(identity.device),
                                   static_cast<u8>(identity.function), offset, width, address))
    {
        (void)session.close();
        return false;
    }

    u32 read_value = 0;
    _mfence();
    switch (width)
    {
        case 1:
            read_value = *reinterpret_cast<volatile u8 *>(address);
            break;
        case 2:
            read_value = *reinterpret_cast<volatile u16 *>(address);
            break;
        case 4:
            read_value = *reinterpret_cast<volatile u32 *>(address);
            break;
        default:
            (void)session.close();
            return false;
    }
    _mfence();
    if (!session.close())
        return false;

    value = read_value;
    return true;
}

} // namespace pci_runtime

bool resolve_ioapic_interrupt_source(u8 enumeration_id, ioapic_source_identity &identity)
{
    return pci_runtime::resolve_ioapic_interrupt_source(enumeration_id, identity);
}

bool install_ioapic_interrupt_route(u8 enumeration_id, u32 destination_apic_id, u8 vector, bool low_polarity,
                                    bool level_triggered, iommu::ioapic_route_encoding &route)
{
    return pci_runtime::install_ioapic_interrupt_route(enumeration_id, destination_apic_id, vector, low_polarity,
                                                       level_triggered, route);
}

bool disable_ioapic_interrupt_route(const iommu::interrupt_route_token &route)
{
    return pci_runtime::disable_ioapic_interrupt_route(route);
}

bool interrupt_remapping_ready() { return pci_runtime::interrupt_remapping_ready(); }

bool enable_interrupt_remapping() { return pci_runtime::enable_interrupt_remapping(); }

bool interrupt_remapping_enabled() { return pci_runtime::interrupt_remapping_enabled(); }

bool interrupt_remapping_activation_uncertain() { return pci_runtime::interrupt_remapping_activation_uncertain(); }

namespace pci_runtime
{
void reset() noexcept
{
    pci_runtime_active = false;
    if (pci_discovery_object != nullptr)
    {
        memory::Delete(memory::KernelCommonAllocatorV, pci_discovery_object);
        pci_discovery_object = nullptr;
    }
    pci_inventory = {};
    pci_dma_quiescence_verified = false;
    for (auto &report : provider_initialization_reports)
        report = {};
    for (auto &state : provider_fault_irq_states)
        state = {};
    provider_fault_soft_irq_setup_attempted = false;
}

void activate() noexcept { pci_runtime_active = pci_discovery_object != nullptr; }

bool prepare_firmware_inventory(acpi_boot::snapshot &snapshot, physical_resource::root &resource_root)
{
    pci_inventory = {snapshot.boot_epoch, snapshot.firmware_generation, snapshot.pci_fabric_generation,
                     snapshot.mcfg_regions.cspan().get(), snapshot.mcfg_regions.size()};
    for (auto &region : snapshot.mcfg_regions)
    {
        const u64 bytes = (static_cast<u64>(region.end_bus) - region.start_bus + 1) << 20;
        physical_resource::lease lease{};
        if (!resource_root.resources()->reserve(
                resource_root, region.base, bytes, region.base, region.base + bytes, physical_resource::kind::pci_ecam,
                physical_resource::cache::uncacheable, physical_resource::access::read_write,
                physical_resource::ledger::kernel_owner(), physical_resource::sharing::exclusive,
                snapshot.firmware_generation, lease) ||
            !hardware_ledger->transition(lease.id, physical_resource::state::reserved,
                                         physical_resource::state::leased))
            return false;
        region.resource_lease_id = lease.id;
    }
    return true;
}

bool create_discovery_fabric(physical_resource::root &resource_root)
{
    pci_discovery_object = memory::New<handle_t<pci::discovery_fabric>>(
        memory::KernelCommonAllocatorV,
        handle_t<pci::discovery_fabric>::make(&pci_inventory, hardware_ledger, &resource_root));
    return pci_discovery_object != nullptr && static_cast<bool>(*pci_discovery_object);
}

bool quiesce_dma(acpi_boot::snapshot &snapshot, u32 &functions_seen, u32 &bus_masters_disabled,
                 pci::dma_quiesce::error &failure)
{
    return quiesce_pci_dma(snapshot, functions_seen, bus_masters_disabled, failure);
}

bool initialize_providers(acpi_boot::snapshot &snapshot, bool pci_dma_quiesced, physical_resource::root &resource_root,
                          range_validator validate_device_range, range_validator validate_register_range)
{
    pci_dma_quiescence_verified = pci_dma_quiesced;
    initialize_iommu_providers_impl(snapshot, pci_dma_quiesced, &resource_root, validate_device_range,
                                    validate_register_range);
    if (!pci::install_platform_operations(pci_platform_operations))
    {
        KLOG_WARN("ACPI: unable to install PCI platform operations");
        return false;
    }
    return true;
}

void enable_fault_interrupts()
{
    for (u32 provider_index = static_cast<u32>(iommu::provider_kind::intel_vtd);
         provider_index <= static_cast<u32>(iommu::provider_kind::amd_vi); ++provider_index)
        initialize_provider_fault_interrupts(static_cast<iommu::provider_kind>(provider_index));
}

void start_deferred_services()
{
    const iommu::fault_quiesce_ops quiesce_ops{nullptr, disable_pci_requester_bus_master};
    for (u32 provider_index = static_cast<u32>(iommu::provider_kind::intel_vtd);
         provider_index <= static_cast<u32>(iommu::provider_kind::amd_vi); ++provider_index)
    {
        const auto provider = static_cast<iommu::provider_kind>(provider_index);
        const auto &initialization = provider_initialization_reports[provider_index];
        if (initialization.status != NA_STATUS_OK ||
            initialization.fault_delivery != iommu::fault_delivery_mode::polling)
            continue;
        if (!iommu::start_provider_fault_polling(provider, quiesce_ops))
            KLOG_WARN("ACPI: IOMMU DMA provider remains unavailable because fault polling could not start provider={}",
                      provider_index);
    }
}
} // namespace pci_runtime
} // namespace arch::ACPI
