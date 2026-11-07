#include "invocation_pci.hpp"
#include "invocation_region_support.hpp"
#include "kernel/acpi/fixed_hardware.hpp"
#include "kernel/acpi/resource_authority.hpp"
#include "kernel/arch/acpi/acpi.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/errno.hpp"
#include "kernel/firmware_root.hpp"
#include "kernel/firmware_version.hpp"
#include "kernel/ipc/channel.hpp"
#include "kernel/ipc/invocation.hpp"
#include "kernel/log.hpp"
#include "kernel/mm/data_plane.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/pci/discovery_fabric.hpp"
#include "kernel/pci/dma_domain.hpp"
#include "kernel/pci/dma_fault_source.hpp"
#include "kernel/pci/dma_status.hpp"
#include "kernel/pci/interrupt_source.hpp"
#include "kernel/pci/topology_lifecycle.hpp"
#include "kernel/service_directory.hpp"
#include "kernel/task.hpp"
#include "kernel/timer.hpp"
#include "kernel/ucontext.hpp"
#include "kernel/usercopy.hpp"
#include "naos/generated/system/AcpiFixedHardware.hpp"
#include "naos/generated/system/AcpiResourceAuthority.hpp"
#include "naos/generated/system/FirmwareRoot.hpp"
#include "naos/generated/system/InterruptSource.hpp"
#include "naos/generated/system/InterruptSource_kernel_dispatch.hpp"
#include "naos/generated/system/MemoryObject.hpp"
#include "naos/generated/system/PciDeviceResourceAttestation.hpp"
#include "naos/generated/system/PciDeviceResourceAttestation_kernel_dispatch.hpp"
#include "naos/generated/system/PciDiscoveryFabric.hpp"
#include "naos/generated/system/PciDiscoveryFabric_kernel_dispatch.hpp"
#include "naos/generated/system/PciDmaDomain.hpp"
#include "naos/generated/system/PciDmaDomain_kernel_dispatch.hpp"
#include "naos/generated/system/PciDmaFaultSource.hpp"
#include "naos/generated/system/PciDmaFaultSource_kernel_dispatch.hpp"
#include "naos/generated/system/PciFabricAdmin.hpp"
#include "naos/generated/system/PciFabricAdmin_kernel_dispatch.hpp"
#include "naos/generated/system/PciFunction.hpp"
#include "naos/generated/system/PciFunction_kernel_dispatch.hpp"
#include "naos/generated/system/PciRootResourceAttestation.hpp"
#include "naos/generated/system/PciRootResourceAttestation_kernel_dispatch.hpp"
#include "naos/generated/system/Process.hpp"
#include "naos/generated/system/ServiceDirectory.hpp"
#include "naos/generated/system/Stream.hpp"
#include "naos/generated/system_uapi.h"
#include <limits>

KLOG_MODULE(ipc);

namespace naos::ipc
{
namespace
{
using dispatch_detail::decode_message;
using dispatch_detail::empty_bytes;
using dispatch_detail::empty_resources;
using dispatch_detail::encode_message;
using dispatch_detail::kernel_view_metadata;
using dispatch_detail::request_region_object;

void release_pci_physical_lease(u64 resource_id)
{
    (void)arch::ACPI::unmap_resource_kernel(resource_id);
    (void)arch::ACPI::revoke_resource_kernel(resource_id);
}

bool read_pci_ecam_dword(void *, u64 page_base, u32 offset, u32 &value)
{
    if ((page_base & (memory::page_size - 1)) != 0 || (offset & 3u) != 0 || offset > 4092 ||
        page_base > ~u64(0) - offset)
        return false;
    auto *address = memory::pa2va<volatile u32 *>(phy_addr_t::from(page_base + offset));
    value = *address;
    return true;
}

bool read_pci_ecam_function_config32(void *context, u16 offset, u32 &value)
{
    const auto *function_page = static_cast<const pci::ecam_scope *>(context);
    return function_page != nullptr && read_pci_ecam_dword(nullptr, function_page->base, offset, value);
}

struct pci_discovery_kernel_handler
{
    pci::discovery_fabric &fabric;
    task::process_t *caller;
    firmware::version_storage version_storage{};

    na_status_t invalid_request() { return NA_STATUS_INVALID_MESSAGE; }

    kernel_dispatch::method_result list_regions(const naos::system::PciDiscoveryFabric::list_regions_request &,
                                                naos::system::PciDiscoveryFabric::list_regions_response &response,
                                                const capability::transfer_record_list &,
                                                capability::transfer_record_list &)
    {
        const auto *inventory = fabric.inventory();
        if (inventory == nullptr)
            return kernel_dispatch::method_result::error(EIO);
        response.regions.data = response.regions_storage.data();
        response.regions.count = static_cast<u32>(inventory->mcfg_region_count);
        if (response.regions.count > 64)
            return kernel_dispatch::method_result::error(ENOMEM);
        firmware::write_version(response.firmware_version, version_storage, inventory->boot_epoch,
                                inventory->firmware_generation);
        for (u32 index = 0; index < response.regions.count; index++)
        {
            const auto *region = inventory->region(index);
            if (region == nullptr)
                return kernel_dispatch::method_result::error(EIO);
            auto &value = response.regions.data[index];
            value.segment = region->segment;
            value.start_bus = region->start_bus;
            value.end_bus = region->end_bus;
            value.bytes = (static_cast<u64>(region->end_bus) - region->start_bus + 1) << 20;
            value.pci_fabric_generation = inventory->pci_fabric_generation;
        }
        return kernel_dispatch::method_result::success();
    }

    kernel_dispatch::method_result bind_root(const naos::system::PciDiscoveryFabric::bind_root_request &request,
                                             naos::system::PciDiscoveryFabric::bind_root_response &response,
                                             capability::transfer_record_list &resources,
                                             capability::transfer_record_list &response_resources)
    {
        const auto *inventory = fabric.inventory();
        if (inventory == nullptr)
            return kernel_dispatch::method_result::error(EIO);
        if (request.index >= inventory->mcfg_region_count)
            return kernel_dispatch::method_result::error(EINVAL);
        u64 boot_epoch = 0;
        u64 firmware_generation = 0;
        if (!firmware::decode_version(request.firmware_version, boot_epoch, firmware_generation) ||
            boot_epoch != inventory->boot_epoch || firmware_generation != inventory->firmware_generation ||
            request.pci_fabric_generation != inventory->pci_fabric_generation)
            return kernel_dispatch::method_result::error(ESTALE);
        if (caller == nullptr || caller->pid == 0 || fabric.ledger() == nullptr)
            return kernel_dispatch::method_result::error(EACCES);
        if (request.attestation.value >= resources.size())
            return kernel_dispatch::method_result::error(EACCES);
        auto &attestation_record = resources.at(request.attestation.value);
        if (!attestation_record.resource.valid())
            return kernel_dispatch::method_result::error(EACCES);
        const auto &metadata = attestation_record.resource.meta();
        if (metadata.binding != NA_BINDING_KERNEL_VIEW || metadata.scope != NA_SCOPE_PCI_ROOT_RESOURCE_ATTESTATION ||
            metadata.revision != naos::system::PciRootResourceAttestation::revision ||
            metadata.features != naos::system::PciRootResourceAttestation::features ||
            memcmp(metadata.protocol_uuid.bytes, naos::system::PciRootResourceAttestation::protocol_uuid.bytes,
                   sizeof(metadata.protocol_uuid.bytes)) != 0 ||
            (metadata.meta_rights & NA_RIGHT_TRANSFER) == 0)
            return kernel_dispatch::method_result::error(EACCES);
        auto *attestation = attestation_record.resource.object()->get<pci::root_resource_attestation_capability>();
        if (attestation == nullptr)
            return kernel_dispatch::method_result::error(EACCES);

        const auto *region = inventory->region(request.index);
        if (region == nullptr)
            return kernel_dispatch::method_result::error(ESTALE);
        pci::root_binding_request binding_request{};
        binding_request.region_index = request.index;
        binding_request.platform_root_id = request.platform_root_id;
        binding_request.boot_epoch = boot_epoch;
        binding_request.firmware_generation = firmware_generation;
        binding_request.aml_namespace_generation = request.aml_namespace_generation;
        binding_request.platform_topology_generation = request.platform_topology_generation;
        binding_request.pci_fabric_generation = request.pci_fabric_generation;
        binding_request.segment = request.segment;
        binding_request.start_bus = request.start_bus;
        binding_request.end_bus = request.end_bus;
        pci::root_binding binding{};
        if (!pci::make_root_binding(*inventory, *region, binding_request, attestation->authority(), binding))
            return kernel_dispatch::method_result::error(EINVAL);
        if (!pci::topology_lifetime().bind_generation(binding.boot_epoch, binding.firmware_generation,
                                                      binding.aml_namespace_generation, binding.pci_fabric_generation,
                                                      binding.platform_topology_generation))
            return kernel_dispatch::method_result::error(ESTALE);

        auto admin =
            handle_t<pci::fabric_admin>::make(inventory, fabric.ledger(), fabric.resource_root(), caller->pid, binding);
        if (!admin)
            return kernel_dispatch::method_result::error(ENOMEM);
        auto admin_metadata = kernel_view_metadata(
            NA_SCOPE_PCI_FABRIC_ADMIN, naos::system::PciFabricAdmin::protocol_uuid, NA_RIGHT_WAIT | NA_RIGHT_INSPECT,
            NA_PROTOCOL_RIGHT_INVOKE, naos::system::PciFabricAdmin::revision, naos::system::PciFabricAdmin::features);
        response.admin.value = 0;
        response_resources.push_back(capability::transfer_record(
            NA_HANDLE_INVALID, true,
            capability::transferred_resource(handle_t<kobject>(admin.get_control()), admin_metadata)));
        return kernel_dispatch::method_result::success();
    }
};

na_status_t publish_pci_discovery_call(invocation_state &state, pci::discovery_fabric &fabric, u64 method_id,
                                       const freelibcxx::vector<byte> &request,
                                       capability::transfer_record_list &resources, task::process_t *caller)
{
    if (fabric.inventory() == nullptr)
        return state.complete_reply(empty_bytes(), empty_resources(), EIO) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
    pci_discovery_kernel_handler handler{fabric, caller};
    return naos::system::PciDiscoveryFabric::PciDiscoveryFabricKernelDispatcher::dispatch(handler, state, method_id,
                                                                                          request, resources);
}

struct pci_fabric_admin_kernel_handler
{
    pci::fabric_admin &admin;
    task::process_t *caller;
    firmware::version_storage version_storage{};

