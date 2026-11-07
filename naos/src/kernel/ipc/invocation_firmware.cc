#include "invocation_handlers.hpp"
#include "invocation_support.hpp"
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
#include "kernel/pci/topology_lifecycle.hpp"
#include "kernel/task.hpp"
#include "kernel/timer.hpp"
#include "kernel/ucontext.hpp"
#include "kernel/usercopy.hpp"
#include "naos/generated/system/AcpiFixedHardware.hpp"
#include "naos/generated/system/AcpiFixedHardware_kernel_dispatch.hpp"
#include "naos/generated/system/AcpiResourceAuthority.hpp"
#include "naos/generated/system/AcpiResourceAuthority_kernel_dispatch.hpp"
#include "naos/generated/system/FirmwareRoot.hpp"
#include "naos/generated/system/FirmwareRoot_kernel_dispatch.hpp"
#include "naos/generated/system/MemoryObject.hpp"
#include "naos/generated/system/PciRootResourceAttestation.hpp"
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

struct firmware_root_kernel_handler
{
    const arch::acpi_boot::snapshot *snapshot;
    firmware::version_storage version_storage{};

    na_status_t invalid_request() { return NA_STATUS_INVALID_MESSAGE; }

    kernel_dispatch::method_result describe(const naos::system::FirmwareRoot::describe_request &,
                                            naos::system::FirmwareRoot::describe_response &response,
                                            const capability::transfer_record_list &,
                                            capability::transfer_record_list &)
    {
        response.kind = 1; // ACPI
        response.revision = 0;
        for (const auto &table : snapshot->tables)
        {
            if (table.object_id == snapshot->rsdp_object_id)
            {
                response.revision = table.revision;
                break;
            }
        }
        firmware::write_version(response.firmware_version, version_storage, snapshot->boot_epoch,
                                snapshot->firmware_generation);
        response.pci_fabric_generation = snapshot->pci_fabric_generation;
        return kernel_dispatch::method_result::success();
    }

    kernel_dispatch::method_result list_objects(const naos::system::FirmwareRoot::list_objects_request &decoded,
                                                naos::system::FirmwareRoot::list_objects_response &response,
                                                const capability::transfer_record_list &,
                                                capability::transfer_record_list &)
    {
        if (decoded.cursor > snapshot->tables.size() || decoded.limit > 64)
            return kernel_dispatch::method_result::error(EINVAL);
        const u64 limit = decoded.limit == 0 ? 64 : decoded.limit;
        const u64 end =
            decoded.cursor +
            (limit < snapshot->tables.size() - decoded.cursor ? limit : snapshot->tables.size() - decoded.cursor);
        response.objects.data = response.objects_storage.data();
        response.objects.count = static_cast<u32>(end - decoded.cursor);
        firmware::write_version(response.firmware_version, version_storage, snapshot->boot_epoch,
                                snapshot->firmware_generation);
        const auto tables = snapshot->tables.cspan();
        for (u64 index = decoded.cursor; index < end; index++)
        {
            const auto &table = tables[index];
            auto &value = response.objects.data[index - decoded.cursor];
            value.object_id = table.object_id;
            value.signature = table.signature;
            value.revision = table.revision;
            value.length = table.length;
            value.root_ordinal = table.root_ordinal;
            value.aml_load_ordinal = table.aml_load_ordinal;
            value.flags = arch::acpi_boot::firmware_object_flags(table.signature, table.has_checksum);
        }
        response.next_cursor = end == snapshot->tables.size() ? 0 : end;
        return kernel_dispatch::method_result::success();
    }

    kernel_dispatch::method_result open_object(const naos::system::FirmwareRoot::open_object_request &decoded,
                                               naos::system::FirmwareRoot::open_object_response &response,
                                               const capability::transfer_record_list &,
                                               capability::transfer_record_list &resources)
    {
        u64 boot_epoch = 0;
        u64 firmware_generation = 0;
        if (!firmware::decode_version(decoded.firmware_version, boot_epoch, firmware_generation) ||
            boot_epoch != snapshot->boot_epoch || firmware_generation != snapshot->firmware_generation)
            return kernel_dispatch::method_result::error(ESTALE);
        const arch::acpi_boot::table_record *selected = nullptr;
        for (const auto &table : snapshot->tables)
        {
            if (table.object_id == decoded.object_id)
            {
                selected = &table;
                break;
            }
        }
        if (selected == nullptr)
            return kernel_dispatch::method_result::error(ENOENT);
        if (!arch::acpi_boot::firmware_object_is_openable(selected->signature))
            return kernel_dispatch::method_result::error(ENOTSUP);
        if (selected->snapshot == nullptr || selected->length == 0)
            return kernel_dispatch::method_result::error(ENOENT);
        auto object = handle_t<naos::data_plane::memory_object>::make(selected->snapshot, selected->length);
        if (!object)
            return kernel_dispatch::method_result::error(ENOMEM);
        auto metadata = kernel_view_metadata(
            NA_SCOPE_MEMORY_OBJECT, naos::system::MemoryObject::protocol_uuid, NA_RIGHT_WAIT | NA_RIGHT_INSPECT,
            NA_MEMORY_RIGHT_READ | NA_MEMORY_RIGHT_MAP | NA_MEMORY_RIGHT_INFO, naos::system::MemoryObject::revision,
            naos::system::MemoryObject::features);
        metadata.binding = NA_BINDING_MEMORY_OBJECT;
        metadata.view_offset = 0;
        metadata.view_length = selected->length;
        response.object.value = 0;
        resources.push_back(capability::transfer_record(NA_HANDLE_INVALID, true,
                                                        capability::transferred_resource(std::move(object), metadata)));
        return kernel_dispatch::method_result::success();
    }

