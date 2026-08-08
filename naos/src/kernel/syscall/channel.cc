#include "kernel/ipc/channel.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/fs/vfs/file.hpp"
#include "kernel/fs/vfs/native_directory.hpp"
#include "kernel/fs/vfs/vfs.hpp"
#include "kernel/ipc/invocation.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/syscall.hpp"
#include "kernel/service_directory.hpp"
#include "kernel/task.hpp"
#include "kernel/time.hpp"
#include "kernel/usercopy.hpp"
#include "naos/bootstrap.hpp"
#include "naos/generated/system_uapi.h"
#include <limits>

namespace naos::syscall
{
namespace
{
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
    metadata.scope = NA_SCOPE_STREAM;
    metadata.revision = 1;
    metadata.meta_rights = NA_RIGHT_DUPLICATE | NA_RIGHT_TRANSFER | NA_RIGHT_WAIT | NA_RIGHT_INSPECT;
    metadata.protocol_rights = NA_PROTOCOL_RIGHT_INVOKE;
    return metadata;
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
    return resources.lookup_native(handle, entry) && entry.object && entry.meta.binding == NA_BINDING_KERNEL_VIEW &&
           entry.meta.scope == NA_SCOPE_DIRECTORY && entry.object->get<fs::vfs::native_directory>() != nullptr;
}

bool valid_bootstrap_stream(task::resource_table_t &resources, na_handle_t handle)
{
    capability::entry entry;
    return resources.lookup_native(handle, entry) && entry.object && entry.meta.binding == NA_BINDING_KERNEL_VIEW &&
           (entry.meta.scope == NA_SCOPE_STREAM || entry.meta.scope == NA_SCOPE_FILE) &&
           entry.object->get<fs::vfs::file>() != nullptr;
}

bool valid_bootstrap_service_directory(task::resource_table_t &resources, na_handle_t handle)
{
    capability::entry entry;
    return resources.lookup_native(handle, entry) && entry.object && entry.meta.binding == NA_BINDING_KERNEL_VIEW &&
           entry.meta.scope == NA_SCOPE_SERVICE_DIRECTORY && entry.object->get<service::directory>() != nullptr;
}
} // namespace

u64 handle_close(na_handle_t handle)
{
    const auto status = task::current_process()->resource.close_native(handle);
    if (status == NA_STATUS_OK)
        ipc::collect_orphaned_channels();
    return status;
}

u64 handle_duplicate(na_handle_t source, na_meta_rights_t rights, na_handle_t *result)
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

u64 handle_restrict(na_handle_t source, const na_handle_restriction_t *restriction, na_handle_t *result)
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

u64 channel_create(const na_channel_options_t *options, na_handle_t *left, na_handle_t *right)
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

u64 channel_send(na_handle_t endpoint, const na_channel_send_frame_t *frame)
{
    return ipc::send_raw_channel(task::current_process()->resource, endpoint, frame);
}

u64 channel_receive(na_handle_t endpoint, na_channel_receive_frame_t *frame)
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

u64 channel_discard(na_handle_t endpoint)
{
    return ipc::discard_raw_channel(task::current_process()->resource, endpoint);
}

u64 handle_wait_many(na_wait_item_t *items, u64 count, const timeclock::time *deadline)
{
    if (deadline == nullptr)
        return ipc::wait_many(task::current_process()->resource, items, count,
                              std::numeric_limits<timeclock::microsecond_t>::max());
    if (!is_user_space_range(deadline, sizeof(*deadline)))
        return NA_STATUS_FAULT;

    timeclock::time value(0, 0);
    if (naos::usercopy::copy_from(&value, reinterpret_cast<u64>(deadline), sizeof(value)) != NA_STATUS_OK)
        return NA_STATUS_FAULT;

    timeclock::microsecond_t deadline_us = 0;
    if (!timeclock::try_to_microseconds(value, deadline_us))
        return NA_STATUS_INVALID_ARGUMENT;
    return ipc::wait_many(task::current_process()->resource, items, count, deadline_us);
}