    na_status_t invalid_request() { return NA_STATUS_INVALID_MESSAGE; }

    kernel_dispatch::method_result open_function(const naos::system::PciFabricAdmin::open_function_request &request,
                                                 naos::system::PciFabricAdmin::open_function_response &response,
                                                 const capability::transfer_record_list &,
                                                 capability::transfer_record_list &response_resources)
    {
        const auto *inventory = admin.inventory();
        const auto &binding = admin.binding();
        switch (pci::validate_function_selector(binding.ecam.start_bus, binding.ecam.end_bus, request.bus,
                                                request.device, request.function))
        {
            case pci::function_selector_error::none:
                break;
            case pci::function_selector_error::invalid_argument:
                return kernel_dispatch::method_result::error(EINVAL);
            case pci::function_selector_error::out_of_range:
                return kernel_dispatch::method_result::error(ERANGE);
            case pci::function_selector_error::outside_root:
                return kernel_dispatch::method_result::error(EACCES);
        }

        pci::function_identity identity{};
        identity.platform_root_id = binding.platform_root_id;
        identity.segment = binding.segment;
        identity.bus = request.bus;
        identity.device = request.device;
        identity.function = request.function;
        identity.boot_epoch = binding.boot_epoch;
        identity.firmware_generation = binding.firmware_generation;
        identity.aml_namespace_generation = binding.aml_namespace_generation;
        identity.platform_topology_generation = binding.platform_topology_generation;
        identity.pci_fabric_generation = binding.pci_fabric_generation;
        auto function = handle_t<pci::function>::make(caller->pid, inventory, admin.ledger(), admin.resource_root(),
                                                      binding, identity);
        if (!function)
            return kernel_dispatch::method_result::error(ENOMEM);
        auto metadata = kernel_view_metadata(
            NA_SCOPE_PCI_FUNCTION, naos::system::PciFunction::protocol_uuid, NA_RIGHT_WAIT | NA_RIGHT_INSPECT,
            NA_PROTOCOL_RIGHT_INVOKE | NA_PCI_FUNCTION_RIGHT_ROUTE_INTERRUPT | NA_PCI_FUNCTION_RIGHT_READ_CONFIG |
                NA_PCI_FUNCTION_RIGHT_CREATE_DMA_DOMAIN | NA_PCI_FUNCTION_RIGHT_OPEN_BAR |
                NA_PCI_FUNCTION_RIGHT_BIND_DEVICE_RESOURCES | NA_PCI_FUNCTION_RIGHT_GRANT_DRIVER,
            naos::system::PciFunction::revision, naos::system::PciFunction::features);
        response.platform_root_id = identity.platform_root_id;
        response.segment = identity.segment;
        response.bus = identity.bus;
        response.device = identity.device;
        response.function = identity.function;
        firmware::write_version(response.firmware_version, version_storage, identity.boot_epoch,
                                identity.firmware_generation);
        response.aml_namespace_generation = identity.aml_namespace_generation;
        response.platform_topology_generation = identity.platform_topology_generation;
        response.pci_fabric_generation = identity.pci_fabric_generation;
        response.capability.value = 0;
        response_resources.push_back(capability::transfer_record(
            NA_HANDLE_INVALID, true,
            capability::transferred_resource(handle_t<kobject>(function.get_control()), metadata)));
        return kernel_dispatch::method_result::success();
    }

    kernel_dispatch::method_result open_region(const naos::system::PciFabricAdmin::open_region_request &,
                                               naos::system::PciFabricAdmin::open_region_response &response,
                                               const capability::transfer_record_list &,
                                               capability::transfer_record_list &response_resources)
    {
        const auto *inventory = admin.inventory();
        const auto &binding = admin.binding();
        const auto *region = inventory->region(binding.region_index);
        if (region == nullptr)
            return kernel_dispatch::method_result::error(ESTALE);
        pci::ecam_scope scope{};
        if (region->segment != binding.segment ||
            !pci::scope_ecam_region(*region, binding.ecam.start_bus, binding.ecam.end_bus, scope) ||
            scope.base != binding.ecam.base || scope.length != binding.ecam.length)
            return kernel_dispatch::method_result::error(ESTALE);

        auto *ledger = admin.ledger();
        const u64 resource_owner = admin.owner();
        physical_resource::lease scoped_lease{};
        if (!ledger->reserve(region->resource_lease_id, scope.base, scope.length, scope.base, scope.base + scope.length,
                             physical_resource::kind::pci_ecam, physical_resource::cache::uncacheable,
                             physical_resource::access::read, resource_owner,
                             physical_resource::sharing::shared_read_only, inventory->firmware_generation,
                             scoped_lease))
            return kernel_dispatch::method_result::error(EACCES);
        if (!ledger->transition(scoped_lease.id, physical_resource::state::reserved, physical_resource::state::leased,
                                resource_owner))
        {
            (void)ledger->revoke_kernel(scoped_lease.id);
            return kernel_dispatch::method_result::error(EACCES);
        }
        if (!ledger->map(scoped_lease.id, scope.base, scope.length, physical_resource::kind::pci_ecam,
                         physical_resource::cache::uncacheable, physical_resource::access::read,
                         inventory->firmware_generation, resource_owner))
        {
            (void)ledger->revoke_kernel(scoped_lease.id);
            return kernel_dispatch::method_result::error(EACCES);
        }
        auto object = handle_t<data_plane::memory_object>::make(
            phy_addr_t::from(scope.base), memory::pa2va<byte *>(phy_addr_t::from(scope.base)), scope.length,
            NA_MEMORY_FLAG_READ_ONLY, release_pci_physical_lease, scoped_lease.id);
        if (!object)
        {
            (void)ledger->unmap_kernel(scoped_lease.id);
            (void)ledger->revoke_kernel(scoped_lease.id);
            return kernel_dispatch::method_result::error(ENOMEM);
        }
        auto metadata = kernel_view_metadata(
            NA_SCOPE_MEMORY_OBJECT, naos::system::MemoryObject::protocol_uuid, NA_RIGHT_WAIT | NA_RIGHT_INSPECT,
            NA_MEMORY_RIGHT_READ | NA_MEMORY_RIGHT_MAP | NA_MEMORY_RIGHT_INFO, naos::system::MemoryObject::revision,
            naos::system::MemoryObject::features);
        metadata.binding = NA_BINDING_MEMORY_OBJECT;
        metadata.view_offset = 0;
        metadata.view_length = scope.length;
        response.segment = region->segment;
        response.start_bus = scope.start_bus;
        response.end_bus = scope.end_bus;
        response.bytes = scope.length;
        firmware::write_version(response.firmware_version, version_storage, inventory->boot_epoch,
                                inventory->firmware_generation);
        response.pci_fabric_generation = inventory->pci_fabric_generation;
        response.ecam.value = 0;
        response_resources.push_back(capability::transfer_record(
            NA_HANDLE_INVALID, true, capability::transferred_resource(std::move(object), metadata)));
        return kernel_dispatch::method_result::success();
    }
};

na_status_t publish_pci_fabric_admin_call(invocation_state &state, pci::fabric_admin &admin, u64 method_id,
                                          const freelibcxx::vector<byte> &request, task::process_t *caller)
{
    const auto *inventory = admin.inventory();
    const auto &binding = admin.binding();
    if (caller == nullptr || caller->pid != admin.owner())
        return NA_STATUS_ACCESS_DENIED;
    if (inventory == nullptr || admin.ledger() == nullptr || binding.region_index >= inventory->mcfg_region_count ||
        binding.boot_epoch != inventory->boot_epoch || binding.firmware_generation != inventory->firmware_generation ||
        binding.pci_fabric_generation != inventory->pci_fabric_generation)
        return state.complete_reply(empty_bytes(), empty_resources(), ESTALE) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
    if (!pci::topology_lifetime().is_current(binding.boot_epoch, binding.firmware_generation,
                                             binding.aml_namespace_generation, binding.pci_fabric_generation,
                                             binding.platform_topology_generation))
        return state.complete_reply(empty_bytes(), empty_resources(), ESTALE) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;

    pci_fabric_admin_kernel_handler handler{admin, caller};
    capability::transfer_record_list resources(memory::KernelCommonAllocatorV);
    return naos::system::PciFabricAdmin::PciFabricAdminKernelDispatcher::dispatch(handler, state, method_id, request,
                                                                                  resources);
}

struct pci_function_kernel_handler
{
    pci::function &function;
    task::process_t *caller;
    firmware::version_storage version_storage{};