    kernel_dispatch::method_result list_relations(const naos::system::FirmwareRoot::list_relations_request &decoded,
                                                  naos::system::FirmwareRoot::list_relations_response &response,
                                                  const capability::transfer_record_list &,
                                                  capability::transfer_record_list &)
    {
        if (decoded.cursor > snapshot->relations.size() || decoded.limit > 64)
            return kernel_dispatch::method_result::error(EINVAL);
        const u64 limit = decoded.limit == 0 ? 64 : decoded.limit;
        const u64 end =
            decoded.cursor +
            (limit < snapshot->relations.size() - decoded.cursor ? limit : snapshot->relations.size() - decoded.cursor);
        response.relations.data = response.relations_storage.data();
        response.relations.count = static_cast<u32>(end - decoded.cursor);
        firmware::write_version(response.firmware_version, version_storage, snapshot->boot_epoch,
                                snapshot->firmware_generation);
        const auto relations = snapshot->relations.cspan();
        for (u64 index = decoded.cursor; index < end; index++)
        {
            const auto &relation = relations[index];
            auto &value = response.relations.data[index - decoded.cursor];
            value.from_object = relation.from;
            value.to_object = relation.to;
            value.kind = static_cast<u32>(relation.kind);
            value.ordinal = relation.ordinal;
        }
        response.next_cursor = end == snapshot->relations.size() ? 0 : end;
        return kernel_dispatch::method_result::success();
    }
};

na_status_t publish_firmware_root_call(invocation_state &state, firmware::root &root, u64 method_id,
                                       const freelibcxx::vector<byte> &request)
{
    const auto *snapshot = root.snapshot();
    if (snapshot == nullptr)
        return state.complete_reply(empty_bytes(), empty_resources(), EIO) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;

    firmware_root_kernel_handler handler{snapshot};
    capability::transfer_record_list resources(memory::KernelCommonAllocatorV);
    return naos::system::FirmwareRoot::FirmwareRootKernelDispatcher::dispatch(handler, state, method_id, request,
                                                                              resources);
}

struct acpi_fixed_hardware_kernel_handler
{
    acpi::fixed_hardware &fixed;
    task::process_t *caller;
    const acpi::fixed_inventory &inventory;
    firmware::version_storage version_storage{};
    u64 rollback_ticket = 0;
    u64 rollback_boot_epoch = 0;
    u64 rollback_firmware_generation = 0;

    na_status_t invalid_request() { return NA_STATUS_INVALID_MESSAGE; }

    template <typename String> bool decode_version(const String &token, u64 &boot_epoch, u64 &firmware_generation) const
    {
        return firmware::decode_version(token, boot_epoch, firmware_generation) && boot_epoch == inventory.boot_epoch &&
               firmware_generation == inventory.firmware_generation;
    }

    static void finish_transition(void *context, bool committed)
    {
        auto &handler = *static_cast<acpi_fixed_hardware_kernel_handler *>(context);
        if (!committed && handler.rollback_ticket != 0)
            (void)handler.fixed.abort_transition(handler.caller, handler.rollback_boot_epoch,
                                                 handler.rollback_firmware_generation, handler.rollback_ticket);
    }

    kernel_dispatch::method_result describe(const naos::system::AcpiFixedHardware::describe_request &,
                                            naos::system::AcpiFixedHardware::describe_response &response,
                                            const capability::transfer_record_list &,
                                            capability::transfer_record_list &)
    {
        firmware::write_version(response.firmware_version, version_storage, inventory.boot_epoch,
                                inventory.firmware_generation);
        response.value.sci_gsi = inventory.sci_gsi;
        response.value.pm1_event_bytes = inventory.pm1_event.length;
        response.value.pm1_control_bytes = inventory.pm1_control.length;
        response.value.pm2_control_bytes = inventory.pm2_control.length;
        response.value.gpe0_bytes = inventory.gpe0_status.length + inventory.gpe0_enable.length;
        response.value.gpe1_bytes = inventory.gpe1_status.length + inventory.gpe1_enable.length;
        response.value.gpe1_base = inventory.gpe1_base;
        response.value.reset_supported = inventory.reset.address != 0 ? 1 : 0;
        response.value.sleep_supported =
            inventory.pm1_control.address != 0 && inventory.pm1_control.length >= 2 ? 1 : 0;
        response.value.sci_sequence = fixed.capability_state();
        response.value.sci_state = fixed.sci_state_value();
        pci::platform_generation_lineage lineage{};
        if (fixed.get_platform_lineage(lineage))
        {
            response.value.aml_namespace_generation = lineage.aml_namespace_generation;
            response.value.platform_topology_generation = lineage.platform_topology_generation;
            response.value.pci_fabric_generation = lineage.pci_fabric_generation;
        }
        return kernel_dispatch::method_result::success();
    }

    kernel_dispatch::method_result
    claim_transition_owner(const naos::system::AcpiFixedHardware::claim_transition_owner_request &request,
                           naos::system::AcpiFixedHardware::claim_transition_owner_response &response,
                           const capability::transfer_record_list &, capability::transfer_record_list &)
    {
        u64 boot_epoch = 0;
        u64 firmware_generation = 0;
        if (!decode_version(request.firmware_version, boot_epoch, firmware_generation))
            return kernel_dispatch::method_result::error(ESTALE);
        const auto result = fixed.claim_transition_owner(caller, boot_epoch, firmware_generation);
        if (result != acpi::sci_operation_result::success)
            return kernel_dispatch::method_result::error(acpi::sci_operation_errno(result));
        firmware::write_version(response.firmware_version, version_storage, inventory.boot_epoch,
                                inventory.firmware_generation);
        return kernel_dispatch::method_result::success();
    }

