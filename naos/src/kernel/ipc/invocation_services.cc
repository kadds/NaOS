#include "invocation_handlers.hpp"
#include "invocation_region_support.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/errno.hpp"
#include "kernel/ipc/channel.hpp"
#include "kernel/ipc/invocation.hpp"
#include "kernel/log.hpp"
#include "kernel/mm/data_plane.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/service_directory.hpp"
#include "kernel/system_status.hpp"
#include "kernel/task.hpp"
#include "kernel/timer.hpp"
#include "kernel/ucontext.hpp"
#include "kernel/usercopy.hpp"
#include "naos/generated/system/MemoryObject.hpp"
#include "naos/generated/system/Process.hpp"
#include "naos/generated/system/ServiceDirectory.hpp"
#include "naos/generated/system/ServiceDirectory_kernel_dispatch.hpp"
#include "naos/generated/system/SystemStatus.hpp"
#include "naos/generated/system/SystemStatus_kernel_dispatch.hpp"
#include "naos/generated/system_uapi.h"
#include <limits>

KLOG_MODULE(ipc);

namespace naos::ipc::dispatch_detail
{
std::atomic_uint64_t status_sample_sequence{0};
}

namespace naos::ipc
{
namespace
{
using dispatch_detail::decode_message;
using dispatch_detail::empty_bytes;
using dispatch_detail::empty_resources;
using dispatch_detail::encode_message;
using dispatch_detail::kernel_view_metadata;
using dispatch_detail::max_kernel_payload;
using dispatch_detail::request_region_object;
using dispatch_detail::status_sample_sequence;

bool service_directory_mutation_allowed(const service::directory &directory, const char *data, u32 size,
                                        u64 protocol_rights)
{
    if ((protocol_rights & NA_SERVICE_DIRECTORY_RIGHT_LOCAL_REGISTER) != 0 && directory.allows_local_registration())
        return true;
    if ((protocol_rights & NA_SERVICE_DIRECTORY_RIGHT_PROVIDER) != 0 && directory.allows_provider_publish(data, size))
        return true;

    constexpr char system_prefix[] = "naos://system/";
    constexpr char service_prefix[] = "naos://service/";
    const bool system_namespace =
        size >= sizeof(system_prefix) - 1 && memcmp(data, system_prefix, sizeof(system_prefix) - 1) == 0;
    const bool service_namespace =
        size >= sizeof(service_prefix) - 1 && memcmp(data, service_prefix, sizeof(service_prefix) - 1) == 0;
    if (system_namespace || service_namespace)
        return (protocol_rights & NA_SERVICE_DIRECTORY_RIGHT_SYSTEM_MANAGER) != 0;
    return (protocol_rights & NA_SERVICE_DIRECTORY_RIGHT_ADMIN) != 0;
}

struct service_directory_kernel_handler
{
    service::directory &directory;
    const handle_t<service::directory> &directory_handle;
    u64 protocol_rights;
    u64 meta_rights;
    task::process_t *caller;

    na_status_t invalid_request() { return NA_STATUS_INVALID_MESSAGE; }

    kernel_dispatch::method_result register_(const naos::system::ServiceDirectory::register_request &request,
                                             naos::system::ServiceDirectory::register_response &,
                                             capability::transfer_record_list &resources,
                                             capability::transfer_record_list &)
    {
        if (request.service.value >= resources.size())
            return kernel_dispatch::method_result::error(EINVAL);
        if (!service_directory_mutation_allowed(directory, request.uri.data, request.uri.size, protocol_rights))
            return kernel_dispatch::method_result::error(EACCES);
        const auto result =
            directory.register_service(reinterpret_cast<const char *>(request.uri.data), request.uri.size,
                                       resources.at(request.service.value), false, caller->pid);
        return result == 0 ? kernel_dispatch::method_result::success() : kernel_dispatch::method_result::error(result);
    }

    kernel_dispatch::method_result resolve(const naos::system::ServiceDirectory::resolve_request &request,
                                           naos::system::ServiceDirectory::resolve_response &response,
                                           const capability::transfer_record_list &,
                                           capability::transfer_record_list &response_resources)
    {
        capability::transferred_resource resource;
        const auto result =
            directory.resolve_service(reinterpret_cast<const char *>(request.uri.data), request.uri.size, resource);
        if (result != 0)
            return kernel_dispatch::method_result::error(result);
        response.service.value = 0;
        response_resources.push_back(capability::transfer_record(NA_HANDLE_INVALID, true, std::move(resource)));
        return kernel_dispatch::method_result::success();
    }