    na_status_t invalid_request() { return NA_STATUS_INVALID_MESSAGE; }

    kernel_dispatch::method_result read_config(const naos::system::PciFunction::read_config_request &request,
                                               naos::system::PciFunction::read_config_response &response,
                                               const capability::transfer_record_list &,
                                               capability::transfer_record_list &)
    {
        if (pci::validate_config_read(request.offset, request.width) != pci::config_read_error::none)
            return kernel_dispatch::method_result::error(EINVAL);
        const auto *inventory = function.inventory();
        const auto &binding = function.binding();
        const auto &identity = function.identity();
        if (inventory == nullptr || binding.region_index >= inventory->mcfg_region_count ||
            binding.boot_epoch != inventory->boot_epoch ||
            binding.firmware_generation != inventory->firmware_generation ||
            binding.pci_fabric_generation != inventory->pci_fabric_generation ||
            identity.platform_root_id != binding.platform_root_id || identity.segment != binding.segment ||
            identity.boot_epoch != binding.boot_epoch || identity.firmware_generation != binding.firmware_generation ||
            identity.aml_namespace_generation != binding.aml_namespace_generation ||
            identity.platform_topology_generation != binding.platform_topology_generation ||
            identity.pci_fabric_generation != binding.pci_fabric_generation || identity.bus > 0xff ||
            identity.device >= 32 || identity.function >= 8)
            return kernel_dispatch::method_result::error(ESTALE);
        if (!pci::read_config(binding, identity, request.offset, request.width, response.value))
            return kernel_dispatch::method_result::error(EIO);
        return kernel_dispatch::method_result::success();
    }

    kernel_dispatch::method_result
    bind_device_resources(const naos::system::PciFunction::bind_device_resources_request &request,
                          naos::system::PciFunction::bind_device_resources_response &,
                          capability::transfer_record_list &resources, capability::transfer_record_list &)
    {
        if (request.authority.value >= resources.size())
            return kernel_dispatch::method_result::error(EINVAL);
        auto &record = resources.at(request.authority.value);
        if (!record.resource.valid())
            return kernel_dispatch::method_result::error(EACCES);
        const auto &metadata = record.resource.meta();
        if (metadata.binding != NA_BINDING_KERNEL_VIEW || metadata.scope != NA_SCOPE_PCI_DEVICE_RESOURCE_ATTESTATION ||
            metadata.revision != naos::system::PciDeviceResourceAttestation::revision ||
            metadata.features != naos::system::PciDeviceResourceAttestation::features ||
            memcmp(metadata.protocol_uuid.bytes, naos::system::PciDeviceResourceAttestation::protocol_uuid.bytes,
                   sizeof(metadata.protocol_uuid.bytes)) != 0 ||
            (metadata.meta_rights & NA_RIGHT_TRANSFER) == 0)
            return kernel_dispatch::method_result::error(EACCES);
        auto *device_attestation = record.resource.object()->get<pci::device_resource_attestation_capability>();
        if (device_attestation == nullptr)
            return kernel_dispatch::method_result::error(EACCES);

        const auto &claimed = device_attestation->authority();
        const auto &identity = function.identity();
        const auto &binding = function.binding();
        const auto *inventory = function.inventory();
        if (request.device_id == 0 || claimed.device_id != request.device_id || inventory == nullptr ||
            function.ledger() == nullptr || binding.region_index >= inventory->mcfg_region_count ||
            claimed.root_id != identity.platform_root_id || claimed.segment != identity.segment ||
            claimed.boot_epoch != identity.boot_epoch || claimed.firmware_generation != identity.firmware_generation ||
            claimed.aml_namespace_generation != identity.aml_namespace_generation ||
            claimed.platform_topology_generation != identity.platform_topology_generation ||
            claimed.pci_fabric_generation != identity.pci_fabric_generation)
            return kernel_dispatch::method_result::error(ESTALE);

        pci::device_resource_evidence evidence{};
        static_cast<pci::root_feature_scope &>(evidence) = static_cast<const pci::root_feature_scope &>(claimed);
        evidence.device_id = claimed.device_id;
        evidence.path_length = claimed.path_length;
        evidence.device_function_path = claimed.device_function_path;
        evidence.memory_window_count = claimed.memory_window_count;
        evidence.memory_windows = claimed.memory_windows;
        pci::device_resource_attestation validated{};
        if (pci::validate_device_resource_evidence(evidence, binding.resources, validated) !=
            pci::device_resource_error::none)
            return kernel_dispatch::method_result::error(EACCES);

        const auto &region = inventory->mcfg_regions[binding.region_index];
        pci::ecam_scope resolved_path{};
        pci::ecam_scope selected_function{};
        if (!pci::resolve_ecam_function_path(region, binding.ecam.start_bus, binding.ecam.end_bus,
                                             validated.device_function_path.data(), validated.path_length,
                                             read_pci_ecam_dword, nullptr, resolved_path) ||
            !pci::scope_ecam_function_page(region, binding.ecam.start_bus, binding.ecam.end_bus, identity.bus,
                                           identity.device, identity.function, selected_function) ||
            resolved_path.base != selected_function.base)
            return kernel_dispatch::method_result::error(EACCES);
        if (!function.bind_device_resources(validated))
            return kernel_dispatch::method_result::error(EBUSY);
        return kernel_dispatch::method_result::success();
    }