    kernel_dispatch::method_result claim_sci(const naos::system::AcpiFixedHardware::claim_sci_request &request,
                                             naos::system::AcpiFixedHardware::claim_sci_response &response,
                                             const capability::transfer_record_list &,
                                             capability::transfer_record_list &)
    {
        u64 boot_epoch = 0;
        u64 firmware_generation = 0;
        if (!decode_version(request.firmware_version, boot_epoch, firmware_generation))
            return kernel_dispatch::method_result::error(ESTALE);
        const auto result = fixed.claim_sci(caller, boot_epoch, firmware_generation);
        if (result != acpi::sci_operation_result::success)
            return kernel_dispatch::method_result::error(acpi::sci_operation_errno(result));
        firmware::write_version(response.firmware_version, version_storage, inventory.boot_epoch,
                                inventory.firmware_generation);
        return kernel_dispatch::method_result::success();
    }

    kernel_dispatch::method_result
    report_sci_failure(const naos::system::AcpiFixedHardware::report_sci_failure_request &request,
                       naos::system::AcpiFixedHardware::report_sci_failure_response &response,
                       const capability::transfer_record_list &, capability::transfer_record_list &)
    {
        u64 boot_epoch = 0;
        u64 firmware_generation = 0;
        if (!decode_version(request.firmware_version, boot_epoch, firmware_generation))
            return kernel_dispatch::method_result::error(ESTALE);
        const auto result = fixed.report_sci_failure(caller, boot_epoch, firmware_generation);
        if (result != acpi::sci_operation_result::success)
            return kernel_dispatch::method_result::error(acpi::sci_operation_errno(result));
        response.sci_state = fixed.sci_state_value();
        return kernel_dispatch::method_result::success();
    }

    kernel_dispatch::method_result drain_sci(const naos::system::AcpiFixedHardware::drain_sci_request &request,
                                             naos::system::AcpiFixedHardware::drain_sci_response &response,
                                             const capability::transfer_record_list &,
                                             capability::transfer_record_list &)
    {
        u32 flags = 0;
        u64 pm1_status = 0;
        u64 pm1_status_b = 0;
        u64 gpe0_status = 0;
        u64 gpe1_status = 0;
        const auto result =
            fixed.drain_sci(caller, request.sequence, flags, pm1_status, pm1_status_b, gpe0_status, gpe1_status);
        if (result != acpi::sci_operation_result::success)
            return kernel_dispatch::method_result::error(acpi::sci_operation_errno(result));
        response.event.sequence = request.sequence;
        response.event.flags = flags;
        response.event.pm1_event_status = pm1_status;
        response.event.pm1_event_status_b = pm1_status_b;
        response.event.gpe0_status = gpe0_status;
        response.event.gpe1_status = gpe1_status;
        pci::platform_generation_lineage lineage{};
        if (!fixed.get_platform_lineage(lineage))
            return kernel_dispatch::method_result::error(ESTALE);
        firmware::write_version(response.firmware_version, version_storage, lineage.boot_epoch,
                                lineage.firmware_generation);
        response.event.aml_namespace_generation = lineage.aml_namespace_generation;
        response.event.platform_topology_generation = lineage.platform_topology_generation;
        response.event.pci_fabric_generation = lineage.pci_fabric_generation;
        return kernel_dispatch::method_result::success();
    }

    kernel_dispatch::method_result
    clear_sci_status(const naos::system::AcpiFixedHardware::clear_sci_status_request &request,
                     naos::system::AcpiFixedHardware::clear_sci_status_response &,
                     const capability::transfer_record_list &, capability::transfer_record_list &)
    {
        u64 boot_epoch = 0;
        u64 firmware_generation = 0;
        if (!decode_version(request.firmware_version, boot_epoch, firmware_generation))
            return kernel_dispatch::method_result::error(ESTALE);
        const acpi::sci_status_masks masks{request.pm1_event_mask, request.pm1_event_status_b_mask, request.gpe0_mask,
                                           request.gpe1_mask};
        const auto result = fixed.clear_sci_status(caller, boot_epoch, firmware_generation, request.sequence, masks);
        return result == acpi::sci_operation_result::success
                   ? kernel_dispatch::method_result::success()
                   : kernel_dispatch::method_result::error(acpi::sci_operation_errno(result));
    }

    kernel_dispatch::method_result
    ack_and_rearm_sci(const naos::system::AcpiFixedHardware::ack_and_rearm_sci_request &request,
                      naos::system::AcpiFixedHardware::ack_and_rearm_sci_response &response,
                      const capability::transfer_record_list &, capability::transfer_record_list &)
    {
        u32 state_value = 0;
        u32 flags = 0;
        const auto result = fixed.ack_and_rearm_sci(caller, request.sequence, state_value, flags);
        if (result != acpi::sci_operation_result::success)
            return kernel_dispatch::method_result::error(acpi::sci_operation_errno(result));
        response.ack.sequence = fixed.capability_state();
        response.ack.state = state_value;
        response.ack.flags = flags;
        pci::platform_generation_lineage lineage{};
        if (!fixed.get_platform_lineage(lineage))
            return kernel_dispatch::method_result::error(ESTALE);
        firmware::write_version(response.firmware_version, version_storage, lineage.boot_epoch,
                                lineage.firmware_generation);
        response.ack.aml_namespace_generation = lineage.aml_namespace_generation;
        response.ack.platform_topology_generation = lineage.platform_topology_generation;
        response.ack.pci_fabric_generation = lineage.pci_fabric_generation;
        return kernel_dispatch::method_result::success();
    }