u64 handle_get_info(na_handle_t handle, na_handle_info_t *output)
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
    return naos::usercopy::copy_to(reinterpret_cast<u64>(output), &info, sizeof(info));
}

u64 protocol_descriptor_create(const na_protocol_descriptor_t *input, na_handle_t *output)
{
    return ipc::create_protocol_descriptor(task::current_process()->resource, input, output);
}

u64 protocol_endpoint_create(na_handle_t descriptor, const na_protocol_endpoint_options_t *options, na_handle_t *client,
                             na_handle_t *server)
{
    return ipc::create_protocol_endpoint(task::current_process()->resource, descriptor, options, client, server);
}

u64 invoke_submit(na_handle_t target, const na_submit_frame_t *frame, na_handle_t *invocation)
{
    return ipc::invoke_submit(task::current_process()->resource, target, frame, invocation, false);
}

u64 invoke_send_oneway(na_handle_t target, const na_submit_frame_t *frame)
{
    return ipc::invoke_submit(task::current_process()->resource, target, frame, nullptr, true);
}

u64 invocation_cancel(na_handle_t invocation)
{
    return ipc::invocation_cancel(task::current_process()->resource, invocation);
}

u64 invocation_take_result(na_handle_t invocation, na_result_frame_t *frame)
{
    return ipc::invocation_take_result(task::current_process()->resource, invocation, frame);
}

u64 responder_reply(na_handle_t responder, const na_reply_frame_t *frame)
{
    return ipc::responder_reply(task::current_process()->resource, responder, frame);
}

u64 responder_fail(na_handle_t responder, const na_fail_frame_t *frame)
{
    return ipc::responder_fail(task::current_process()->resource, responder, frame);
}