    kernel_dispatch::method_result open_bar(const naos::system::PciFunction::open_bar_request &request,
                                            naos::system::PciFunction::open_bar_response &response,
                                            const capability::transfer_record_list &,
                                            capability::transfer_record_list &response_resources)
    {
        pci::device_resource_attestation device_resources{};
        if (!function.get_device_resources(device_resources) || function.inventory() == nullptr ||
            function.ledger() == nullptr || function.resource_root() == nullptr ||
            function.resource_root()->resources() != function.ledger())
            return kernel_dispatch::method_result::error(EACCES);

        const auto &identity = function.identity();
        const auto &binding = function.binding();
        const auto *inventory = function.inventory();
        if (binding.region_index >= inventory->mcfg_region_count)
            return kernel_dispatch::method_result::error(ESTALE);
        const auto &region = inventory->mcfg_regions[binding.region_index];
        pci::ecam_scope function_page{};
        if (!pci::scope_ecam_function_page(region, binding.ecam.start_bus, binding.ecam.end_bus, identity.bus,
                                           identity.device, identity.function, function_page))
            return kernel_dispatch::method_result::error(ESTALE);

        u32 command_and_status = 0;
        if (!read_pci_ecam_dword(nullptr, function_page.base, 0x04, command_and_status))
            return kernel_dispatch::method_result::error(EIO);
        if ((command_and_status & (1u << 1)) == 0)
            return kernel_dispatch::method_result::error(ENODEV);

        u32 header = 0;
        if (!read_pci_ecam_dword(nullptr, function_page.base, 0x0c, header))
            return kernel_dispatch::method_result::error(EIO);
        const u32 header_type = (header >> 16) & 0x7fu;
        const u8 bar_count = header_type == 0 ? 6 : header_type == 1 ? 2 : 0;
        if (bar_count == 0 || request.index >= bar_count)
            return kernel_dispatch::method_result::error(header_type > 1 ? ENOTSUP : EINVAL);
        if (function.is_driver_grant() && !pci::driver_profile_allows_bar(function.driver_profile(), request.index))
            return kernel_dispatch::method_result::error(EACCES);

        switch (pci::validate_msix_bar_mapping(read_pci_ecam_function_config32, &function_page, request.index))
        {
            case pci::msix_bar_mapping_error::none:
                break;
            case pci::msix_bar_mapping_error::table_bar_reserved:
                return kernel_dispatch::method_result::error(EACCES);
            case pci::msix_bar_mapping_error::config_read_failed:
                return kernel_dispatch::method_result::error(EIO);
            case pci::msix_bar_mapping_error::invalid_bar_index:
                return kernel_dispatch::method_result::error(EINVAL);
            case pci::msix_bar_mapping_error::invalid_capability:
                return kernel_dispatch::method_result::error(ENOTSUP);
        }

        for (u32 preceding = 0; preceding < request.index;)
        {
            u32 preceding_low = 0;
            if (!read_pci_ecam_dword(nullptr, function_page.base, 0x10u + preceding * sizeof(u32), preceding_low))
                return kernel_dispatch::method_result::error(EIO);
            if ((preceding_low & (3u << 1)) == (2u << 1))
            {
                if (preceding + 1 == request.index)
                    return kernel_dispatch::method_result::error(EINVAL);
                preceding += 2;
            }
            else
                ++preceding;
        }

        u32 low = 0;
        const u32 offset = 0x10u + request.index * sizeof(u32);
        if (!read_pci_ecam_dword(nullptr, function_page.base, offset, low))
            return kernel_dispatch::method_result::error(EIO);
        const bool is_64_bit = (low & (3u << 1)) == (2u << 1);
        bool upper_available = false;
        u32 high = 0;
        if (is_64_bit && request.index + 1 < bar_count)
        {
            upper_available = read_pci_ecam_dword(nullptr, function_page.base, offset + sizeof(u32), high);
            if (!upper_available)
                return kernel_dispatch::method_result::error(EIO);
        }
        pci::bar_descriptor bar{};
        switch (pci::decode_bar(bar_count, static_cast<u8>(request.index), low, upper_available, high, bar))
        {
            case pci::bar_decode_error::none:
                break;
            case pci::bar_decode_error::unassigned:
                return kernel_dispatch::method_result::error(ENODEV);
            case pci::bar_decode_error::io_space_unsupported:
            case pci::bar_decode_error::memory_below_1mb_unsupported:
            case pci::bar_decode_error::reserved_memory_type:
                return kernel_dispatch::method_result::error(ENOTSUP);
            default:
                return kernel_dispatch::method_result::error(EINVAL);
        }

        const pci::device_resource_window *window = nullptr;
        for (u32 index = 0; index < device_resources.memory_window_count; ++index)
        {
            if (device_resources.memory_windows[index].base == bar.base)
            {
                window = &device_resources.memory_windows[index];
                break;
            }
        }
        if (window == nullptr)
            return kernel_dispatch::method_result::error(EACCES);
        if (!pci::valid_bar_aperture(bar.base, window->length))
            return kernel_dispatch::method_result::error(ENOTSUP);
        pci::bar_page_cover cover{};
        if (window->length > NA_MEMORY_MAP_MAX_BYTES || window->base > memory::max_memory_support - window->length ||
            !pci::make_bar_page_cover(window->base, window->length, cover) || cover.base != window->base ||
            cover.end - cover.base != window->length)
            return kernel_dispatch::method_result::error(ENOTSUP);
        bool within_root = false;
        for (u32 index = 0; index < binding.resources.memory_window_count; ++index)
        {
            const auto &root_window = binding.resources.memory_windows[index];
            if (pci::bar_range_within_window(window->base, window->length, root_window.base, root_window.length))
            {
                within_root = true;
                break;
            }
        }
        if (!within_root)
            return kernel_dispatch::method_result::error(EACCES);

        auto *ledger = function.ledger();
        auto *resource_root = function.resource_root();
        const u64 resource_owner = function.owner();
        const auto mode =
            (window->flags & 1u) != 0 ? physical_resource::access::read_write : physical_resource::access::read;
        physical_resource::lease lease{};
        if (!ledger->reserve(*resource_root, window->base, window->length, cover.base, cover.end,
                             physical_resource::kind::pci_bar, physical_resource::cache::uncacheable, mode,
                             resource_owner, physical_resource::sharing::exclusive, identity.firmware_generation,
                             lease))
            return kernel_dispatch::method_result::error(EBUSY);
        const auto revoke_lease = [&]() { (void)ledger->revoke_kernel(lease.id); };
        if (!ledger->bind_namespace(lease.id, resource_owner, identity.aml_namespace_generation) ||
            !ledger->bind_device(lease.id, resource_owner, device_resources.device_id) ||
            !ledger->transition(lease.id, physical_resource::state::reserved, physical_resource::state::leased,
                                resource_owner))
        {
            revoke_lease();
            return kernel_dispatch::method_result::error(EACCES);
        }
        if (!ledger->map(lease.id, window->base, window->length, physical_resource::kind::pci_bar,
                         physical_resource::cache::uncacheable, mode, identity.firmware_generation, resource_owner))
        {
            revoke_lease();
            return kernel_dispatch::method_result::error(EACCES);
        }
        auto object = handle_t<data_plane::memory_object>::make(
            phy_addr_t::from(window->base), memory::pa2va<byte *>(phy_addr_t::from(window->base)), window->length,
            static_cast<u32>(NA_MEMORY_FLAG_DEVICE_MMIO) |
                (mode == physical_resource::access::read ? static_cast<u32>(NA_MEMORY_FLAG_READ_ONLY) : 0),
            release_pci_physical_lease, lease.id);
        if (!object)
        {
            (void)ledger->unmap_kernel(lease.id);
            revoke_lease();
            return kernel_dispatch::method_result::error(ENOMEM);
        }
        const bool writable = mode == physical_resource::access::read_write;
        auto metadata = kernel_view_metadata(
            NA_SCOPE_MEMORY_OBJECT, naos::system::MemoryObject::protocol_uuid, NA_RIGHT_WAIT | NA_RIGHT_INSPECT,
            NA_MEMORY_RIGHT_READ | NA_MEMORY_RIGHT_MAP | NA_MEMORY_RIGHT_INFO |
                (writable ? static_cast<u64>(NA_MEMORY_RIGHT_WRITE) : 0),
            naos::system::MemoryObject::revision, naos::system::MemoryObject::features);
        metadata.binding = NA_BINDING_MEMORY_OBJECT;
        metadata.view_offset = 0;
        metadata.view_length = window->length;
        response.length = window->length;
        response.mapping.value = 0;
        response_resources.push_back(capability::transfer_record(
            NA_HANDLE_INVALID, true,
            capability::transferred_resource(handle_t<kobject>(object.get_control()), metadata)));
        return kernel_dispatch::method_result::success();
    }

    kernel_dispatch::method_result route_interrupt(const naos::system::PciFunction::route_interrupt_request &,
                                                   naos::system::PciFunction::route_interrupt_response &response,
                                                   const capability::transfer_record_list &,
                                                   capability::transfer_record_list &response_resources)
    {
        handle_t<pci::interrupt_source> source;
        bool outcome_unknown = false;
        const auto status = pci::create_interrupt_source(function, source, outcome_unknown);
        if (outcome_unknown)
            return kernel_dispatch::method_result::failure(NA_EXECUTION_OUTCOME_UNKNOWN,
                                                           NA_OUTCOME_REASON_HARDWARE_OUTCOME_UNKNOWN);
        if (status != NA_STATUS_OK)
        {
            const i64 error = status == NA_STATUS_ACCESS_DENIED        ? EACCES
                              : status == NA_STATUS_INVALID_ARGUMENT   ? EINVAL
                              : status == NA_STATUS_ALREADY_CONSUMED   ? EBUSY
                              : status == NA_STATUS_RESOURCE_EXHAUSTED ? ENOMEM
                              : status == NA_STATUS_NOT_SUPPORTED      ? ENOTSUP
                              : status == NA_STATUS_OBJECT_REVOKED     ? EIDRM
                              : status == NA_STATUS_WOULD_BLOCK        ? EAGAIN
                                                                       : EIO;
            return kernel_dispatch::method_result::error(error);
        }
        constexpr na_meta_rights_t source_meta_rights = NA_RIGHT_WAIT | NA_RIGHT_INSPECT;
        constexpr u64 source_protocol_rights = NA_PROTOCOL_RIGHT_INVOKE | NA_INTERRUPT_SOURCE_RIGHT_BIND |
                                               NA_INTERRUPT_SOURCE_RIGHT_WAIT | NA_INTERRUPT_SOURCE_RIGHT_MASK |
                                               NA_INTERRUPT_SOURCE_RIGHT_ACK;
        auto metadata = kernel_view_metadata(
            NA_SCOPE_INTERRUPT_SOURCE, naos::system::InterruptSource::protocol_uuid, source_meta_rights,
            source_protocol_rights, naos::system::InterruptSource::revision, naos::system::InterruptSource::features);
        response.source.value = 0;
        response_resources.push_back(capability::transfer_record(
            NA_HANDLE_INVALID, true,
            capability::transferred_resource(handle_t<kobject>(source.get_control()), metadata)));
        return kernel_dispatch::method_result::success();
    }