    kernel_dispatch::method_result set_gpe_mask(const naos::system::AcpiFixedHardware::set_gpe_mask_request &request,
                                                naos::system::AcpiFixedHardware::set_gpe_mask_response &,
                                                const capability::transfer_record_list &,
                                                capability::transfer_record_list &)
    {
        if (request.enable > 1 || request.mask == 0 || request.bank > 1)
            return kernel_dispatch::method_result::error(EINVAL);
        u64 boot_epoch = 0;
        u64 firmware_generation = 0;
        if (!decode_version(request.firmware_version, boot_epoch, firmware_generation))
            return kernel_dispatch::method_result::error(ESTALE);
        const auto result = fixed.set_gpe_mask(caller, boot_epoch, firmware_generation, request.bank, request.mask,
                                               request.enable != 0);
        return result == acpi::sci_operation_result::success
                   ? kernel_dispatch::method_result::success()
                   : kernel_dispatch::method_result::error(acpi::sci_operation_errno(result));
    }

    kernel_dispatch::method_result prepare_sleep(const naos::system::AcpiFixedHardware::prepare_sleep_request &request,
                                                 naos::system::AcpiFixedHardware::prepare_sleep_response &response,
                                                 const capability::transfer_record_list &,
                                                 capability::transfer_record_list &)
    {
        u64 boot_epoch = 0;
        u64 firmware_generation = 0;
        if (!decode_version(request.firmware_version, boot_epoch, firmware_generation))
            return kernel_dispatch::method_result::error(ESTALE);
        u64 ticket = 0;
        const auto result = fixed.prepare_sleep(caller, boot_epoch, firmware_generation, request.sleep_type_a,
                                                request.sleep_type_b, ticket);
        if (result != acpi::sci_operation_result::success)
            return kernel_dispatch::method_result::error(acpi::sci_operation_errno(result));
        response.ticket.ticket = ticket;
        firmware::write_version(response.firmware_version, version_storage, inventory.boot_epoch,
                                inventory.firmware_generation);
        rollback_ticket = ticket;
        rollback_boot_epoch = boot_epoch;
        rollback_firmware_generation = firmware_generation;
        return kernel_dispatch::method_result::success_with_completion(this, finish_transition);
    }

    kernel_dispatch::method_result commit_sleep(const naos::system::AcpiFixedHardware::commit_sleep_request &request,
                                                naos::system::AcpiFixedHardware::commit_sleep_response &response,
                                                const capability::transfer_record_list &,
                                                capability::transfer_record_list &)
    {
        u64 boot_epoch = 0;
        u64 firmware_generation = 0;
        if (!decode_version(request.firmware_version, boot_epoch, firmware_generation))
            return kernel_dispatch::method_result::error(ESTALE);
        u32 outcome = 0;
        const auto result = fixed.commit_sleep(caller, boot_epoch, firmware_generation, request.ticket, outcome);
        if (result != acpi::sci_operation_result::success)
            return kernel_dispatch::method_result::error(acpi::sci_operation_errno(result));
        response.result.outcome = outcome;
        return kernel_dispatch::method_result::success();
    }

    kernel_dispatch::method_result prepare_reset(const naos::system::AcpiFixedHardware::prepare_reset_request &request,
                                                 naos::system::AcpiFixedHardware::prepare_reset_response &response,
                                                 const capability::transfer_record_list &,
                                                 capability::transfer_record_list &)
    {
        u64 boot_epoch = 0;
        u64 firmware_generation = 0;
        if (!decode_version(request.firmware_version, boot_epoch, firmware_generation))
            return kernel_dispatch::method_result::error(ESTALE);
        u64 ticket = 0;
        const auto result = fixed.prepare_reset(caller, boot_epoch, firmware_generation, ticket);
        if (result != acpi::sci_operation_result::success)
            return kernel_dispatch::method_result::error(acpi::sci_operation_errno(result));
        response.ticket.ticket = ticket;
        firmware::write_version(response.firmware_version, version_storage, inventory.boot_epoch,
                                inventory.firmware_generation);
        rollback_ticket = ticket;
        rollback_boot_epoch = boot_epoch;
        rollback_firmware_generation = firmware_generation;
        return kernel_dispatch::method_result::success_with_completion(this, finish_transition);
    }

    kernel_dispatch::method_result commit_reset(const naos::system::AcpiFixedHardware::commit_reset_request &request,
                                                naos::system::AcpiFixedHardware::commit_reset_response &response,
                                                const capability::transfer_record_list &,
                                                capability::transfer_record_list &)
    {
        u64 boot_epoch = 0;
        u64 firmware_generation = 0;
        if (!decode_version(request.firmware_version, boot_epoch, firmware_generation))
            return kernel_dispatch::method_result::error(ESTALE);
        u32 outcome = 0;
        const auto result = fixed.commit_reset(caller, boot_epoch, firmware_generation, request.ticket, outcome);
        if (result != acpi::sci_operation_result::success)
            return kernel_dispatch::method_result::error(acpi::sci_operation_errno(result));
        response.result.outcome = outcome;
        return kernel_dispatch::method_result::success();
    }

    kernel_dispatch::method_result
    abort_transition(const naos::system::AcpiFixedHardware::abort_transition_request &request,
                     naos::system::AcpiFixedHardware::abort_transition_response &,
                     const capability::transfer_record_list &, capability::transfer_record_list &)
    {
        u64 boot_epoch = 0;
        u64 firmware_generation = 0;
        if (!decode_version(request.firmware_version, boot_epoch, firmware_generation))
            return kernel_dispatch::method_result::error(ESTALE);
        const auto result = fixed.abort_transition(caller, boot_epoch, firmware_generation, request.ticket);
        return result == acpi::sci_operation_result::success
                   ? kernel_dispatch::method_result::success()
                   : kernel_dispatch::method_result::error(acpi::sci_operation_errno(result));
    }
};