    kernel_dispatch::method_result unregister(const naos::system::ServiceDirectory::unregister_request &request,
                                              naos::system::ServiceDirectory::unregister_response &,
                                              const capability::transfer_record_list &,
                                              capability::transfer_record_list &)
    {
        if (!service_directory_mutation_allowed(directory, request.uri.data, request.uri.size, protocol_rights))
            return kernel_dispatch::method_result::error(EACCES);
        const auto result = directory.unregister_service(reinterpret_cast<const char *>(request.uri.data),
                                                         request.uri.size, caller->pid);
        return result == 0 ? kernel_dispatch::method_result::success() : kernel_dispatch::method_result::error(result);
    }

    kernel_dispatch::method_result list(const naos::system::ServiceDirectory::list_request &request,
                                        naos::system::ServiceDirectory::list_response &response,
                                        capability::transfer_record_list &resources, capability::transfer_record_list &)
    {
        if (request.requested_bytes > max_kernel_payload)
            return kernel_dispatch::method_result::error(EINVAL);
        u64 region_offset = 0;
        auto *region =
            request_region_object(resources, request.buffer.value, NA_MEMORY_RIGHT_MAP | NA_MEMORY_RIGHT_WRITE, 0,
                                  request.requested_bytes, region_offset);
        if (region == nullptr || !region->writable(region_offset, request.requested_bytes))
            return kernel_dispatch::method_result::error(EINVAL);

        freelibcxx::vector<byte> records(memory::MemoryAllocatorV);
        u64 next = 0;
        u64 count = 0;
        const auto result =
            directory.list_services(nullptr, 0, request.offset, request.requested_bytes, records, next, count);
        if (result != 0)
            return kernel_dispatch::method_result::error(result);
        u64 written_bytes = 0;
        if (region->write(region_offset, records.data(), records.size(), written_bytes) != NA_STATUS_OK)
            return kernel_dispatch::method_result::error(EINVAL);
        response.next = next;
        response.count = count;
        response.bytes = written_bytes;
        return kernel_dispatch::method_result::success();
    }

    kernel_dispatch::method_result list_prefix(const naos::system::ServiceDirectory::list_prefix_request &request,
                                               naos::system::ServiceDirectory::list_prefix_response &response,
                                               capability::transfer_record_list &resources,
                                               capability::transfer_record_list &)
    {
        if (request.requested_bytes > max_kernel_payload)
            return kernel_dispatch::method_result::error(EINVAL);
        u64 region_offset = 0;
        auto *region =
            request_region_object(resources, request.buffer.value, NA_MEMORY_RIGHT_MAP | NA_MEMORY_RIGHT_WRITE, 0,
                                  request.requested_bytes, region_offset);
        if (region == nullptr || !region->writable(region_offset, request.requested_bytes))
            return kernel_dispatch::method_result::error(EINVAL);

        freelibcxx::vector<byte> records(memory::MemoryAllocatorV);
        u64 next = 0;
        u64 count = 0;
        const auto result =
            directory.list_services(reinterpret_cast<const char *>(request.prefix.data), request.prefix.size,
                                    request.offset, request.requested_bytes, records, next, count);
        if (result != 0)
            return kernel_dispatch::method_result::error(result);
        u64 written_bytes = 0;
        if (region->write(region_offset, records.data(), records.size(), written_bytes) != NA_STATUS_OK)
            return kernel_dispatch::method_result::error(EINVAL);
        response.next = next;
        response.count = count;
        response.bytes = written_bytes;
        return kernel_dispatch::method_result::success();
    }

    kernel_dispatch::method_result listen(const naos::system::ServiceDirectory::listen_request &request,
                                          naos::system::ServiceDirectory::listen_response &,
                                          capability::transfer_record_list &resources,
                                          capability::transfer_record_list &)
    {
        if (request.listener.value >= resources.size() || request.descriptor.value >= resources.size() ||
            request.listener.value == request.descriptor.value)
            return kernel_dispatch::method_result::error(EINVAL);
        if (!service_directory_mutation_allowed(directory, request.uri.data, request.uri.size, protocol_rights))
            return kernel_dispatch::method_result::error(EACCES);
        khandle send_endpoint = resources.at(request.listener.value).resource.take_object_to_table();
        khandle descriptor = resources.at(request.descriptor.value).resource.take_object_to_table();
        const auto result =
            directory.listen_service(reinterpret_cast<const char *>(request.uri.data), request.uri.size,
                                     std::move(send_endpoint), std::move(descriptor), request.max_pending, caller->pid);
        return result == 0 ? kernel_dispatch::method_result::success() : kernel_dispatch::method_result::error(result);
    }