    kernel_dispatch::method_result create_dma_domain(const naos::system::PciFunction::create_dma_domain_request &,
                                                     naos::system::PciFunction::create_dma_domain_response &response,
                                                     const capability::transfer_record_list &,
                                                     capability::transfer_record_list &response_resources)
    {
        handle_t<pci::dma_domain> domain;
        handle_t<pci::dma_fault_source> faults;
        const auto result = pci::create_dma_domain(function.identity(), caller->pid, domain, faults);
        if (result.outcome_unknown)
            return kernel_dispatch::method_result::failure(NA_EXECUTION_OUTCOME_UNKNOWN,
                                                           NA_OUTCOME_REASON_HARDWARE_OUTCOME_UNKNOWN);
        if (result.status != NA_STATUS_OK)
            return kernel_dispatch::method_result::error(pci::dma::status_errno(result.status));
        auto metadata = kernel_view_metadata(
            NA_SCOPE_PCI_DMA_DOMAIN, naos::system::PciDmaDomain::protocol_uuid, pci::dma_capability_meta_rights,
            NA_PROTOCOL_RIGHT_INVOKE, naos::system::PciDmaDomain::revision, naos::system::PciDmaDomain::features);
        auto fault_metadata =
            kernel_view_metadata(NA_SCOPE_PCI_DMA_FAULT_SOURCE, naos::system::PciDmaFaultSource::protocol_uuid,
                                 pci::dma_capability_meta_rights, NA_PROTOCOL_RIGHT_INVOKE,
                                 naos::system::PciDmaFaultSource::revision, naos::system::PciDmaFaultSource::features);
        response.domain.value = 0;
        response.faults.value = 1;
        response_resources.push_back(capability::transfer_record(
            NA_HANDLE_INVALID, true,
            capability::transferred_resource(handle_t<kobject>(domain.get_control()), metadata)));
        response_resources.push_back(capability::transfer_record(
            NA_HANDLE_INVALID, true,
            capability::transferred_resource(handle_t<kobject>(faults.get_control()), fault_metadata)));
        return kernel_dispatch::method_result::success();
    }

    kernel_dispatch::method_result grant_driver(const naos::system::PciFunction::grant_driver_request &request,
                                                naos::system::PciFunction::grant_driver_response &response,
                                                capability::transfer_record_list &resources,
                                                capability::transfer_record_list &response_resources)
    {
        if (function.is_driver_grant() || request.driver_process.value >= resources.size())
            return kernel_dispatch::method_result::error(function.is_driver_grant() ? EACCES : EINVAL);
        auto &process_resource = resources.at(request.driver_process.value).resource;
        if (!process_resource.valid())
            return kernel_dispatch::method_result::error(EACCES);
        const auto &process_meta = process_resource.meta();
        if (process_meta.binding != NA_BINDING_KERNEL_VIEW || process_meta.scope != NA_SCOPE_PROCESS ||
            process_meta.revision != naos::system::Process::revision ||
            process_meta.features != naos::system::Process::features ||
            (process_meta.meta_rights & NA_RIGHT_TRANSFER) == 0 ||
            memcmp(process_meta.protocol_uuid.bytes, naos::system::Process::protocol_uuid.bytes,
                   sizeof(process_meta.protocol_uuid.bytes)) != 0)
            return kernel_dispatch::method_result::error(EACCES);
        auto *process_capability = process_resource.object()->get<task::process_object>();
        auto *driver_process = process_capability == nullptr ? nullptr : process_capability->process();
        if (driver_process == nullptr)
            return kernel_dispatch::method_result::error(EACCES);
        const auto target_error = pci::validate_driver_process_target(
            caller->pid, driver_process->pid,
            driver_process->start_state.load(std::memory_order_acquire) == task::process_start_state::started,
            (driver_process->attributes.load(std::memory_order_acquire) & task::process_attributes::no_thread) != 0);
        if (target_error == pci::driver_target_error::invalid_process ||
            target_error == pci::driver_target_error::same_process)
            return kernel_dispatch::method_result::error(EACCES);
        if (target_error == pci::driver_target_error::already_started ||
            target_error == pci::driver_target_error::exited)
            return kernel_dispatch::method_result::error(EBUSY);

        const pci::driver_function_profile profile{request.profile.rights, request.profile.bar_mask};
        const auto *device_resources = function.device_resources();
        switch (pci::validate_driver_function_profile(profile, device_resources != nullptr))
        {
            case pci::driver_profile_error::none:
                break;
            case pci::driver_profile_error::unknown_rights:
            case pci::driver_profile_error::unsupported_bar:
                return kernel_dispatch::method_result::error(EINVAL);
            case pci::driver_profile_error::bars_require_attestation:
                return kernel_dispatch::method_result::error(ENOTSUP);
        }
        const auto *inventory = function.inventory();
        const auto &binding = function.binding();
        const auto &identity = function.identity();
        if (inventory == nullptr || binding.region_index >= inventory->mcfg_region_count ||
            binding.boot_epoch != inventory->boot_epoch ||
            binding.firmware_generation != inventory->firmware_generation ||
            binding.pci_fabric_generation != inventory->pci_fabric_generation ||
            identity.platform_root_id != binding.platform_root_id || identity.segment != binding.segment ||
            identity.boot_epoch != binding.boot_epoch || identity.firmware_generation != binding.firmware_generation ||
            identity.aml_namespace_generation != binding.aml_namespace_generation ||
            identity.platform_topology_generation != binding.platform_topology_generation ||
            identity.pci_fabric_generation != binding.pci_fabric_generation)
            return kernel_dispatch::method_result::error(ESTALE);
        if (profile.bar_mask != 0)
        {
            const auto &attested = static_cast<const pci::root_feature_scope &>(*device_resources);
            if (device_resources->device_id == 0 || device_resources->path_length == 0 ||
                device_resources->memory_window_count == 0 || attested.root_id != identity.platform_root_id ||
                attested.segment != identity.segment || attested.boot_epoch != identity.boot_epoch ||
                attested.firmware_generation != identity.firmware_generation ||
                attested.aml_namespace_generation != identity.aml_namespace_generation ||
                attested.platform_topology_generation != identity.platform_topology_generation ||
                attested.pci_fabric_generation != identity.pci_fabric_generation)
                return kernel_dispatch::method_result::error(ESTALE);
        }
        auto driver_function =
            handle_t<pci::function>::make(driver_process->pid, inventory, function.ledger(), function.resource_root(),
                                          binding, identity, true, profile);
        if (!driver_function)
            return kernel_dispatch::method_result::error(ENOMEM);
        if (profile.bar_mask != 0 && !driver_function->bind_device_resources(*device_resources))
            return kernel_dispatch::method_result::error(EBUSY);
        u64 function_protocol_rights = NA_PROTOCOL_RIGHT_INVOKE;
        if ((profile.rights & pci::driver_profile_read_config) != 0)
            function_protocol_rights |= NA_PCI_FUNCTION_RIGHT_READ_CONFIG;
        if ((profile.rights & pci::driver_profile_dma_domain) != 0)
            function_protocol_rights |= NA_PCI_FUNCTION_RIGHT_CREATE_DMA_DOMAIN;
        if ((profile.rights & pci::driver_profile_route_interrupt) != 0)
            function_protocol_rights |= NA_PCI_FUNCTION_RIGHT_ROUTE_INTERRUPT;
        if (profile.bar_mask != 0)
            function_protocol_rights |= NA_PCI_FUNCTION_RIGHT_OPEN_BAR;
        auto function_metadata =
            kernel_view_metadata(NA_SCOPE_PCI_FUNCTION, naos::system::PciFunction::protocol_uuid,
                                 NA_RIGHT_TRANSFER | NA_RIGHT_WAIT | NA_RIGHT_INSPECT, function_protocol_rights,
                                 naos::system::PciFunction::revision, naos::system::PciFunction::features);
        auto private_directory = service::directory::make_private_registry(8);
        if (!private_directory)
            return kernel_dispatch::method_result::error(ENOMEM);
        capability::transfer_record function_record(
            NA_HANDLE_INVALID, true,
            capability::transferred_resource(handle_t<kobject>(driver_function.get_control()), function_metadata));
        constexpr auto function_uri_size = sizeof(NAOS_SERVICE_PCI_FUNCTION) - 1;
        const auto register_status =
            private_directory->register_service(NAOS_SERVICE_PCI_FUNCTION, function_uri_size, function_record, true);
        if (register_status != 0)
            return kernel_dispatch::method_result::error(register_status == ENOMEM ? ENOMEM : EIO);
        auto directory_metadata =
            kernel_view_metadata(NA_SCOPE_SERVICE_DIRECTORY, naos::system::ServiceDirectory::protocol_uuid,
                                 NA_RIGHT_DUPLICATE | NA_RIGHT_TRANSFER | NA_RIGHT_WAIT | NA_RIGHT_INSPECT,
                                 NA_PROTOCOL_RIGHT_INVOKE | NA_SERVICE_DIRECTORY_RIGHT_LOCAL_REGISTER,
                                 naos::system::ServiceDirectory::revision, naos::system::ServiceDirectory::features);
        response.service_directory.value = 0;
        response_resources.push_back(capability::transfer_record(
            NA_HANDLE_INVALID, true,
            capability::transferred_resource(handle_t<kobject>(private_directory.get_control()), directory_metadata)));
        return kernel_dispatch::method_result::success();
    }