na_status_t publish_acpi_fixed_hardware_call(invocation_state &state, acpi::fixed_hardware &fixed, u64 method_id,
                                             const freelibcxx::vector<byte> &request, task::process_t *caller)
{
    acpi_fixed_hardware_kernel_handler handler{fixed, caller, fixed.inventory()};
    capability::transfer_record_list resources(memory::KernelCommonAllocatorV);
    return naos::system::AcpiFixedHardware::AcpiFixedHardwareKernelDispatcher::dispatch(handler, state, method_id,
                                                                                        request, resources);
}

struct acpi_resource_authority_kernel_handler
{
    acpi::resource_authority &authority;
    const arch::acpi_boot::snapshot *snapshot;
    task::process_t *caller;
    task::thread_t *caller_thread;
    u64 caller_io_lease_generation;
    u64 rollback_lease_id = 0;
    u64 rollback_boot_epoch = 0;
    u64 rollback_firmware_generation = 0;
    u64 rollback_namespace_generation = 0;
    process_id rollback_owner = 0;

    na_status_t invalid_request() { return NA_STATUS_INVALID_MESSAGE; }

    process_id owner() const { return caller == nullptr ? physical_resource::ledger::kernel_owner() : caller->pid; }

    template <typename String> bool decode_version(const String &token, u64 &boot_epoch, u64 &firmware_generation) const
    {
        return firmware::decode_version(token, boot_epoch, firmware_generation) && boot_epoch == snapshot->boot_epoch &&
               firmware_generation == snapshot->firmware_generation;
    }

    static void fill_grant(const acpi::resource_authority::grant &grant,
                           naos::system::AcpiResourceAuthority::AcpiRegionGrant &encoded)
    {
        encoded.lease_id = grant.lease_id;
        encoded.resource_generation = grant.resource_generation;
        encoded.namespace_generation = grant.namespace_generation;
        encoded.logical_offset = grant.logical_offset;
        encoded.mapping_bytes = grant.mapping_bytes;
        encoded.parent_device_id = grant.parent_device_id;
        encoded.space = static_cast<u32>(grant.space);
        encoded.pci_config_scope_id = grant.pci_config_scope_id;
        encoded.pci_segment = grant.pci_segment;
        encoded.pci_bus = grant.pci_bus;
        encoded.pci_device = grant.pci_device;
        encoded.pci_function = grant.pci_function;
    }

    static acpi::resource_authority::pci_config_request
    decode_pci_request(const naos::system::AcpiResourceAuthority::AcpiRegionRequest &wire)
    {
        acpi::resource_authority::pci_config_request request{};
        request.scope_id = wire.pci_config_scope_id;
        request.root_id = wire.pci_root_id;
        request.segment = wire.pci_segment;
        request.root_start_bus = wire.pci_root_start_bus;
        request.root_end_bus = wire.pci_root_end_bus;
        request.path_length = wire.pci_path_length;
        for (u32 index = 0; index < request.device_function_path.size(); ++index)
            request.device_function_path[index] = wire.pci_device_function_path[index];
        return request;
    }

    static void finish_authorize(void *context, bool committed)
    {
        auto &handler = *static_cast<acpi_resource_authority_kernel_handler *>(context);
        if (!committed && handler.rollback_lease_id != 0)
            (void)handler.authority.release(handler.rollback_boot_epoch, handler.rollback_firmware_generation,
                                            handler.rollback_namespace_generation, handler.rollback_owner,
                                            handler.caller_thread, handler.caller_io_lease_generation,
                                            handler.rollback_lease_id);
    }

    kernel_dispatch::method_result
    validate_pci_root_features(const naos::system::AcpiResourceAuthority::validate_pci_root_features_request &request,
                               naos::system::AcpiResourceAuthority::validate_pci_root_features_response &,
                               const capability::transfer_record_list &, capability::transfer_record_list &)
    {
        if (caller == nullptr)
            return kernel_dispatch::method_result::error(EACCES);
        const auto &wire = request.evidence;
        pci::root_feature_evidence evidence{};
        evidence.root_id = wire.root_id;
        if (!decode_version(request.firmware_version, evidence.boot_epoch, evidence.firmware_generation))
            return kernel_dispatch::method_result::error(ESTALE);
        evidence.aml_namespace_generation = wire.aml_namespace_generation;
        evidence.platform_topology_generation = wire.platform_topology_generation;
        evidence.pci_fabric_generation = wire.pci_fabric_generation;
        evidence.compatibility_profile_hash = wire.compatibility_profile_hash;
        evidence.segment = wire.segment;
        evidence.start_bus = wire.start_bus;
        evidence.end_bus = wire.end_bus;
        evidence.requested_support = wire.requested_support;
        evidence.requested_control = wire.requested_control;
        evidence.query_support = wire.query_support;
        evidence.query_granted_control = wire.query_granted_control;
        evidence.commit_support = wire.commit_support;
        evidence.granted_control = wire.granted_control;
        evidence.denied_control = wire.denied_control;
        evidence.masked_control = wire.masked_control;
        evidence.query_errors = wire.query_errors;
        evidence.commit_errors = wire.commit_errors;
        const auto status = authority.validate_pci_root_features(caller->pid, wire.pci_config_scope_id, evidence);
        if (status == NA_STATUS_OK)
            return kernel_dispatch::method_result::success();
        const i64 error = status == NA_STATUS_NOT_SUPPORTED      ? ENOTSUP
                          : status == NA_STATUS_INVALID_ARGUMENT ? EINVAL
                                                                 : EACCES;
        return kernel_dispatch::method_result::error(error);
    }

