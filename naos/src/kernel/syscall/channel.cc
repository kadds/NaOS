#include "kernel/ipc/channel.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/dev/framebuffer.hpp"
#include "kernel/input_event_source.hpp"
#include "kernel/ipc/invocation.hpp"
#include "kernel/ipc/epoll.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/service_directory.hpp"
#include "kernel/syscall.hpp"
#include "kernel/task.hpp"
#include "kernel/terminal_views.hpp"
#include "kernel/time.hpp"
#include "kernel/usercopy.hpp"
#include "naos/bootstrap.hpp"
#include "naos/generated/system/Directory.hpp"
#include "naos/generated/system/InputEventSource.hpp"
#include "naos/generated/system/ServiceDirectory.hpp"
#include "naos/generated/system/Stream.hpp"
#include "naos/generated/system/TerminalDriverFactory.hpp"
#include "naos/generated/system/TerminalMaster.hpp"
#include "naos/generated/system/TerminalSlave.hpp"
#include "naos/generated/system_uapi.h"
#include <limits>

namespace naos::syscall
{
KLOG_MODULE(ipc);
namespace
{
bool protocol_uuid_matches(const na_uuid_t &left, const na_uuid_t &right)
{
    return memcmp(left.bytes, right.bytes, sizeof(left.bytes)) == 0;
}

bool valid_output_handle(na_handle_t *handle)
{
    return handle != nullptr && is_user_space_range(handle, sizeof(*handle));
}

na_status_t write_handle(na_handle_t *destination, na_handle_t value)
{
    return naos::usercopy::copy_to(reinterpret_cast<u64>(destination), &value, sizeof(value));
}

capability::metadata stream_metadata()
{
    capability::metadata metadata;
    metadata.binding = NA_BINDING_KERNEL_VIEW;
    metadata.protocol_uuid = naos::system::Stream::protocol_uuid;
    metadata.scope = NA_SCOPE_STREAM;
    metadata.revision = naos::system::Stream::revision;
    metadata.meta_rights = NA_RIGHT_DUPLICATE | NA_RIGHT_TRANSFER | NA_RIGHT_WAIT | NA_RIGHT_INSPECT;
    metadata.protocol_rights = NA_PROTOCOL_RIGHT_INVOKE;
    return metadata;
}

capability::metadata bootstrap_stdio_metadata(const capability::metadata &source)
{
    if (source.binding == NA_BINDING_CLIENT_END &&
        (source.scope == NA_SCOPE_TERMINAL_MASTER || source.scope == NA_SCOPE_TERMINAL_SLAVE))
        return source;
    return stream_metadata();
}

void close_received_handles(task::resource_table_t &resources, const freelibcxx::vector<na_handle_t> &handles)
{
    for (auto handle : handles)
    {
        if (handle != NA_HANDLE_INVALID)
            resources.close_native(handle);
    }
}

bool valid_bootstrap_directory(task::resource_table_t &resources, na_handle_t handle)
{
    capability::entry entry;
    if (!(resources.lookup_native(handle, entry) && entry.object))
        return false;
    // Directory bootstrap is a userspace namespace capability.  A kernel
    // Directory view is deliberately not accepted here: accepting it would
    // let the bootstrap syscall silently recreate the removed global-root
    // fallback even when no vfsd endpoint was transferred.
    return entry.meta.binding == NA_BINDING_CLIENT_END && entry.meta.scope == NA_SCOPE_DIRECTORY &&
           protocol_uuid_matches(entry.meta.protocol_uuid, naos::system::Directory::protocol_uuid) &&
           entry.meta.revision == naos::system::Directory::revision;
}

bool valid_bootstrap_stream(task::resource_table_t &resources, na_handle_t handle)
{
    capability::entry entry;
    if (!(resources.lookup_native(handle, entry) && entry.object))
        return false;
    if (entry.meta.binding == NA_BINDING_KERNEL_VIEW && entry.meta.scope == NA_SCOPE_STREAM &&
        protocol_uuid_matches(entry.meta.protocol_uuid, naos::system::Stream::protocol_uuid) &&
        entry.meta.revision == naos::system::Stream::revision &&
        (entry.object->get<dev::tty::console_stream>() != nullptr ||
         entry.object->get<dev::tty::klog_stream>() != nullptr))
        return true;
    return entry.meta.binding == NA_BINDING_CLIENT_END &&
           (entry.meta.scope == NA_SCOPE_TERMINAL_MASTER || entry.meta.scope == NA_SCOPE_TERMINAL_SLAVE) &&
           ((entry.meta.scope == NA_SCOPE_TERMINAL_MASTER &&
             protocol_uuid_matches(entry.meta.protocol_uuid, naos::system::TerminalMaster::protocol_uuid) &&
             entry.meta.revision == naos::system::TerminalMaster::revision) ||
            (entry.meta.scope == NA_SCOPE_TERMINAL_SLAVE &&
             protocol_uuid_matches(entry.meta.protocol_uuid, naos::system::TerminalSlave::protocol_uuid) &&
             entry.meta.revision == naos::system::TerminalSlave::revision));
}

bool valid_bootstrap_service_directory(task::resource_table_t &resources, na_handle_t handle)
{
    capability::entry entry;
    return resources.lookup_native(handle, entry) && entry.object && entry.meta.binding == NA_BINDING_KERNEL_VIEW &&
           entry.meta.scope == NA_SCOPE_SERVICE_DIRECTORY &&
           protocol_uuid_matches(entry.meta.protocol_uuid, naos::system::ServiceDirectory::protocol_uuid) &&
           entry.meta.revision == naos::system::ServiceDirectory::revision &&
           entry.object->get<service::directory>() != nullptr;
}

} // namespace

na_status_t handle_close(na_handle_t handle)
{
    const auto status = task::current_process()->resource.close_native(handle);
    if (status == NA_STATUS_OK)
        ipc::collect_orphaned_channels();
    return status;
}

na_status_t handle_duplicate(na_handle_t source, na_meta_rights_t rights, na_handle_t *result)
{
    if (!valid_output_handle(result))
        return NA_STATUS_FAULT;
    na_handle_t handle = NA_HANDLE_INVALID;
    const auto status = task::current_process()->resource.duplicate_native(source, rights, handle);
    if (status != NA_STATUS_OK)
        return status;
    const auto copy_status = write_handle(result, handle);
    if (copy_status != NA_STATUS_OK)
        task::current_process()->resource.close_native(handle);
    return copy_status;
}

na_status_t handle_restrict(na_handle_t source, const na_handle_restriction_t *restriction, na_handle_t *result)
{
    if (!valid_output_handle(result) || restriction == nullptr || !is_user_space_range(restriction, sizeof(u32)))
        return NA_STATUS_FAULT;
    na_handle_restriction_t values{};
    auto status = naos::usercopy::copy_versioned(values, restriction);
    if (status != NA_STATUS_OK)
        return status;
    const u64 restriction_bytes = values.struct_size < sizeof(values) ? values.struct_size : sizeof(values);
    if (naos::usercopy::ranges_overlap(reinterpret_cast<u64>(restriction), restriction_bytes,
                                       reinterpret_cast<u64>(result), sizeof(*result)))
        return NA_STATUS_INVALID_ARGUMENT;
    na_handle_t handle = NA_HANDLE_INVALID;
    capability::entry source_backup;
    status = task::current_process()->resource.restrict_native(source, values, handle, source_backup);
    if (status != NA_STATUS_OK)
        return status;
    const auto copy_status = write_handle(result, handle);
    if (copy_status != NA_STATUS_OK)
    {
        task::current_process()->resource.rollback_restrict(source, handle, source_backup);
        return copy_status;
    }
    status = task::current_process()->resource.commit_restrict(source, handle);
    if (status != NA_STATUS_OK)
        task::current_process()->resource.rollback_restrict(source, handle, source_backup);
    return status;
}

na_status_t channel_create(const na_channel_options_t *options, na_handle_t *left, na_handle_t *right)
{
    if (!valid_output_handle(left) || !valid_output_handle(right))
        return NA_STATUS_FAULT;
    khandle left_object;
    khandle right_object;
    auto status = ipc::create_raw_channel(left_object, right_object, options);
    if (status != NA_STATUS_OK)
        return status;
    if (options != nullptr && naos::usercopy::ranges_overlap(reinterpret_cast<u64>(options), sizeof(*options),
                                                             reinterpret_cast<u64>(left), sizeof(*left)))
    {
        left_object.reset();
        right_object.reset();
        ipc::collect_orphaned_channels();
        return NA_STATUS_INVALID_ARGUMENT;
    }
    if (options != nullptr && naos::usercopy::ranges_overlap(reinterpret_cast<u64>(options), sizeof(*options),
                                                             reinterpret_cast<u64>(right), sizeof(*right)))
    {
        left_object.reset();
        right_object.reset();
        ipc::collect_orphaned_channels();
        return NA_STATUS_INVALID_ARGUMENT;
    }
    if (naos::usercopy::ranges_overlap(reinterpret_cast<u64>(left), sizeof(*left), reinterpret_cast<u64>(right),
                                       sizeof(*right)))
    {
        left_object.reset();
        right_object.reset();
        ipc::collect_orphaned_channels();
        return NA_STATUS_INVALID_ARGUMENT;
    }

    auto &resources = task::current_process()->resource;
    freelibcxx::vector<na_handle_t> handles(memory::KernelCommonAllocatorV);
    status = resources.reserve_native(handles, 2);
    if (status != NA_STATUS_OK)
    {
        left_object.reset();
        right_object.reset();
        ipc::collect_orphaned_channels();
        return status;
    }

    capability::metadata meta;
    meta.binding = NA_BINDING_RAW_CHANNEL_END;
    meta.meta_rights = NA_RIGHT_TRANSFER | NA_RIGHT_WAIT;
    capability::transferred_resource left_resource(std::move(left_object), meta);
    capability::transferred_resource right_resource(std::move(right_object), meta);

    status = resources.activate_native(handles[0], std::move(left_resource));
    if (status == NA_STATUS_OK)
        status = resources.activate_native(handles[1], std::move(right_resource));
    if (status != NA_STATUS_OK)
    {
        resources.close_native(handles[0]);
        resources.rollback_native(handles);
        return status;
    }
    status = write_handle(left, handles[0]);
    if (status == NA_STATUS_OK)
        status = write_handle(right, handles[1]);
    if (status != NA_STATUS_OK)
    {
        resources.close_native(handles[0]);
        resources.close_native(handles[1]);
    }
    return status;
}

na_status_t channel_send(na_handle_t endpoint, const na_channel_send_frame_t *frame)
{
    return ipc::send_raw_channel(task::current_process()->resource, endpoint, frame);
}

na_status_t channel_receive(na_handle_t endpoint, na_channel_receive_frame_t *frame)
{
    capability::entry entry;
    if (!task::current_process()->resource.lookup_native(endpoint, entry) || !entry.object)
        return NA_STATUS_INVALID_HANDLE;
    if (entry.meta.binding == NA_BINDING_SERVER_END)
        return ipc::receive_protocol(task::current_process()->resource, endpoint, frame);
    if (entry.meta.binding == NA_BINDING_RAW_CHANNEL_END)
        return ipc::receive_raw_channel(task::current_process()->resource, endpoint, frame);
    return NA_STATUS_WRONG_BINDING;
}

na_status_t channel_discard(na_handle_t endpoint)
{
    return ipc::discard_raw_channel(task::current_process()->resource, endpoint);
}

na_status_t epoll_create(na_handle_t *result)
{
    if (!valid_output_handle(result))
        return NA_STATUS_FAULT;
    auto object = handle_t<ipc::epoll>::make();
    if (!object)
        return NA_STATUS_RESOURCE_EXHAUSTED;
    capability::metadata metadata;
    metadata.binding = NA_BINDING_EPOLL;
    metadata.meta_rights = NA_RIGHT_DUPLICATE | NA_RIGHT_TRANSFER | NA_RIGHT_WAIT | NA_RIGHT_INSPECT;
    const auto handle = task::current_process()->resource.install_native(std::move(object), metadata);
    if (handle == NA_HANDLE_INVALID)
        return NA_STATUS_RESOURCE_EXHAUSTED;
    const auto status = write_handle(result, handle);
    if (status != NA_STATUS_OK)
        task::current_process()->resource.close_native(handle);
    return status;
}

na_status_t epoll_ctl(na_handle_t epoll_handle, u32 operation, na_handle_t target,
                      const na_epoll_event_t *event)
{
    if (operation != NA_EPOLL_CTL_DEL && (event == nullptr || !is_user_space_range(event, sizeof(*event))))
        return NA_STATUS_FAULT;
    if (epoll_handle == NA_HANDLE_INVALID || target == NA_HANDLE_INVALID || epoll_handle == target)
        return NA_STATUS_INVALID_ARGUMENT;

    na_epoll_event_t values{};
    if (event != nullptr)
    {
        const auto status = naos::usercopy::copy_from(&values, reinterpret_cast<u64>(event), sizeof(values));
        if (status != NA_STATUS_OK)
            return status;
    }

    capability::entry entry;
    auto &resources = task::current_process()->resource;
    if (!resources.lookup_native(epoll_handle, entry) || !entry.object || entry.meta.binding != NA_BINDING_EPOLL)
        return NA_STATUS_WRONG_BINDING;
    auto *object = entry.object->get<ipc::epoll>();
    if (object == nullptr)
        return NA_STATUS_WRONG_BINDING;
    return object->control(resources, target, operation, event == nullptr ? nullptr : &values);
}

na_status_t epoll_wait(na_handle_t epoll_handle, na_epoll_event_t *events, u64 capacity, u64 *actual,
                       const timeclock::time *deadline)
{
    if (capacity == 0 || capacity > NA_CAPABILITY_MAX_PER_PROCESS || events == nullptr || actual == nullptr ||
        !is_user_space_range(events, capacity * sizeof(na_epoll_event_t)) || !valid_output_handle(actual))
        return NA_STATUS_FAULT;
    if (epoll_handle == NA_HANDLE_INVALID)
        return NA_STATUS_INVALID_HANDLE;
    if (naos::usercopy::ranges_overlap(reinterpret_cast<u64>(events), capacity * sizeof(na_epoll_event_t),
                                        reinterpret_cast<u64>(actual), sizeof(*actual)))
        return NA_STATUS_INVALID_ARGUMENT;

    timeclock::microsecond_t deadline_us = std::numeric_limits<timeclock::microsecond_t>::max();
    if (deadline != nullptr)
    {
        if (!is_user_space_range(deadline, sizeof(*deadline)))
            return NA_STATUS_FAULT;
        timeclock::time value(0, 0);
        if (naos::usercopy::copy_from(&value, reinterpret_cast<u64>(deadline), sizeof(value)) != NA_STATUS_OK)
            return NA_STATUS_FAULT;
        if (!timeclock::try_to_microseconds(value, deadline_us))
            return NA_STATUS_INVALID_ARGUMENT;
    }

    capability::entry entry;
    auto &resources = task::current_process()->resource;
    if (!resources.lookup_native(epoll_handle, entry) || !entry.object || entry.meta.binding != NA_BINDING_EPOLL)
        return NA_STATUS_WRONG_BINDING;
    auto *object = entry.object->get<ipc::epoll>();
    if (object == nullptr)
        return NA_STATUS_WRONG_BINDING;

    freelibcxx::vector<na_epoll_event_t> ready(memory::MemoryAllocatorV);
    ready.ensure(capacity);
    if (ready.data() == nullptr)
        return NA_STATUS_RESOURCE_EXHAUSTED;
    const auto status = object->wait(ready, deadline_us);
    u64 count = ready.size();
    if (naos::usercopy::copy_to(reinterpret_cast<u64>(actual), &count, sizeof(count)) != NA_STATUS_OK)
        return NA_STATUS_FAULT;
    if (!ready.empty() && naos::usercopy::copy_to(reinterpret_cast<u64>(events), ready.data(),
                                                   ready.size() * sizeof(na_epoll_event_t)) != NA_STATUS_OK)
        return NA_STATUS_FAULT;
    return status;
}

na_status_t handle_get_info(na_handle_t handle, na_handle_info_t *output)
{
    if (output == nullptr || !is_user_space_range(output, sizeof(*output)))
        return NA_STATUS_FAULT;
    capability::entry entry;
    auto &resources = task::current_process()->resource;
    if (!resources.lookup_native(handle, entry) || !entry.object)
        return NA_STATUS_INVALID_HANDLE;
    if ((entry.meta.meta_rights & NA_RIGHT_INSPECT) == 0)
        return NA_STATUS_ACCESS_DENIED;

    na_handle_info_t info{};
    info.struct_size = sizeof(info);
    info.binding = entry.meta.binding;
    info.scope = entry.meta.scope;
    info.revision = entry.meta.revision;
    info.features = entry.meta.features;
    info.meta_rights = entry.meta.meta_rights;
    info.protocol_rights = entry.meta.protocol_rights;
    info.signals = entry.object->capability_signals();
    info.generation = entry.generation;
    info.object_state = entry.object->capability_state();
    info.protocol_uuid = entry.meta.protocol_uuid;
    info.object_id = entry.object->object_id();
    info.view_offset = entry.meta.view_offset;
    info.view_length = entry.meta.view_length;
    return naos::usercopy::copy_to(reinterpret_cast<u64>(output), &info, sizeof(info));
}

na_status_t protocol_descriptor_create(const na_protocol_descriptor_t *input, na_handle_t *output)
{
    return ipc::create_protocol_descriptor(task::current_process()->resource, input, output);
}

na_status_t protocol_endpoint_create(na_handle_t descriptor, const na_protocol_endpoint_options_t *options,
                                     na_handle_t *client, na_handle_t *server)
{
    return ipc::create_protocol_endpoint(task::current_process()->resource, descriptor, options, client, server);
}

na_status_t invoke_submit(na_handle_t target, const na_submit_frame_t *frame, na_handle_t *invocation)
{
    return ipc::invoke_submit(task::current_process()->resource, target, frame, invocation, false);
}

na_status_t invoke_send_oneway(na_handle_t target, const na_submit_frame_t *frame)
{
    return ipc::invoke_submit(task::current_process()->resource, target, frame, nullptr, true);
}

na_status_t invocation_cancel(na_handle_t invocation)
{
    return ipc::invocation_cancel(task::current_process()->resource, invocation);
}

na_status_t invocation_take_result(na_handle_t invocation, na_result_frame_t *frame)
{
    return ipc::invocation_take_result(task::current_process()->resource, invocation, frame);
}

na_status_t responder_reply(na_handle_t responder, const na_reply_frame_t *frame)
{
    return ipc::responder_reply(task::current_process()->resource, responder, frame);
}

na_status_t responder_fail(na_handle_t responder, const na_fail_frame_t *frame)
{
    return ipc::responder_fail(task::current_process()->resource, responder, frame);
}

na_status_t bootstrap(na_bootstrap_frame_t *frame)
{
    if (frame == nullptr || !is_user_space_range(frame, sizeof(u32)))
        return NA_STATUS_FAULT;
    na_bootstrap_frame_t values{};
    auto status = naos::usercopy::copy_versioned(values, frame);
    if (status != NA_STATUS_OK)
        return status;
    if (values.struct_size < sizeof(values) || values.reserved0 != 0)
        return NA_STATUS_INVALID_ARGUMENT;

    auto *process = task::current_process();
    auto &resources = process->resource;

    if (values.flags == NA_BOOTSTRAP_FLAG_REBIND_CONSOLE)
    {
        if (!valid_bootstrap_stream(resources, values.stdin_stream) ||
            !valid_bootstrap_stream(resources, values.stdout_stream) ||
            !valid_bootstrap_stream(resources, values.stderr_stream))
            return NA_STATUS_INVALID_MESSAGE;
        process->console_in_handle = values.stdin_stream;
        process->console_out_handle = values.stdout_stream;
        process->console_err_handle = values.stderr_stream;
        return NA_STATUS_OK;
    }
    if (values.flags == NA_BOOTSTRAP_FLAG_EARLY_SERVICE)
    {
        return [&]() __attribute__((noinline)) -> na_status_t {
            if (process->bootstrap_consumed.exchange(true))
                return NA_STATUS_ALREADY_CONSUMED;

            // Early-service bootstrap (USERSPACE_FILESYSTEM_ADR §5.1.4/§5.5):
            // there is no kernel root/cwd runtime on this path, so those slots
            // stay absent and are reported as invalid handles.  The kernel only
            // hands out only the ServiceDirectory and diagnostic stdio streams.
            capability::entry stdin_entry;
            capability::entry stdout_entry;
            capability::entry stderr_entry;
            if (!resources.lookup_native(process->console_in_handle, stdin_entry) ||
                !resources.lookup_native(process->console_out_handle, stdout_entry) ||
                !resources.lookup_native(process->console_err_handle, stderr_entry) || !stdin_entry.object ||
                !stdout_entry.object || !stderr_entry.object)
            {
                return NA_STATUS_RESOURCE_EXHAUSTED;
            }
            khandle stdin_object = stdin_entry.object;
            khandle stdout_object = stdout_entry.object;
            khandle stderr_object = stderr_entry.object;

            auto service_object = service::get_global_service_directory();
            if (!service_object)
                service_object = handle_t<service::directory>::make();
            capability::metadata service_meta;
            service_meta.binding = NA_BINDING_KERNEL_VIEW;
            service_meta.protocol_uuid = naos::system::ServiceDirectory::protocol_uuid;
            service_meta.scope = NA_SCOPE_SERVICE_DIRECTORY;
            service_meta.revision = naos::system::ServiceDirectory::revision;
            service_meta.meta_rights = NA_RIGHT_DUPLICATE | NA_RIGHT_TRANSFER | NA_RIGHT_WAIT | NA_RIGHT_INSPECT;
            // Attenuated grant (§7): the early service may register endpoints
            // below naos://system/ and naos://service/ (SYSTEM_MANAGER) but
            // holds no ADMIN right.
            service_meta.protocol_rights = NA_PROTOCOL_RIGHT_INVOKE | NA_SERVICE_DIRECTORY_RIGHT_SYSTEM_MANAGER;
            const auto stdin_meta = bootstrap_stdio_metadata(stdin_entry.meta);
            const auto stdout_meta = bootstrap_stdio_metadata(stdout_entry.meta);
            const auto stderr_meta = bootstrap_stdio_metadata(stderr_entry.meta);

            const na_handle_t service_handle = resources.install_native(std::move(service_object), service_meta);
            const na_handle_t stdin_handle = resources.install_native(std::move(stdin_object), stdin_meta);
            const na_handle_t stdout_handle = resources.install_native(std::move(stdout_object), stdout_meta);
            const na_handle_t stderr_handle = resources.install_native(std::move(stderr_object), stderr_meta);
            if (service_handle == NA_HANDLE_INVALID || stdin_handle == NA_HANDLE_INVALID ||
                stdout_handle == NA_HANDLE_INVALID || stderr_handle == NA_HANDLE_INVALID)
            {
                resources.close_native(service_handle);
                resources.close_native(stdin_handle);
                resources.close_native(stdout_handle);
                resources.close_native(stderr_handle);
                return NA_STATUS_RESOURCE_EXHAUSTED;
            }

            values.root_directory = NA_HANDLE_INVALID;
            values.current_directory = NA_HANDLE_INVALID;
            values.service_directory = service_handle;
            values.stdin_stream = stdin_handle;
            values.stdout_stream = stdout_handle;
            values.stderr_stream = stderr_handle;
            KLOG_INFO("early-service bootstrap delivered to {} (service directory, stdio)",
                      (const char *)process->name);
            status = naos::usercopy::copy_to(reinterpret_cast<u64>(frame), &values, sizeof(values));
            if (status != NA_STATUS_OK)
            {
                resources.close_native(service_handle);
                resources.close_native(stdin_handle);
                resources.close_native(stdout_handle);
                resources.close_native(stderr_handle);
            }
            return status;
        }();
    }

    if (values.flags != 0)
        return NA_STATUS_INVALID_ARGUMENT;
    // In-place exec replaces the user image but retains its resource table.
    // The previous image has already consumed and closed its bootstrap
    // channel, so serve the namespace handles explicitly carried by the exec
    // syscall exactly once to the new runtime.
    if (process->exec_bootstrap_pending.load(std::memory_order_acquire))
    {
        const auto root_handle = process->exec_root_directory;
        const auto current_handle = process->exec_current_directory;
        const auto service_handle = process->exec_service_directory;
        if (!valid_bootstrap_directory(resources, root_handle) ||
            !valid_bootstrap_directory(resources, current_handle) ||
            !valid_bootstrap_service_directory(resources, service_handle) ||
            !valid_bootstrap_stream(resources, process->console_in_handle) ||
            !valid_bootstrap_stream(resources, process->console_out_handle) ||
            !valid_bootstrap_stream(resources, process->console_err_handle))
            return NA_STATUS_INVALID_MESSAGE;

        values.root_directory = root_handle;
        values.current_directory = current_handle;
        values.service_directory = service_handle;
        values.stdin_stream = process->console_in_handle;
        values.stdout_stream = process->console_out_handle;
        values.stderr_stream = process->console_err_handle;
        status = naos::usercopy::copy_to(reinterpret_cast<u64>(frame), &values, sizeof(values));
        if (status == NA_STATUS_OK)
            process->exec_bootstrap_pending.store(false, std::memory_order_release);
        return status;
    }
    if (process->bootstrap_channel_handle != NA_HANDLE_INVALID)
    {
        return [&]() __attribute__((noinline)) -> na_status_t {
            if (process->bootstrap_consumed.exchange(true))
                return NA_STATUS_ALREADY_CONSUMED;

            const auto endpoint = process->bootstrap_channel_handle;
            auto close_endpoint = [&] {
                if (process->bootstrap_channel_handle != NA_HANDLE_INVALID)
                {
                    resources.close_native(process->bootstrap_channel_handle);
                    process->bootstrap_channel_handle = NA_HANDLE_INVALID;
                }
            };

            auto *message_bytes = reinterpret_cast<byte *>(
                memory::MemoryAllocatorV->allocate(NA_CHANNEL_MAX_MESSAGE_BYTES, alignof(byte)));
            if (message_bytes == nullptr)
            {
                close_endpoint();
                return NA_STATUS_RESOURCE_EXHAUSTED;
            }
            freelibcxx::vector<na_handle_t> received(memory::KernelCommonAllocatorV);
            u64 actual_bytes = 0;
            for (;;)
            {
                status = ipc::receive_raw_channel_kernel(resources, endpoint, message_bytes,
                                                         NA_CHANNEL_MAX_MESSAGE_BYTES, actual_bytes, received);
                if (status != NA_STATUS_WOULD_BLOCK)
                    break;
                status = ipc::wait_for_raw_channel(resources, endpoint,
                                                   NA_SIGNAL_READABLE | NA_SIGNAL_PEER_CLOSED,
                                                   std::numeric_limits<u64>::max());
                if (status != NA_STATUS_OK)
                    break;
            }
            close_endpoint();
            if (status != NA_STATUS_OK)
            {
                close_received_handles(resources, received);
                memory::MemoryAllocatorV->deallocate(message_bytes);
                return status;
            }
            if (actual_bytes != sizeof(na_bootstrap_message_t))
            {
                close_received_handles(resources, received);
                memory::MemoryAllocatorV->deallocate(message_bytes);
                return NA_STATUS_INVALID_MESSAGE;
            }

            na_bootstrap_message_t message{};
            memcpy(&message, message_bytes, sizeof(message));
            memory::MemoryAllocatorV->deallocate(message_bytes);
            if (!naos::bootstrap::valid_message(message, received.size()))
            {
                close_received_handles(resources, received);
                return NA_STATUS_INVALID_MESSAGE;
            }

            if (message.flags == NA_BOOTSTRAP_FLAG_EARLY_SERVICE)
            {
                const auto service_handle = received[message.service_directory];
                const auto stdin_handle = received[message.stdin_stream];
                const auto stdout_handle = received[message.stdout_stream];
                const auto stderr_handle = received[message.stderr_stream];
                if (!valid_bootstrap_service_directory(resources, service_handle) ||
                    !valid_bootstrap_stream(resources, stdin_handle) ||
                    !valid_bootstrap_stream(resources, stdout_handle) ||
                    !valid_bootstrap_stream(resources, stderr_handle))
                {
                    close_received_handles(resources, received);
                    return NA_STATUS_INVALID_MESSAGE;
                }

                // Early-service children have no kernel root/cwd state.  The
                // transferred ServiceDirectory and stdio resources become
                // their bootstrap authorities; all other authorities use
                // ServiceDirectory discovery.
                process->console_in_handle = stdin_handle;
                process->console_out_handle = stdout_handle;
                process->console_err_handle = stderr_handle;

                values.root_directory = NA_HANDLE_INVALID;
                values.current_directory = NA_HANDLE_INVALID;
                values.flags = NA_BOOTSTRAP_FLAG_EARLY_SERVICE;
                values.service_directory = service_handle;
                values.stdin_stream = stdin_handle;
                values.stdout_stream = stdout_handle;
                values.stderr_stream = stderr_handle;
                status = naos::usercopy::copy_to(reinterpret_cast<u64>(frame), &values, sizeof(values));
                if (status != NA_STATUS_OK)
                    close_received_handles(resources, received);
                return status;
            }

            const auto root_handle = received[message.root_directory];
            const auto current_handle = received[message.current_directory];
            const auto service_handle = received[message.service_directory];
            const auto stdin_handle = received[message.stdin_stream];
            auto stdout_handle = received[message.stdout_stream];
            auto stderr_handle = received[message.stderr_stream];
            if (!valid_bootstrap_directory(resources, root_handle) ||
                !valid_bootstrap_directory(resources, current_handle) ||
                !valid_bootstrap_service_directory(resources, service_handle) ||
                !valid_bootstrap_stream(resources, stdin_handle) || !valid_bootstrap_stream(resources, stdout_handle) ||
                !valid_bootstrap_stream(resources, stderr_handle))
            {
                close_received_handles(resources, received);
                return NA_STATUS_INVALID_MESSAGE;
            }

            if (process->klog_stdio)
            {
                const auto original_stdout = stdout_handle;
                const auto original_stderr = stderr_handle;
                resources.close_native(original_stdout);
                if (original_stderr != original_stdout)
                    resources.close_native(original_stderr);

                auto stdout_object = handle_t<dev::tty::klog_stream>::make();
                auto stderr_object = handle_t<dev::tty::klog_stream>::make();
                if (!stdout_object || !stderr_object)
                {
                    close_received_handles(resources, received);
                    return NA_STATUS_RESOURCE_EXHAUSTED;
                }
                stdout_handle = resources.install_native(std::move(stdout_object), stream_metadata());
                stderr_handle = resources.install_native(std::move(stderr_object), stream_metadata());
                if (stdout_handle == NA_HANDLE_INVALID || stderr_handle == NA_HANDLE_INVALID)
                {
                    resources.close_native(stdout_handle);
                    resources.close_native(stderr_handle);
                    close_received_handles(resources, received);
                    return NA_STATUS_RESOURCE_EXHAUSTED;
                }
            }

            // Keep the process-owned console capabilities in sync with the
            // handles installed by the child bootstrap.
            process->console_in_handle = stdin_handle;
            process->console_out_handle = stdout_handle;
            process->console_err_handle = stderr_handle;

            values.root_directory = root_handle;
            values.current_directory = current_handle;
            values.service_directory = service_handle;
            values.stdin_stream = stdin_handle;
            values.stdout_stream = stdout_handle;
            values.stderr_stream = stderr_handle;
            status = naos::usercopy::copy_to(reinterpret_cast<u64>(frame), &values, sizeof(values));
            if (status != NA_STATUS_OK)
                close_received_handles(resources, received);
            return status;
        }();
    }

    // A normal bootstrap must arrive through the explicit channel created by
    // Process.spawn.  There is intentionally no implicit global_root fallback
    // for kernel-created processes: the namespace owner has to transfer real
    // Directory CLIENT_END capabilities.  Leave the transaction untouched so
    // the runtime can retry with the explicit EARLY_SERVICE flag for a
    // kernel-launched service (which has no channel).
    return process->bootstrap_consumed.load() ? NA_STATUS_ALREADY_CONSUMED : NA_STATUS_NOT_SUPPORTED;
}

BEGIN_SYSCALL
SYSCALL(NA_SYSCALL_HANDLE_CLOSE, handle_close)
SYSCALL(NA_SYSCALL_CHANNEL_CREATE, channel_create)
SYSCALL(NA_SYSCALL_CHANNEL_SEND, channel_send)
SYSCALL(NA_SYSCALL_CHANNEL_RECEIVE, channel_receive)
SYSCALL(NA_SYSCALL_CHANNEL_DISCARD, channel_discard)
SYSCALL(NA_SYSCALL_EPOLL_CREATE, epoll_create)
SYSCALL(NA_SYSCALL_EPOLL_CTL, epoll_ctl)
SYSCALL(NA_SYSCALL_EPOLL_WAIT, epoll_wait)
SYSCALL(NA_SYSCALL_HANDLE_DUPLICATE, handle_duplicate)
SYSCALL(NA_SYSCALL_HANDLE_RESTRICT, handle_restrict)
SYSCALL(NA_SYSCALL_HANDLE_GET_INFO, handle_get_info)
SYSCALL(NA_SYSCALL_PROTOCOL_DESCRIPTOR_CREATE, protocol_descriptor_create)
SYSCALL(NA_SYSCALL_PROTOCOL_ENDPOINT_CREATE, protocol_endpoint_create)
SYSCALL(NA_SYSCALL_INVOKE_SUBMIT, invoke_submit)
SYSCALL(NA_SYSCALL_INVOKE_SEND_ONEWAY, invoke_send_oneway)
SYSCALL(NA_SYSCALL_INVOCATION_CANCEL, invocation_cancel)
SYSCALL(NA_SYSCALL_INVOCATION_TAKE_RESULT, invocation_take_result)
SYSCALL(NA_SYSCALL_RESPONDER_REPLY, responder_reply)
SYSCALL(NA_SYSCALL_RESPONDER_FAIL, responder_fail)
SYSCALL(NA_SYSCALL_BOOTSTRAP, bootstrap)
END_SYSCALL
} // namespace naos::syscall