    kernel_dispatch::method_result get_identity(const naos::system::PciFunction::get_identity_request &,
                                                naos::system::PciFunction::get_identity_response &response,
                                                const capability::transfer_record_list &,
                                                capability::transfer_record_list &)
    {
        const auto &identity = function.identity();
        response.value.platform_root_id = identity.platform_root_id;
        response.value.segment = identity.segment;
        response.value.bus = identity.bus;
        response.value.device = identity.device;
        response.value.function = identity.function;
        firmware::write_version(response.firmware_version, version_storage, identity.boot_epoch,
                                identity.firmware_generation);
        response.value.aml_namespace_generation = identity.aml_namespace_generation;
        response.value.platform_topology_generation = identity.platform_topology_generation;
        response.value.pci_fabric_generation = identity.pci_fabric_generation;
        return kernel_dispatch::method_result::success();
    }
};

na_status_t publish_pci_function_call(invocation_state &state, pci::function &function, u64 method_id,
                                      const freelibcxx::vector<byte> &request,
                                      capability::transfer_record_list &resources, task::process_t *caller)
{
    if (caller == nullptr || caller->pid != function.owner())
        return NA_STATUS_ACCESS_DENIED;

    const auto &profile = function.driver_profile();
    const bool driver_profile = function.is_driver_grant();
    if (driver_profile &&
        ((method_id == NA_METHOD_PCI_FUNCTION_BIND_DEVICE_RESOURCES) ||
         (method_id == NA_METHOD_PCI_FUNCTION_GRANT_DRIVER) ||
         (method_id == NA_METHOD_PCI_FUNCTION_READ_CONFIG && (profile.rights & pci::driver_profile_read_config) == 0) ||
         (method_id == NA_METHOD_PCI_FUNCTION_CREATE_DMA_DOMAIN &&
          (profile.rights & pci::driver_profile_dma_domain) == 0) ||
         (method_id == NA_METHOD_PCI_FUNCTION_ROUTE_INTERRUPT &&
          (profile.rights & pci::driver_profile_route_interrupt) == 0) ||
         (method_id == NA_METHOD_PCI_FUNCTION_OPEN_BAR && profile.bar_mask == 0)))
        return state.complete_reply(empty_bytes(), empty_resources(), EACCES) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;

    const auto &identity = function.identity();
    if (method_id != NA_METHOD_PCI_FUNCTION_READ_CONFIG && method_id != NA_METHOD_PCI_FUNCTION_GET_IDENTITY &&
        !pci::topology_lifetime().is_current(identity.boot_epoch, identity.firmware_generation,
                                             identity.aml_namespace_generation, identity.pci_fabric_generation,
                                             identity.platform_topology_generation))
        return state.complete_reply(empty_bytes(), empty_resources(), ESTALE) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;

    pci_function_kernel_handler handler{function, caller};
    return naos::system::PciFunction::PciFunctionKernelDispatcher::dispatch(handler, state, method_id, request,
                                                                            resources);
}

namespace
{
i64 interrupt_source_protocol_error(na_status_t status)
{
    return status == NA_STATUS_ACCESS_DENIED                                             ? EACCES
           : status == NA_STATUS_INVALID_ARGUMENT || status == NA_STATUS_INVALID_MESSAGE ? EINVAL
           : status == NA_STATUS_ALREADY_CONSUMED                                        ? EALREADY
           : status == NA_STATUS_WOULD_BLOCK                                             ? EAGAIN
           : status == NA_STATUS_OBJECT_REVOKED                                          ? EIDRM
           : status == NA_STATUS_NOT_SUPPORTED                                           ? ENOTSUP
           : status == NA_STATUS_RESOURCE_EXHAUSTED                                      ? ENOMEM
                                                                                         : EIO;
}

struct interrupt_source_kernel_handler
{
    invocation_state &state;
    pci::interrupt_source &source;
    process_id owner;
    firmware::version_storage version_storage{};

    na_status_t invalid_request()
    {
        return state.complete_reply(empty_bytes(), empty_resources(), EINVAL) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
    }

    kernel_dispatch::method_result bind(const naos::system::InterruptSource::bind_request &,
                                        naos::system::InterruptSource::bind_response &,
                                        const capability::transfer_record_list &, capability::transfer_record_list &)
    {
        const auto result = source.bind(owner);
        if (result.outcome_unknown)
            return kernel_dispatch::method_result::failure(NA_EXECUTION_OUTCOME_UNKNOWN,
                                                           NA_OUTCOME_REASON_HARDWARE_OUTCOME_UNKNOWN);
        if (result.status != NA_STATUS_OK)
            return kernel_dispatch::method_result::error(interrupt_source_protocol_error(result.status));
        return kernel_dispatch::method_result::success();
    }

    kernel_dispatch::method_result wait(const naos::system::InterruptSource::wait_request &,
                                        naos::system::InterruptSource::wait_response &response,
                                        const capability::transfer_record_list &, capability::transfer_record_list &)
    {
        const auto wait_status = source.wait(owner, response.event.sequence, response.event.flags);
        if (wait_status != NA_STATUS_OK)
            return kernel_dispatch::method_result::error(interrupt_source_protocol_error(wait_status));

        const auto &identity = source.identity();
        firmware::write_version(response.firmware_version, version_storage, identity.boot_epoch,
                                identity.firmware_generation);
        response.event.aml_namespace_generation = identity.aml_namespace_generation;
        response.event.platform_topology_generation = identity.platform_topology_generation;
        response.event.pci_fabric_generation = identity.pci_fabric_generation;
        return kernel_dispatch::method_result::success();
    }

    kernel_dispatch::method_result set_mask(const naos::system::InterruptSource::set_mask_request &request,
                                            naos::system::InterruptSource::set_mask_response &,
                                            const capability::transfer_record_list &,
                                            capability::transfer_record_list &)
    {
        if (request.masked > 1)
            return kernel_dispatch::method_result::error(EINVAL);
        const auto result = source.set_masked(owner, request.masked != 0);
        if (result.outcome_unknown)
            return kernel_dispatch::method_result::failure(NA_EXECUTION_OUTCOME_UNKNOWN,
                                                           NA_OUTCOME_REASON_HARDWARE_OUTCOME_UNKNOWN);
        if (result.status != NA_STATUS_OK)
            return kernel_dispatch::method_result::error(interrupt_source_protocol_error(result.status));
        return kernel_dispatch::method_result::success();
    }

    kernel_dispatch::method_result ack(const naos::system::InterruptSource::ack_request &request,
                                       naos::system::InterruptSource::ack_response &response,
                                       const capability::transfer_record_list &, capability::transfer_record_list &)
    {
        bool pending = false;
        const auto status =
            source.acknowledge(owner, request.sequence, response.event.sequence, pending, response.event.flags);
        if (status != NA_STATUS_OK)
            return kernel_dispatch::method_result::error(interrupt_source_protocol_error(status));
        if (pending)
            response.event.flags |= 1u;

        const auto &identity = source.identity();
        firmware::write_version(response.firmware_version, version_storage, identity.boot_epoch,
                                identity.firmware_generation);
        response.event.aml_namespace_generation = identity.aml_namespace_generation;
        response.event.platform_topology_generation = identity.platform_topology_generation;
        response.event.pci_fabric_generation = identity.pci_fabric_generation;
        return kernel_dispatch::method_result::success();
    }
};
} // namespace

na_status_t publish_interrupt_source_call(invocation_state &state, pci::interrupt_source &source, u64 method_id,
                                          const freelibcxx::vector<byte> &request,
                                          capability::transfer_record_list &resources, task::process_t *caller)
{
    if (caller == nullptr)
        return NA_STATUS_ACCESS_DENIED;

    interrupt_source_kernel_handler handler{state, source, caller->pid};
    return naos::system::InterruptSource::InterruptSourceKernelDispatcher::dispatch(handler, state, method_id, request,
                                                                                    resources);
}

struct pci_dma_domain_kernel_handler
{
    pci::dma_domain &domain;
    process_id caller;