    kernel_dispatch::method_result mint_pci_root_resource_attestation(
        const naos::system::AcpiResourceAuthority::mint_pci_root_resource_attestation_request &wire,
        naos::system::AcpiResourceAuthority::mint_pci_root_resource_attestation_response &response,
        const capability::transfer_record_list &, capability::transfer_record_list &response_resources)
    {
        if (caller == nullptr)
            return kernel_dispatch::method_result::error(EACCES);
        pci::root_resource_evidence evidence{};
        evidence.root_id = wire.root_id;
        if (!decode_version(wire.firmware_version, evidence.boot_epoch, evidence.firmware_generation))
            return kernel_dispatch::method_result::error(ESTALE);
        evidence.aml_namespace_generation = wire.aml_namespace_generation;
        evidence.platform_topology_generation = wire.platform_topology_generation;
        evidence.pci_fabric_generation = wire.pci_fabric_generation;
        evidence.compatibility_profile_hash = wire.compatibility_profile_hash;
        evidence.segment = wire.segment;
        evidence.start_bus = wire.start_bus;
        evidence.end_bus = wire.end_bus;
        evidence.window_count = wire.windows.count;
        for (u32 index = 0; index < wire.windows.count; ++index)
        {
            const auto &source = wire.windows.data[index];
            evidence.windows[index] = {source.resource_type, source.descriptor_revision,
                                       source.general_flags, source.type_specific_flags,
                                       source.granularity,   source.minimum,
                                       source.maximum,       source.translation_offset,
                                       source.length,        source.type_specific_attributes};
        }
        handle_t<pci::root_resource_attestation_capability> attestation;
        const auto status =
            authority.authorize_pci_root_resources(caller->pid, wire.pci_config_scope_id, evidence, attestation);
        if (status != NA_STATUS_OK)
        {
            const i64 error = status == NA_STATUS_NOT_SUPPORTED        ? ENOTSUP
                              : status == NA_STATUS_INVALID_ARGUMENT   ? EINVAL
                              : status == NA_STATUS_RESOURCE_EXHAUSTED ? ENOMEM
                              : status == NA_STATUS_IO_ERROR           ? EIO
                                                                       : EACCES;
            return kernel_dispatch::method_result::error(error);
        }
        auto metadata = kernel_view_metadata(
            NA_SCOPE_PCI_ROOT_RESOURCE_ATTESTATION, naos::system::PciRootResourceAttestation::protocol_uuid,
            NA_RIGHT_WAIT | NA_RIGHT_INSPECT | NA_RIGHT_TRANSFER, NA_PROTOCOL_RIGHT_INVOKE,
            naos::system::PciRootResourceAttestation::revision, naos::system::PciRootResourceAttestation::features);
        response.authority.value = 0;
        response_resources.push_back(capability::transfer_record(
            NA_HANDLE_INVALID, true,
            capability::transferred_resource(handle_t<kobject>(attestation.get_control()), metadata)));
        return kernel_dispatch::method_result::success();
    }

    kernel_dispatch::method_result
    bind_execution_thread(const naos::system::AcpiResourceAuthority::bind_execution_thread_request &request,
                          naos::system::AcpiResourceAuthority::bind_execution_thread_response &,
                          const capability::transfer_record_list &, capability::transfer_record_list &)
    {
        u64 boot_epoch = 0;
        u64 firmware_generation = 0;
        if (caller == nullptr || !decode_version(request.firmware_version, boot_epoch, firmware_generation) ||
            !authority.bind_execution_thread(boot_epoch, firmware_generation, caller->pid, caller_thread,
                                             request.request.execution_thread_id))
            return kernel_dispatch::method_result::error(EACCES);
        return kernel_dispatch::method_result::success();
    }

    kernel_dispatch::method_result
    bind_platform_generation(const naos::system::AcpiResourceAuthority::bind_platform_generation_request &request,
                             naos::system::AcpiResourceAuthority::bind_platform_generation_response &,
                             const capability::transfer_record_list &, capability::transfer_record_list &)
    {
        const auto &binding = request.binding;
        u64 boot_epoch = 0;
        u64 firmware_generation = 0;
        if (caller == nullptr || !decode_version(request.firmware_version, boot_epoch, firmware_generation) ||
            !authority.bind_platform_generation(boot_epoch, firmware_generation, binding.aml_namespace_generation,
                                                binding.platform_topology_generation, caller->pid))
            return kernel_dispatch::method_result::error(ESTALE);
        return kernel_dispatch::method_result::success();
    }