    kernel_dispatch::method_result connect(const naos::system::ServiceDirectory::connect_request &request,
                                           naos::system::ServiceDirectory::connect_response &response,
                                           const capability::transfer_record_list &,
                                           capability::transfer_record_list &response_resources)
    {
        na_uuid_t expected_uuid{};
        memcpy(expected_uuid.bytes, request.expected_uuid.data(), sizeof(expected_uuid.bytes));
        capability::transferred_resource client_resource;
        response.client.value = 0;
        const auto result =
            directory.connect_service(reinterpret_cast<const char *>(request.uri.data), request.uri.size, expected_uuid,
                                      request.requested_rights, request.requested_revision, request.requested_features,
                                      caller, client_resource, response.revision, response.features);
        if (result != 0)
            return kernel_dispatch::method_result::error(result);
        response_resources.push_back(capability::transfer_record(NA_HANDLE_INVALID, true, std::move(client_resource)));
        return kernel_dispatch::method_result::success();
    }

    kernel_dispatch::method_result create_view(const naos::system::ServiceDirectory::create_view_request &request,
                                               naos::system::ServiceDirectory::create_view_response &response,
                                               const capability::transfer_record_list &,
                                               capability::transfer_record_list &response_resources)
    {
        if ((meta_rights & NA_RIGHT_TRANSFER) == 0)
            return kernel_dispatch::method_result::error(EACCES);
        if (request.uri.size == 0 || request.uri.size > 255)
            return kernel_dispatch::method_result::error(EINVAL);

        const auto *uri = reinterpret_cast<const char *>(request.uri.data);
        const auto validation = directory.validate_view_uri(uri, request.uri.size);
        if (validation != 0)
            return kernel_dispatch::method_result::error(validation);
        char scoped_uri[256]{};
        memcpy(scoped_uri, uri, request.uri.size);
        const char *scoped_uris[] = {scoped_uri};
        auto view = service::directory::make_scoped_view(directory_handle, scoped_uris, 1);
        if (!view)
            return kernel_dispatch::method_result::error(ENOMEM);

        auto metadata = kernel_view_metadata(
            NA_SCOPE_SERVICE_DIRECTORY, naos::system::ServiceDirectory::protocol_uuid,
            meta_rights & (NA_RIGHT_DUPLICATE | NA_RIGHT_TRANSFER | NA_RIGHT_WAIT | NA_RIGHT_INSPECT),
            NA_PROTOCOL_RIGHT_INVOKE, naos::system::ServiceDirectory::revision,
            naos::system::ServiceDirectory::features);
        response.view.value = 0;
        response_resources.push_back(capability::transfer_record(
            NA_HANDLE_INVALID, true,
            capability::transferred_resource(handle_t<kobject>(view.get_control()), metadata)));
        return kernel_dispatch::method_result::success();
    }
};

na_status_t publish_service_directory_call(invocation_state &state, service::directory &directory,
                                           const handle_t<service::directory> &directory_handle, u64 method_id,
                                           const freelibcxx::vector<byte> &request,
                                           capability::transfer_record_list &resources, u64 protocol_rights,
                                           u64 meta_rights, task::process_t *caller)
{
    service_directory_kernel_handler handler{directory, directory_handle, protocol_rights, meta_rights, caller};
    return naos::system::ServiceDirectory::ServiceDirectoryKernelDispatcher::dispatch(handler, state, method_id,
                                                                                      request, resources);
}

struct system_status_kernel_handler
{
    na_status_t invalid_request() { return NA_STATUS_INVALID_MESSAGE; }

    kernel_dispatch::method_result list_processes(const naos::system::SystemStatus::list_processes_request &request,
                                                  naos::system::SystemStatus::list_processes_response &response,
                                                  const capability::transfer_record_list &,
                                                  capability::transfer_record_list &response_resources)
    {
        freelibcxx::vector<task::process_snapshot_entry> entries(memory::KernelCommonAllocatorV);
        process_id next_pid = 0;
        const auto status = task::open_process_snapshot(request.after_pid, 64, entries, next_pid);
        if (status != 0)
            return kernel_dispatch::method_result::error(status);

        response.next_after_pid = next_pid;
        response.processes.data = response.processes_storage.data();
        response.processes.count = static_cast<u32>(entries.size());
        for (u32 index = 0; index < response.processes.count; index++)
        {
            response.processes.data[index].pid = entries[index].pid;
            response.processes.data[index].process.value = index;
        }

        const auto process_meta = kernel_view_metadata(
            NA_SCOPE_PROCESS, naos::system::Process::protocol_uuid, NA_RIGHT_TRANSFER | NA_RIGHT_INSPECT,
            NA_PROCESS_RIGHT_INSPECT, naos::system::Process::revision, naos::system::Process::features);
        for (auto &entry : entries)
        {
            response_resources.push_back(capability::transfer_record(
                NA_HANDLE_INVALID, true, capability::transferred_resource(std::move(entry.object), process_meta)));
        }
        return kernel_dispatch::method_result::success();
    }