    na_status_t invalid_request() { return NA_STATUS_INVALID_MESSAGE; }

    static kernel_dispatch::method_result convert_result(pci::dma_operation_result result)
    {
        if (result.outcome_unknown)
            return kernel_dispatch::method_result::failure(NA_EXECUTION_OUTCOME_UNKNOWN,
                                                           NA_OUTCOME_REASON_HARDWARE_OUTCOME_UNKNOWN);
        return result.status == NA_STATUS_OK
                   ? kernel_dispatch::method_result::success()
                   : kernel_dispatch::method_result::error(pci::dma::status_errno(result.status));
    }

    kernel_dispatch::method_result map_page(const naos::system::PciDmaDomain::map_page_request &request,
                                            naos::system::PciDmaDomain::map_page_response &,
                                            capability::transfer_record_list &resources,
                                            capability::transfer_record_list &)
    {
        naos::data_plane::dma_direction direction{};
        if (request.direction == 0)
            direction = naos::data_plane::dma_direction::device_reads;
        else if (request.direction == 1)
            direction = naos::data_plane::dma_direction::device_writes;
        else if (request.direction == 2)
            direction = naos::data_plane::dma_direction::device_reads_and_writes;
        else
            return kernel_dispatch::method_result::error(EINVAL);

        u64 object_offset = 0;
        u64 direction_right = NA_MEMORY_RIGHT_WRITE;
        if (direction == naos::data_plane::dma_direction::device_reads)
            direction_right = NA_MEMORY_RIGHT_READ;
        else if (direction == naos::data_plane::dma_direction::device_reads_and_writes)
            direction_right |= NA_MEMORY_RIGHT_READ;
        auto *backing_object =
            request_region_object(resources, request.backing.value, NA_MEMORY_RIGHT_MAP | direction_right,
                                  request.object_offset, memory::page_size, object_offset);
        if (backing_object == nullptr)
            return kernel_dispatch::method_result::error(EACCES);
        auto backing = resources.at(request.backing.value).resource.object().as<data_plane::memory_object>();
        return convert_result(domain.map_page(caller, std::move(backing), request.iova, object_offset, direction));
    }

    kernel_dispatch::method_result unmap_page(const naos::system::PciDmaDomain::unmap_page_request &request,
                                              naos::system::PciDmaDomain::unmap_page_response &,
                                              const capability::transfer_record_list &,
                                              capability::transfer_record_list &)
    {
        return convert_result(domain.unmap_page(caller, request.iova));
    }

    kernel_dispatch::method_result set_bus_master(const naos::system::PciDmaDomain::set_bus_master_request &request,
                                                  naos::system::PciDmaDomain::set_bus_master_response &,
                                                  const capability::transfer_record_list &,
                                                  capability::transfer_record_list &)
    {
        if (request.enabled > 1)
            return kernel_dispatch::method_result::error(EINVAL);
        return convert_result(domain.set_bus_master(caller, request.enabled != 0));
    }

    kernel_dispatch::method_result close(const naos::system::PciDmaDomain::close_request &,
                                         naos::system::PciDmaDomain::close_response &,
                                         const capability::transfer_record_list &, capability::transfer_record_list &)
    {
        return convert_result(domain.close(caller));
    }
};

na_status_t publish_pci_dma_domain_call(invocation_state &state, pci::dma_domain &domain, u64 method_id,
                                        const freelibcxx::vector<byte> &request,
                                        capability::transfer_record_list &resources, process_id caller)
{
    if (caller == 0 || caller != domain.owner())
        return state.complete_reply(empty_bytes(), empty_resources(), EACCES) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
    pci_dma_domain_kernel_handler handler{domain, caller};
    return naos::system::PciDmaDomain::PciDmaDomainKernelDispatcher::dispatch(handler, state, method_id, request,
                                                                              resources);
}

struct pci_dma_fault_source_kernel_handler
{
    pci::dma_fault_source &source;
    process_id caller;
    firmware::version_storage version_storage{};

    na_status_t invalid_request() { return NA_STATUS_INVALID_MESSAGE; }

    kernel_dispatch::method_result take(const naos::system::PciDmaFaultSource::take_request &request,
                                        naos::system::PciDmaFaultSource::take_response &response,
                                        const capability::transfer_record_list &, capability::transfer_record_list &)
    {
        pci::dma_fault_batch batch{};
        const auto status = source.take_faults(caller, request.limit, batch);
        if (status != NA_STATUS_OK)
        {
            const i64 error = status == NA_STATUS_ACCESS_DENIED      ? EACCES
                              : status == NA_STATUS_INVALID_ARGUMENT ? EINVAL
                              : status == NA_STATUS_OBJECT_REVOKED   ? EIDRM
                                                                     : EIO;
            return kernel_dispatch::method_result::error(error);
        }
        response.sequence = batch.sequence;
        response.count = batch.count;
        response.dropped_records = batch.dropped_records;
        response.unit_overflow = batch.unit_overflow ? 1u : 0u;
        firmware::write_version(response.firmware_version, version_storage, batch.boot_epoch,
                                batch.firmware_generation);
        response.aml_namespace_generation = batch.aml_namespace_generation;
        response.platform_topology_generation = batch.platform_topology_generation;
        response.pci_fabric_generation = batch.pci_fabric_generation;
        response.records.data = response.records_storage.data();
        response.records.count = batch.count;
        for (u32 index = 0; index < batch.count; ++index)
        {
            response.records.data[index].iova = batch.records[index].iova;
            response.records.data[index].requester_id = batch.records[index].requester_id;
            response.records.data[index].reason = batch.records[index].reason;
            response.records.data[index].direction = static_cast<u8>(batch.records[index].direction);
        }
        return kernel_dispatch::method_result::success();
    }
};

na_status_t publish_pci_dma_fault_source_call(invocation_state &state, pci::dma_fault_source &source, u64 method_id,
                                              const freelibcxx::vector<byte> &request,
                                              capability::transfer_record_list &resources, process_id caller)
{
    if (caller == 0 || caller != source.owner())
        return state.complete_reply(empty_bytes(), empty_resources(), EACCES) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
    pci_dma_fault_source_kernel_handler handler{source, caller};
    return naos::system::PciDmaFaultSource::PciDmaFaultSourceKernelDispatcher::dispatch(handler, state, method_id,
                                                                                        request, resources);
}

struct pci_root_resource_attestation_kernel_handler
{
    pci::root_resource_attestation_capability &attestation;
    task::process_t *caller;
    firmware::version_storage version_storage{};

    na_status_t invalid_request() { return NA_STATUS_INVALID_MESSAGE; }

    kernel_dispatch::method_result
    get_authority(const naos::system::PciRootResourceAttestation::get_authority_request &,
                  naos::system::PciRootResourceAttestation::get_authority_response &response,
                  const capability::transfer_record_list &, capability::transfer_record_list &)
    {
        const auto &authority = attestation.authority();
        response.root_id = authority.root_id;
        firmware::write_version(response.firmware_version, version_storage, authority.boot_epoch,
                                authority.firmware_generation);
        response.aml_namespace_generation = authority.aml_namespace_generation;
        response.platform_topology_generation = authority.platform_topology_generation;
        response.pci_fabric_generation = authority.pci_fabric_generation;
        response.compatibility_profile_hash = authority.compatibility_profile_hash;
        response.segment = authority.segment;
        response.start_bus = authority.start_bus;
        response.end_bus = authority.end_bus;
        response.memory_windows.data = response.memory_windows_storage.data();
        response.memory_windows.count = authority.memory_window_count;
        for (u32 index = 0; index < authority.memory_window_count; ++index)
        {
            response.memory_windows.data[index].base = authority.memory_windows[index].base;
            response.memory_windows.data[index].length = authority.memory_windows[index].length;
        }
        return kernel_dispatch::method_result::success();
    }