    kernel_dispatch::method_result
    acquire_global_lock(const naos::system::AcpiResourceAuthority::acquire_global_lock_request &request,
                        naos::system::AcpiResourceAuthority::acquire_global_lock_response &response,
                        const capability::transfer_record_list &, capability::transfer_record_list &)
    {
        if (caller == nullptr)
            return kernel_dispatch::method_result::error(EACCES);
        const auto &lock_request = request.request;
        u64 boot_epoch = 0;
        u64 firmware_generation = 0;
        if (!decode_version(request.firmware_version, boot_epoch, firmware_generation))
            return kernel_dispatch::method_result::error(ESTALE);
        const auto result = authority.acquire_global_lock(boot_epoch, firmware_generation, caller->pid, caller_thread,
                                                          lock_request.timeout_us);
        if (result != acpi::global_lock_operation_result::success)
            KLOG_WARN("ACPI Global Lock acquire failed result={} caller_tid={} current_tid={}",
                      static_cast<u32>(result), caller_thread == nullptr ? 0 : caller_thread->tid,
                      task::current() == nullptr ? 0 : task::current()->tid);
        if (result == acpi::global_lock_operation_result::access_denied)
            return kernel_dispatch::method_result::error(EACCES);
        if (result == acpi::global_lock_operation_result::stale_generation)
            return kernel_dispatch::method_result::error(ESTALE);
        if (result == acpi::global_lock_operation_result::resource_revoked)
            return kernel_dispatch::method_result::error(EIDRM);
        switch (result)
        {
            case acpi::global_lock_operation_result::success:
                response.response.outcome = 0;
                break;
            case acpi::global_lock_operation_result::timed_out:
                response.response.outcome = 1;
                break;
            case acpi::global_lock_operation_result::unsupported:
                response.response.outcome = 2;
                break;
            case acpi::global_lock_operation_result::io_error:
            case acpi::global_lock_operation_result::not_acquired:
                response.response.outcome = 3;
                break;
            case acpi::global_lock_operation_result::access_denied:
            case acpi::global_lock_operation_result::resource_revoked:
            case acpi::global_lock_operation_result::stale_generation:
                return kernel_dispatch::method_result::error(EINVAL);
        }
        return kernel_dispatch::method_result::success();
    }

    kernel_dispatch::method_result
    release_global_lock(const naos::system::AcpiResourceAuthority::release_global_lock_request &request,
                        naos::system::AcpiResourceAuthority::release_global_lock_response &response,
                        const capability::transfer_record_list &, capability::transfer_record_list &)
    {
        if (caller == nullptr)
            return kernel_dispatch::method_result::error(EACCES);
        u64 boot_epoch = 0;
        u64 firmware_generation = 0;
        if (!decode_version(request.firmware_version, boot_epoch, firmware_generation))
            return kernel_dispatch::method_result::error(ESTALE);
        const auto result = authority.release_global_lock(boot_epoch, firmware_generation, caller->pid, caller_thread);
        if (result == acpi::global_lock_operation_result::access_denied)
            return kernel_dispatch::method_result::error(EACCES);
        if (result == acpi::global_lock_operation_result::stale_generation)
            return kernel_dispatch::method_result::error(ESTALE);
        if (result == acpi::global_lock_operation_result::resource_revoked)
            return kernel_dispatch::method_result::error(EIDRM);
        response.response.outcome = result == acpi::global_lock_operation_result::success       ? 0u
                                    : result == acpi::global_lock_operation_result::unsupported ? 2u
                                                                                                : 3u;
        return kernel_dispatch::method_result::success();
    }

    kernel_dispatch::method_result
    bind_pci_config_root(const naos::system::AcpiResourceAuthority::bind_pci_config_root_request &request,
                         naos::system::AcpiResourceAuthority::bind_pci_config_root_response &response,
                         const capability::transfer_record_list &, capability::transfer_record_list &)
    {
        u64 scope_id = 0;
        u64 boot_epoch = 0;
        u64 firmware_generation = 0;
        if (caller == nullptr || !decode_version(request.firmware_version, boot_epoch, firmware_generation) ||
            !authority.bind_pci_config_root(boot_epoch, firmware_generation, request.request.namespace_generation,
                                            request.request.platform_topology_generation,
                                            request.request.compatibility_profile_hash, caller->pid,
                                            request.request.root_id, request.request.segment, request.request.start_bus,
                                            request.request.end_bus, scope_id))
            return kernel_dispatch::method_result::error(EACCES);
        response.response.scope_id = scope_id;
        return kernel_dispatch::method_result::success();
    }

    kernel_dispatch::method_result
    authorize_region(const naos::system::AcpiResourceAuthority::authorize_region_request &request,
                     naos::system::AcpiResourceAuthority::authorize_region_response &response,
                     const capability::transfer_record_list &, capability::transfer_record_list &)
    {
        const auto &wire = request.request;
        acpi::resource_authority::grant grant{};
        const auto request_owner = owner();
        u64 boot_epoch = 0;
        u64 firmware_generation = 0;
        if (!decode_version(request.firmware_version, boot_epoch, firmware_generation))
            return kernel_dispatch::method_result::error(ESTALE);
        if (!authority.authorize(static_cast<acpi::resource_authority::region_space>(wire.space), wire.mode, boot_epoch,
                                 firmware_generation, wire.namespace_generation, wire.parent_device_id,
                                 decode_pci_request(wire), wire.base, wire.length, request_owner, caller_thread,
                                 caller_io_lease_generation, grant))
            return kernel_dispatch::method_result::error(EACCES);
        fill_grant(grant, response.grant);
        rollback_lease_id = grant.lease_id;
        rollback_boot_epoch = boot_epoch;
        rollback_firmware_generation = firmware_generation;
        rollback_namespace_generation = wire.namespace_generation;
        rollback_owner = request_owner;
        return kernel_dispatch::method_result::success_with_completion(this, finish_authorize);
    }