    kernel_dispatch::method_result get(const naos::system::SystemStatus::get_request &,
                                       naos::system::SystemStatus::get_response &response,
                                       const capability::transfer_record_list &, capability::transfer_record_list &)
    {
        const auto allocator = memory::allocator_status();
        task::memory_accounting_t accounting;
        task::get_system_memory_accounting(accounting);
        response.value.sample_sequence = status_sample_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
        response.value.sample_time_us = timer::get_high_resolution_time();
        response.value.page_size = memory::page_size;
        response.value.physical_pages = allocator.physical_pages;
        response.value.usable_pages = allocator.usable_pages;
        response.value.reserved_pages = allocator.reserved_pages;
        response.value.free_pages = allocator.free_pages;
        response.value.available_pages = allocator.available_pages;
        response.value.anonymous_pages = accounting.anonymous_pages;
        response.value.file_cache_pages = accounting.file_cache_pages;
        response.value.shared_pages = accounting.shared_pages;
        response.value.reclaimable_pages = allocator.available_pages - allocator.free_pages;
        response.value.kernel_reclaimable_pages = 0;
        const u64 allocated_pages =
            allocator.usable_pages > allocator.free_pages ? allocator.usable_pages - allocator.free_pages : 0;
        // User mappings are currently reported as mapping counts, so this is a
        // conservative attribution rather than a unique-frame total when shared mappings exist.
        response.value.kernel_pages =
            allocated_pages > accounting.rss_pages ? allocated_pages - accounting.rss_pages : 0;
        response.value.kernel_unreclaimable_pages = response.value.kernel_pages;
        response.value.user_resident_pages = accounting.rss_pages;
        response.value.user_committed_pages = accounting.committed_pages;
        response.value.committed_pages = accounting.committed_pages;
        response.value.commit_limit_pages = allocator.usable_pages;
        // Swap is a deliberate ABI placeholder. NaOS has no swap backend yet.
        response.value.swap_total_pages = 0;
        response.value.swap_free_pages = 0;
        response.value.swap_used_pages = 0;
        response.value.process_count = accounting.process_count;
        response.value.thread_count = accounting.thread_count;
        response.value.vma_count = accounting.vma_count;
        response.value.page_faults = accounting.page_faults;
        response.value.allocation_failures = 0;
        response.value.flags = 0;
        return kernel_dispatch::method_result::success();
    }
};

na_status_t publish_system_status_call(invocation_state &state, system_status_service::object &, u64 method_id,
                                       const freelibcxx::vector<byte> &request)
{
    capability::transfer_record_list resources(memory::KernelCommonAllocatorV);
    system_status_kernel_handler handler;
    return naos::system::SystemStatus::SystemStatusKernelDispatcher::dispatch(handler, state, method_id, request,
                                                                              resources);
}

} // namespace

namespace
{
na_status_t dispatch_system_status(kernel_dispatch::kernel_view_context &context, system_status_service::object &object)
{
    return publish_system_status_call(context.state, object, context.method_id, context.request);
}

na_status_t dispatch_service_directory(kernel_dispatch::kernel_view_context &context, service::directory &object)
{
    const handle_t<service::directory> directory_handle(context.target.object.get_control());
    return publish_service_directory_call(context.state, object, directory_handle, context.method_id, context.request,
                                          context.resources, context.target.meta.protocol_rights,
                                          context.target.meta.meta_rights, context.caller);
}

constexpr kernel_dispatch::kernel_view_route service_kernel_view_route_entries[] = {
    kernel_dispatch::make_kernel_view_route<system_status_service::object, dispatch_system_status>(
        NA_SCOPE_SYSTEM_STATUS, naos::system::SystemStatus::descriptor),
    kernel_dispatch::make_kernel_view_route<service::directory, dispatch_service_directory>(
        NA_SCOPE_SERVICE_DIRECTORY, naos::system::ServiceDirectory::descriptor),
};
} // namespace

kernel_dispatch::kernel_view_route_table service_kernel_view_routes() noexcept
{
    return {service_kernel_view_route_entries,
            sizeof(service_kernel_view_route_entries) / sizeof(service_kernel_view_route_entries[0])};
}
} // namespace naos::ipc