    kernel_dispatch::method_result
    mint_device_resources(const naos::system::PciRootResourceAttestation::mint_device_resources_request &request,
                          naos::system::PciRootResourceAttestation::mint_device_resources_response &response,
                          const capability::transfer_record_list &,
                          capability::transfer_record_list &response_resources)
    {
        if (caller == nullptr || caller->pid != attestation.issuer())
            return kernel_dispatch::method_result::error(EACCES);
        const auto &authority = attestation.authority();
        pci::device_resource_evidence evidence{};
        static_cast<pci::root_feature_scope &>(evidence) = static_cast<const pci::root_feature_scope &>(authority);
        evidence.device_id = request.device_id;
        evidence.path_length = request.path_length;
        for (u32 index = 0; index < pci::max_pci_device_function_path; ++index)
            evidence.device_function_path[index] = request.device_function_path[index];
        evidence.memory_window_count = request.memory_windows.count;
        for (u32 index = 0; index < request.memory_windows.count; ++index)
        {
            evidence.memory_windows[index] = {request.memory_windows.data[index].base,
                                              request.memory_windows.data[index].length,
                                              request.memory_windows.data[index].flags};
        }

        pci::device_resource_attestation device_authority{};
        switch (pci::validate_device_resource_evidence(evidence, authority, device_authority))
        {
            case pci::device_resource_error::none:
                break;
            case pci::device_resource_error::stale_scope:
                return kernel_dispatch::method_result::error(ESTALE);
            case pci::device_resource_error::outside_root:
                return kernel_dispatch::method_result::error(EACCES);
            default:
                return kernel_dispatch::method_result::error(EINVAL);
        }
        auto device_attestation =
            handle_t<pci::device_resource_attestation_capability>::make(caller->pid, device_authority);
        if (!device_attestation)
            return kernel_dispatch::method_result::error(ENOMEM);
        auto metadata = kernel_view_metadata(
            NA_SCOPE_PCI_DEVICE_RESOURCE_ATTESTATION, naos::system::PciDeviceResourceAttestation::protocol_uuid,
            NA_RIGHT_WAIT | NA_RIGHT_INSPECT | NA_RIGHT_TRANSFER, NA_PROTOCOL_RIGHT_INVOKE,
            naos::system::PciDeviceResourceAttestation::revision, naos::system::PciDeviceResourceAttestation::features);
        response.authority.value = 0;
        response_resources.push_back(capability::transfer_record(
            NA_HANDLE_INVALID, true,
            capability::transferred_resource(handle_t<kobject>(device_attestation.get_control()), metadata)));
        return kernel_dispatch::method_result::success();
    }
};

na_status_t publish_pci_root_resource_attestation_call(invocation_state &state,
                                                       pci::root_resource_attestation_capability &attestation,
                                                       u64 method_id, const freelibcxx::vector<byte> &request,
                                                       task::process_t *caller)
{
    capability::transfer_record_list resources(memory::KernelCommonAllocatorV);
    pci_root_resource_attestation_kernel_handler handler{attestation, caller};
    return naos::system::PciRootResourceAttestation::PciRootResourceAttestationKernelDispatcher::dispatch(
        handler, state, method_id, request, resources);
}

struct pci_device_resource_attestation_kernel_handler
{
    pci::device_resource_attestation_capability &attestation;
    firmware::version_storage version_storage{};

    na_status_t invalid_request() { return NA_STATUS_INVALID_MESSAGE; }

    kernel_dispatch::method_result
    get_authority(const naos::system::PciDeviceResourceAttestation::get_authority_request &,
                  naos::system::PciDeviceResourceAttestation::get_authority_response &response,
                  const capability::transfer_record_list &, capability::transfer_record_list &)
    {
        const auto &authority = attestation.authority();
        response.root_id = authority.root_id;
        response.device_id = authority.device_id;
        firmware::write_version(response.firmware_version, version_storage, authority.boot_epoch,
                                authority.firmware_generation);
        response.aml_namespace_generation = authority.aml_namespace_generation;
        response.platform_topology_generation = authority.platform_topology_generation;
        response.pci_fabric_generation = authority.pci_fabric_generation;
        response.compatibility_profile_hash = authority.compatibility_profile_hash;
        response.segment = authority.segment;
        response.start_bus = authority.start_bus;
        response.end_bus = authority.end_bus;
        response.path_length = authority.path_length;
        response.device_function_path = authority.device_function_path;
        response.memory_windows.data = response.memory_windows_storage.data();
        response.memory_windows.count = authority.memory_window_count;
        for (u32 index = 0; index < authority.memory_window_count; ++index)
        {
            response.memory_windows.data[index].base = authority.memory_windows[index].base;
            response.memory_windows.data[index].length = authority.memory_windows[index].length;
            response.memory_windows.data[index].flags = authority.memory_windows[index].flags;
        }
        return kernel_dispatch::method_result::success();
    }
};

na_status_t publish_pci_device_resource_attestation_call(invocation_state &state,
                                                         pci::device_resource_attestation_capability &attestation,
                                                         u64 method_id, const freelibcxx::vector<byte> &request)
{
    capability::transfer_record_list resources(memory::KernelCommonAllocatorV);
    pci_device_resource_attestation_kernel_handler handler{attestation};
    return naos::system::PciDeviceResourceAttestation::PciDeviceResourceAttestationKernelDispatcher::dispatch(
        handler, state, method_id, request, resources);
}

} // namespace

namespace
{
na_status_t dispatch_pci_discovery_fabric(kernel_dispatch::kernel_view_context &context, pci::discovery_fabric &object)
{
    return publish_pci_discovery_call(context.state, object, context.method_id, context.request, context.resources,
                                      context.caller);
}

na_status_t dispatch_pci_fabric_admin(kernel_dispatch::kernel_view_context &context, pci::fabric_admin &object)
{
    return publish_pci_fabric_admin_call(context.state, object, context.method_id, context.request, context.caller);
}

na_status_t dispatch_pci_function(kernel_dispatch::kernel_view_context &context, pci::function &object)
{
    return publish_pci_function_call(context.state, object, context.method_id, context.request, context.resources,
                                     context.caller);
}

na_status_t dispatch_pci_interrupt_source(kernel_dispatch::kernel_view_context &context, pci::interrupt_source &object)
{
    return publish_interrupt_source_call(context.state, object, context.method_id, context.request, context.resources,
                                         context.caller);
}

na_status_t dispatch_pci_dma_domain(kernel_dispatch::kernel_view_context &context, pci::dma_domain &object)
{
    const process_id caller = context.caller == nullptr ? 0 : context.caller->pid;
    return publish_pci_dma_domain_call(context.state, object, context.method_id, context.request, context.resources,
                                       caller);
}

na_status_t dispatch_pci_dma_fault_source(kernel_dispatch::kernel_view_context &context, pci::dma_fault_source &object)
{
    const process_id caller = context.caller == nullptr ? 0 : context.caller->pid;
    return publish_pci_dma_fault_source_call(context.state, object, context.method_id, context.request,
                                             context.resources, caller);
}

na_status_t dispatch_pci_root_resource_attestation(kernel_dispatch::kernel_view_context &context,
                                                   pci::root_resource_attestation_capability &object)
{
    return publish_pci_root_resource_attestation_call(context.state, object, context.method_id, context.request,
                                                      context.caller);
}

na_status_t dispatch_pci_device_resource_attestation(kernel_dispatch::kernel_view_context &context,
                                                     pci::device_resource_attestation_capability &object)
{
    return publish_pci_device_resource_attestation_call(context.state, object, context.method_id, context.request);
}

constexpr kernel_dispatch::kernel_view_route pci_kernel_view_route_entries[] = {
    kernel_dispatch::make_kernel_view_route<pci::discovery_fabric, dispatch_pci_discovery_fabric>(
        NA_SCOPE_PCI_DISCOVERY_FABRIC, naos::system::PciDiscoveryFabric::descriptor),
    kernel_dispatch::make_kernel_view_route<pci::fabric_admin, dispatch_pci_fabric_admin>(
        NA_SCOPE_PCI_FABRIC_ADMIN, naos::system::PciFabricAdmin::descriptor),
    kernel_dispatch::make_kernel_view_route<pci::function, dispatch_pci_function>(
        NA_SCOPE_PCI_FUNCTION, naos::system::PciFunction::descriptor),
    kernel_dispatch::make_kernel_view_route<pci::interrupt_source, dispatch_pci_interrupt_source>(
        NA_SCOPE_INTERRUPT_SOURCE, naos::system::InterruptSource::descriptor),
    kernel_dispatch::make_kernel_view_route<pci::dma_domain, dispatch_pci_dma_domain>(
        NA_SCOPE_PCI_DMA_DOMAIN, naos::system::PciDmaDomain::descriptor),
    kernel_dispatch::make_kernel_view_route<pci::dma_fault_source, dispatch_pci_dma_fault_source>(
        NA_SCOPE_PCI_DMA_FAULT_SOURCE, naos::system::PciDmaFaultSource::descriptor),
    kernel_dispatch::make_kernel_view_route<pci::root_resource_attestation_capability,
                                            dispatch_pci_root_resource_attestation>(
        NA_SCOPE_PCI_ROOT_RESOURCE_ATTESTATION, naos::system::PciRootResourceAttestation::descriptor),
    kernel_dispatch::make_kernel_view_route<pci::device_resource_attestation_capability,
                                            dispatch_pci_device_resource_attestation>(
        NA_SCOPE_PCI_DEVICE_RESOURCE_ATTESTATION, naos::system::PciDeviceResourceAttestation::descriptor),
};
} // namespace

kernel_dispatch::kernel_view_route_table pci_kernel_view_routes() noexcept
{
    return {pci_kernel_view_route_entries,
            sizeof(pci_kernel_view_route_entries) / sizeof(pci_kernel_view_route_entries[0])};
}
} // namespace naos::ipc