    kernel_dispatch::method_result map_region(const naos::system::AcpiResourceAuthority::map_region_request &request,
                                              naos::system::AcpiResourceAuthority::map_region_response &response,
                                              const capability::transfer_record_list &,
                                              capability::transfer_record_list &response_resources)
    {
        const auto &wire = request.request;
        acpi::resource_authority::grant requested{};
        u64 boot_epoch = 0;
        u64 firmware_generation = 0;
        if (!decode_version(request.firmware_version, boot_epoch, firmware_generation))
            return kernel_dispatch::method_result::error(ESTALE);
        requested.lease_id = request.lease_id;
        requested.resource_generation = firmware_generation;
        requested.namespace_generation = wire.namespace_generation;
        requested.parent_device_id = wire.parent_device_id;
        requested.pci_config_scope_id = wire.pci_config_scope_id;
        requested.pci_segment = wire.pci_segment;
        requested.space = static_cast<acpi::resource_authority::region_space>(wire.space);
        handle_t<naos::data_plane::memory_object> object;
        acpi::resource_authority::grant grant{};
        if (!authority.map(requested, boot_epoch, firmware_generation, wire.namespace_generation, wire.parent_device_id,
                           decode_pci_request(wire), wire.base, wire.length, wire.mode, object, owner(), caller_thread,
                           grant))
            return kernel_dispatch::method_result::error(EACCES);
        fill_grant(grant, response.grant);
        response.mapping.value = 0;
        const u64 rights = NA_MEMORY_RIGHT_READ | NA_MEMORY_RIGHT_MAP | NA_MEMORY_RIGHT_INFO |
                           (wire.mode == 1 ? NA_MEMORY_RIGHT_WRITE : 0);
        auto metadata = kernel_view_metadata(
            NA_SCOPE_MEMORY_OBJECT, naos::system::MemoryObject::protocol_uuid, NA_RIGHT_WAIT | NA_RIGHT_INSPECT, rights,
            naos::system::MemoryObject::revision, naos::system::MemoryObject::features);
        metadata.binding = NA_BINDING_MEMORY_OBJECT;
        metadata.view_offset = 0;
        metadata.view_length = grant.mapping_bytes;
        response_resources.push_back(capability::transfer_record(
            NA_HANDLE_INVALID, true, capability::transferred_resource(std::move(object), metadata)));
        return kernel_dispatch::method_result::success();
    }

    kernel_dispatch::method_result
    release_region(const naos::system::AcpiResourceAuthority::release_region_request &request,
                   naos::system::AcpiResourceAuthority::release_region_response &,
                   const capability::transfer_record_list &, capability::transfer_record_list &)
    {
        u64 boot_epoch = 0;
        u64 firmware_generation = 0;
        if (!decode_version(request.firmware_version, boot_epoch, firmware_generation) ||
            !authority.release(boot_epoch, firmware_generation, request.namespace_generation, owner(), caller_thread,
                               caller_io_lease_generation, request.lease_id))
            return kernel_dispatch::method_result::error(EACCES);
        return kernel_dispatch::method_result::success();
    }
};

na_status_t publish_acpi_resource_authority_call(invocation_state &state, acpi::resource_authority &authority,
                                                 u64 method_id, const freelibcxx::vector<byte> &request,
                                                 task::process_t *caller, task::thread_t *caller_thread,
                                                 u64 caller_io_lease_generation)
{
    const auto *snapshot = authority.snapshot();
    if (snapshot == nullptr)
        return state.complete_reply(empty_bytes(), empty_resources(), EIO) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
    acpi_resource_authority_kernel_handler handler{authority, snapshot, caller, caller_thread,
                                                   caller_io_lease_generation};
    capability::transfer_record_list resources(memory::KernelCommonAllocatorV);
    return naos::system::AcpiResourceAuthority::AcpiResourceAuthorityKernelDispatcher::dispatch(
        handler, state, method_id, request, resources);
}

} // namespace

namespace
{
na_status_t dispatch_firmware_root(kernel_dispatch::kernel_view_context &context, firmware::root &root)
{
    return publish_firmware_root_call(context.state, root, context.method_id, context.request);
}

na_status_t dispatch_acpi_fixed_hardware(kernel_dispatch::kernel_view_context &context, acpi::fixed_hardware &fixed)
{
    return publish_acpi_fixed_hardware_call(context.state, fixed, context.method_id, context.request, context.caller);
}

na_status_t dispatch_acpi_resource_authority(kernel_dispatch::kernel_view_context &context,
                                             acpi::resource_authority &authority)
{
    auto *caller_thread = context.caller == nullptr ? nullptr : task::find_tid(context.caller, context.caller_tid);
    if (!task::direct_io_lease_generation_is_current(caller_thread, context.caller_io_lease_generation))
        return NA_STATUS_ACCESS_DENIED;
    return publish_acpi_resource_authority_call(context.state, authority, context.method_id, context.request,
                                                context.caller, caller_thread, context.caller_io_lease_generation);
}

constexpr kernel_dispatch::kernel_view_route firmware_root_kernel_view_route_entries[] = {
    kernel_dispatch::make_kernel_view_route<firmware::root, dispatch_firmware_root>(
        NA_SCOPE_FIRMWARE_ROOT, naos::system::FirmwareRoot::descriptor),
};

constexpr kernel_dispatch::kernel_view_route acpi_kernel_view_route_entries[] = {
    kernel_dispatch::make_kernel_view_route<acpi::fixed_hardware, dispatch_acpi_fixed_hardware>(
        NA_SCOPE_ACPI_FIXED_HARDWARE, naos::system::AcpiFixedHardware::descriptor),
    kernel_dispatch::make_kernel_view_route<acpi::resource_authority, dispatch_acpi_resource_authority>(
        NA_SCOPE_ACPI_RESOURCE_AUTHORITY, naos::system::AcpiResourceAuthority::descriptor),
};
} // namespace

kernel_dispatch::kernel_view_route_table firmware_root_kernel_view_routes() noexcept
{
    return {firmware_root_kernel_view_route_entries, 1};
}

kernel_dispatch::kernel_view_route_table acpi_kernel_view_routes() noexcept
{
    return {acpi_kernel_view_route_entries,
            sizeof(acpi_kernel_view_route_entries) / sizeof(acpi_kernel_view_route_entries[0])};
}
} // namespace naos::ipc