u64 bootstrap(na_bootstrap_frame_t *frame)
{
    if (frame == nullptr || !is_user_space_range(frame, sizeof(u32)))
        return NA_STATUS_FAULT;
    na_bootstrap_frame_t values{};
    auto status = naos::usercopy::copy_versioned(values, frame);
    if (status != NA_STATUS_OK)
        return status;
    if (values.struct_size < sizeof(values) || values.flags != 0 || values.reserved0 != 0 || values.reserved1 != 0)
        return NA_STATUS_INVALID_ARGUMENT;

    auto *process = task::current_process();
    auto &resources = process->resource;

    if (process->bootstrap_channel_handle != NA_HANDLE_INVALID)
    {
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

        auto *message_bytes =
            reinterpret_cast<byte *>(memory::MemoryAllocatorV->allocate(NA_CHANNEL_MAX_MESSAGE_BYTES, alignof(byte)));
        if (message_bytes == nullptr)
        {
            close_endpoint();
            return NA_STATUS_RESOURCE_EXHAUSTED;
        }
        freelibcxx::vector<na_handle_t> received(memory::KernelCommonAllocatorV);
        u64 actual_bytes = 0;
        for (;;)
        {
            status = ipc::receive_raw_channel_kernel(resources, endpoint, message_bytes, NA_CHANNEL_MAX_MESSAGE_BYTES,
                                                     actual_bytes, received);
            if (status != NA_STATUS_WOULD_BLOCK)
                break;
            status = ipc::wait_for_signal(resources, endpoint, NA_SIGNAL_READABLE | NA_SIGNAL_PEER_CLOSED,
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

        const auto root_handle = received[message.root_directory];
        const auto current_handle = received[message.current_directory];
        const auto service_handle = received[message.service_directory];
        const auto stdin_handle = received[message.stdin_stream];
        const auto stdout_handle = received[message.stdout_stream];
        const auto stderr_handle = received[message.stderr_stream];
        if (!valid_bootstrap_directory(resources, root_handle) ||
            !valid_bootstrap_directory(resources, current_handle) ||
            !valid_bootstrap_service_directory(resources, service_handle) ||
            !valid_bootstrap_stream(resources, stdin_handle) ||
            !valid_bootstrap_stream(resources, stdout_handle) || !valid_bootstrap_stream(resources, stderr_handle))
        {
            close_received_handles(resources, received);
            return NA_STATUS_INVALID_MESSAGE;
        }

        // Keep the process-owned console capabilities in sync with the
        // handles installed by the child bootstrap.  Forked children use
        // these capabilities to rebuild their userland bootstrap state.
        process->console_in_handle = stdin_handle;
        process->console_out_handle = stdout_handle;
        process->console_err_handle = stderr_handle;

        capability::entry root_entry;
        capability::entry current_entry;
        if (!resources.lookup_native(root_handle, root_entry) || !resources.lookup_native(current_handle, current_entry) ||
            !root_entry.object || !current_entry.object ||
            root_entry.object->get<fs::vfs::native_directory>() == nullptr ||
            current_entry.object->get<fs::vfs::native_directory>() == nullptr)
            return NA_STATUS_INVALID_MESSAGE;
        process->bootstrap_root_directory =
            handle_t<fs::vfs::native_directory>(root_entry.object.get_control());
        process->bootstrap_current_directory =
            handle_t<fs::vfs::native_directory>(current_entry.object.get_control());

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
    }

    if (process->bootstrap_consumed.exchange(true))
        return NA_STATUS_ALREADY_CONSUMED;

    const auto root = fs::vfs::global_root;
    if (root == nullptr)
        return NA_STATUS_RESOURCE_EXHAUSTED;
    const auto process_root = process->bootstrap_root_directory ? process->bootstrap_root_directory->root() : root;
    const auto process_current =
        process->bootstrap_current_directory ? process->bootstrap_current_directory->current() : process_root;

    auto root_object = process->bootstrap_root_directory
                           ? process->bootstrap_root_directory
                           : handle_t<fs::vfs::native_directory>::make(process_root, process_root);
    auto current_object = process->bootstrap_current_directory
                              ? process->bootstrap_current_directory
                              : handle_t<fs::vfs::native_directory>::make(process_root, process_current);
    auto service_object = handle_t<service::directory>::make();
    capability::entry stdin_entry;
    capability::entry stdout_entry;
    capability::entry stderr_entry;
    if (!resources.lookup_native(task::current_process()->console_in_handle, stdin_entry) ||
        !resources.lookup_native(task::current_process()->console_out_handle, stdout_entry) ||
        !resources.lookup_native(task::current_process()->console_err_handle, stderr_entry) || !stdin_entry.object ||
        !stdout_entry.object || !stderr_entry.object)
        return NA_STATUS_RESOURCE_EXHAUSTED;
    khandle stdin_object = stdin_entry.object;
    khandle stdout_object = stdout_entry.object;
    khandle stderr_object = stderr_entry.object;

    capability::metadata directory_meta;
    directory_meta.binding = NA_BINDING_KERNEL_VIEW;
    directory_meta.scope = NA_SCOPE_DIRECTORY;
    directory_meta.revision = 1;
    directory_meta.meta_rights = NA_RIGHT_DUPLICATE | NA_RIGHT_TRANSFER | NA_RIGHT_WAIT | NA_RIGHT_INSPECT;
    directory_meta.protocol_rights = NA_PROTOCOL_RIGHT_INVOKE;
    capability::metadata service_meta;
    service_meta.binding = NA_BINDING_KERNEL_VIEW;
    service_meta.scope = NA_SCOPE_SERVICE_DIRECTORY;
    service_meta.revision = 1;
    service_meta.meta_rights = NA_RIGHT_DUPLICATE | NA_RIGHT_TRANSFER | NA_RIGHT_WAIT | NA_RIGHT_INSPECT;
    service_meta.protocol_rights = NA_PROTOCOL_RIGHT_INVOKE;
    auto stdin_meta = stream_metadata();
    const na_handle_t root_handle = resources.install_native(std::move(root_object), directory_meta);
    const na_handle_t current_handle = resources.install_native(std::move(current_object), directory_meta);
    const na_handle_t service_handle = resources.install_native(std::move(service_object), service_meta);
    const na_handle_t stdin_handle = resources.install_native(std::move(stdin_object), stdin_meta);
    const na_handle_t stdout_handle = resources.install_native(std::move(stdout_object), stdin_meta);
    const na_handle_t stderr_handle = resources.install_native(std::move(stderr_object), stdin_meta);
    if (root_handle == NA_HANDLE_INVALID || current_handle == NA_HANDLE_INVALID ||
        service_handle == NA_HANDLE_INVALID || stdin_handle == NA_HANDLE_INVALID ||
        stdout_handle == NA_HANDLE_INVALID || stderr_handle == NA_HANDLE_INVALID)
    {
        resources.close_native(root_handle);
        resources.close_native(current_handle);
        resources.close_native(service_handle);
        resources.close_native(stdin_handle);
        resources.close_native(stdout_handle);
        resources.close_native(stderr_handle);
        return NA_STATUS_RESOURCE_EXHAUSTED;
    }
    values.root_directory = root_handle;
    values.current_directory = current_handle;
    values.service_directory = service_handle;
    values.stdin_stream = stdin_handle;
    values.stdout_stream = stdout_handle;
    values.stderr_stream = stderr_handle;
    status = naos::usercopy::copy_to(reinterpret_cast<u64>(frame), &values, sizeof(values));
    if (status != NA_STATUS_OK)
    {
        resources.close_native(root_handle);
        resources.close_native(current_handle);
        resources.close_native(service_handle);
        resources.close_native(stdin_handle);
        resources.close_native(stdout_handle);
        resources.close_native(stderr_handle);
    }
    return status;
}

u64 tty_control_acquire(na_handle_t stream, na_handle_t *output)
{
    if (!valid_output_handle(output))
        return NA_STATUS_FAULT;
    auto &resources = task::current_process()->resource;
    capability::entry source;
    if (!resources.lookup_native(stream, source) || !source.object)
        return NA_STATUS_INVALID_HANDLE;
    if (source.meta.binding != NA_BINDING_KERNEL_VIEW ||
        (source.meta.scope != NA_SCOPE_STREAM && source.meta.scope != NA_SCOPE_FILE))
        return NA_STATUS_WRONG_SCOPE;
    auto *file = source.object->get<fs::vfs::file>();
    if (file == nullptr || file->get_pseudo() == nullptr)
        return NA_STATUS_WRONG_BINDING;
    capability::metadata metadata = source.meta;
    metadata.scope = NA_SCOPE_TTY_CONTROL;
    metadata.meta_rights = capability::derive_tty_control_rights(source.meta.meta_rights);
    const auto handle = resources.install_native(source.object, metadata);
    if (handle == NA_HANDLE_INVALID)
        return NA_STATUS_RESOURCE_EXHAUSTED;
    const auto status = write_handle(output, handle);
    if (status != NA_STATUS_OK)
        resources.close_native(handle);
    return status;
}

BEGIN_SYSCALL
SYSCALL(NA_SYSCALL_HANDLE_CLOSE, handle_close)
SYSCALL(NA_SYSCALL_CHANNEL_CREATE, channel_create)
SYSCALL(NA_SYSCALL_CHANNEL_SEND, channel_send)
SYSCALL(NA_SYSCALL_CHANNEL_RECEIVE, channel_receive)
SYSCALL(NA_SYSCALL_CHANNEL_DISCARD, channel_discard)
SYSCALL(NA_SYSCALL_HANDLE_WAIT_MANY, handle_wait_many)
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
SYSCALL(NA_SYSCALL_TTY_CONTROL_ACQUIRE, tty_control_acquire)
END_SYSCALL
} // namespace naos::syscall
