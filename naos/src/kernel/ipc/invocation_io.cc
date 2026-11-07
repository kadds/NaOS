#include "invocation_io.hpp"
#include "invocation_region_support.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/dev/framebuffer.hpp"
#include "kernel/errno.hpp"
#include "kernel/input_event_source.hpp"
#include "kernel/ipc/channel.hpp"
#include "kernel/ipc/invocation.hpp"
#include "kernel/log.hpp"
#include "kernel/mm/data_plane.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/task.hpp"
#include "kernel/terminal.hpp"
#include "kernel/terminal_views.hpp"
#include "kernel/timer.hpp"
#include "kernel/ucontext.hpp"
#include "kernel/usercopy.hpp"
#include "naos/generated/system/Framebuffer.hpp"
#include "naos/generated/system/Framebuffer_kernel_dispatch.hpp"
#include "naos/generated/system/InputEventSource.hpp"
#include "naos/generated/system/InputEventSource_kernel_dispatch.hpp"
#include "naos/generated/system/MemoryObject.hpp"
#include "naos/generated/system/MemoryObject_kernel_dispatch.hpp"
#include "naos/generated/system/Stream.hpp"
#include "naos/generated/system/Stream_kernel_dispatch.hpp"
#include "naos/generated/system/TerminalDriverControl.hpp"
#include "naos/generated/system/TerminalDriverControl_kernel_dispatch.hpp"
#include "naos/generated/system/TerminalDriverFactory.hpp"
#include "naos/generated/system/TerminalDriverFactory_kernel_dispatch.hpp"
#include "naos/generated/system/TerminalJobControl.hpp"
#include "naos/generated/system/TerminalJobControl_kernel_dispatch.hpp"
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
using dispatch_detail::max_kernel_payload;
using dispatch_detail::request_region_object;

template <typename Layout> bool valid_iov_layout(const Layout &layout, u64 size)
{
    if (layout.segment_count > layout.lengths.size())
        return false;
    u64 total = 0;
    for (u64 i = 0; i < layout.segment_count; i++)
    {
        if (layout.lengths[i] > size || total > size - layout.lengths[i])
            return false;
        total += layout.lengths[i];
    }
    return total == size;
}

constexpr u64 region_copy_chunk = 4096;

/// Resolve the bulk region a migrated method names with `buffer`.  A region
/// travels as an argument resource of the invocation, so the decoded index
/// selects from this request's resource list.  The generated contract admits
/// exactly one duplicated MemoryObject; enforce binding, scope, disposition
/// and the memory rights the transfer direction needs before the object is
/// touched.  A violation is EINVAL for the caller.
na_status_t copy_object_span(data_plane::memory_object &source, u64 source_offset,
                             data_plane::memory_object &destination, u64 destination_offset, u64 size, u64 &actual)
{
    actual = 0;
    if (&source == &destination)
        return NA_STATUS_INVALID_ARGUMENT;
    byte bounce[region_copy_chunk];
    while (actual != size)
    {
        const u64 remaining = size - actual;
        const u64 chunk = remaining < region_copy_chunk ? remaining : region_copy_chunk;
        u64 read_bytes = 0;
        auto status = source.read(source_offset + actual, bounce, chunk, read_bytes);
        if (status != NA_STATUS_OK)
            return status;
        u64 written_bytes = 0;
        status = destination.write(destination_offset + actual, bounce, read_bytes, written_bytes);
        if (status != NA_STATUS_OK)
            return status;
        if (written_bytes != read_bytes)
            return NA_STATUS_IO_ERROR;
        actual += written_bytes;
    }
    return NA_STATUS_OK;
}

/// Pull bytes the stream produced into the caller's region.  A v-layout's
/// segment lengths are consecutive inside the region, so the transfer stays
/// contiguous there; `error` receives the stream's negative errno when it
/// fails mid-transfer, and the returned count is what was written.
template <typename Stream>
u64 fill_region_from_stream(Stream &stream, data_plane::memory_object &region, u64 region_offset, u64 size, i64 &error)
{
    u64 count = 0;
    byte bounce[region_copy_chunk];
    while (count < size)
    {
        const u64 remaining = size - count;
        const u64 chunk = remaining < region_copy_chunk ? remaining : region_copy_chunk;
        const auto produced = stream.read(bounce, chunk);
        if (produced < 0)
        {
            error = produced;
            break;
        }
        if (produced == 0)
            break;
        u64 written_bytes = 0;
        const auto status = region.write(region_offset + count, bounce, static_cast<u64>(produced), written_bytes);
        if (status != NA_STATUS_OK)
        {
            error = EINVAL;
            break;
        }
        count += written_bytes;
        if (written_bytes != static_cast<u64>(produced))
            break;
    }
    return count;
}

/// Send the bytes the caller placed in its region to the stream, returning
/// how many were consumed and the negative errno on failure.
i64 write_to_stream(dev::tty::console_stream &stream, const byte *data, u64 size, const char *process_name)
{
    (void)process_name;
    return stream.write(data, size);
}

i64 write_to_stream(dev::tty::klog_stream &stream, const byte *data, u64 size, const char *process_name)
{
    return stream.write(data, size, process_name);
}

template <typename Stream>
u64 drain_region_to_stream(Stream &stream, data_plane::memory_object &region, u64 region_offset, u64 size,
                           const char *process_name, i64 &error)
{
    u64 consumed = 0;
    byte bounce[region_copy_chunk];
    while (consumed < size)
    {
        const u64 remaining = size - consumed;
        const u64 chunk = remaining < region_copy_chunk ? remaining : region_copy_chunk;
        u64 read_bytes = 0;
        const auto status = region.read(region_offset + consumed, bounce, chunk, read_bytes);
        if (status != NA_STATUS_OK)
        {
            error = EINVAL;
            break;
        }
        const auto written = write_to_stream(stream, bounce, read_bytes, process_name);
        if (written < 0)
        {
            error = written;
            break;
        }
        consumed += static_cast<u64>(written);
        if (static_cast<u64>(written) != read_bytes)
            break;
    }
    return consumed;
}

class file_call_wait_registration
{
  public:
    explicit file_call_wait_registration(invocation_state &state)
        : state_(state)
    {
    }

    ~file_call_wait_registration()
    {
        if (queue_ != nullptr)
            state_.clear_execution_wait_queue(queue_);
    }

    bool interrupted() const { return state_.execution_interrupted(); }

    void register_queue(task::wait_queue_t *queue)
    {
        if (queue_ != nullptr && queue_ != queue)
            state_.clear_execution_wait_queue(queue_);
        queue_ = queue;
        if (queue_ != nullptr)
            state_.set_execution_wait_queue(queue_);
    }

  private:
    invocation_state &state_;
    task::wait_queue_t *queue_ = nullptr;
};

template <typename Stream> struct stream_kernel_handler
{
    invocation_state &state;
    Stream &stream;
    const char *process_name;

    na_status_t invalid_request()
    {
        return state.complete_failure(NA_EXECUTION_NOT_DELIVERED, NA_OUTCOME_REASON_PROTOCOL_VIOLATION)
                   ? NA_STATUS_OK
                   : NA_STATUS_INVALID_MESSAGE;
    }

    kernel_dispatch::method_result read(const naos::system::Stream::read_request &request,
                                        naos::system::Stream::read_response &response,
                                        capability::transfer_record_list &resources, capability::transfer_record_list &)
    {
        u64 region_offset = 0;
        auto *region =
            request_region_object(resources, request.buffer.value, NA_MEMORY_RIGHT_MAP | NA_MEMORY_RIGHT_WRITE, 0,
                                  request.size, region_offset);
        if (region == nullptr || !region->writable(region_offset, request.size))
            return kernel_dispatch::method_result::error(EINVAL);
        i64 error = 0;
        const u64 count = fill_region_from_stream(stream, *region, region_offset, request.size, error);
        response.count = error < 0 ? static_cast<u64>(-error) : count;
        return error == 0 ? kernel_dispatch::method_result::success()
                          : kernel_dispatch::method_result::error_reply(error);
    }

    kernel_dispatch::method_result write(const naos::system::Stream::write_request &request,
                                         naos::system::Stream::write_response &response,
                                         capability::transfer_record_list &resources,
                                         capability::transfer_record_list &)
    {
        u64 region_offset = 0;
        auto *region =
            request_region_object(resources, request.buffer.value, NA_MEMORY_RIGHT_MAP | NA_MEMORY_RIGHT_READ, 0,
                                  request.size, region_offset);
        if (region == nullptr || !region->readable(region_offset, request.size))
            return kernel_dispatch::method_result::error(EINVAL);
        i64 error = 0;
        const u64 consumed = drain_region_to_stream(stream, *region, region_offset, request.size, process_name, error);
        response.count = error < 0 ? static_cast<u64>(-error) : consumed;
        return error == 0 ? kernel_dispatch::method_result::success()
                          : kernel_dispatch::method_result::error_reply(error);
    }

    kernel_dispatch::method_result readv(const naos::system::Stream::readv_request &request,
                                         naos::system::Stream::readv_response &response,
                                         capability::transfer_record_list &resources,
                                         capability::transfer_record_list &)
    {
        if (!valid_iov_layout(request.layout, request.size))
            return kernel_dispatch::method_result::failure(NA_EXECUTION_NOT_DELIVERED,
                                                           NA_OUTCOME_REASON_PROTOCOL_VIOLATION);
        u64 region_offset = 0;
        auto *region =
            request_region_object(resources, request.buffer.value, NA_MEMORY_RIGHT_MAP | NA_MEMORY_RIGHT_WRITE, 0,
                                  request.size, region_offset);
        if (region == nullptr || !region->writable(region_offset, request.size))
            return kernel_dispatch::method_result::error(EINVAL);
        i64 error = 0;
        const u64 count = fill_region_from_stream(stream, *region, region_offset, request.size, error);
        response.count = error < 0 ? static_cast<u64>(-error) : count;
        return error == 0 ? kernel_dispatch::method_result::success()
                          : kernel_dispatch::method_result::error_reply(error);
    }

    kernel_dispatch::method_result writev(const naos::system::Stream::writev_request &request,
                                          naos::system::Stream::writev_response &response,
                                          capability::transfer_record_list &resources,
                                          capability::transfer_record_list &)
    {
        if (!valid_iov_layout(request.layout, request.size))
            return kernel_dispatch::method_result::failure(NA_EXECUTION_NOT_DELIVERED,
                                                           NA_OUTCOME_REASON_PROTOCOL_VIOLATION);
        u64 region_offset = 0;
        auto *region =
            request_region_object(resources, request.buffer.value, NA_MEMORY_RIGHT_MAP | NA_MEMORY_RIGHT_READ, 0,
                                  request.size, region_offset);
        if (region == nullptr || !region->readable(region_offset, request.size))
            return kernel_dispatch::method_result::error(EINVAL);
        i64 error = 0;
        const u64 consumed = drain_region_to_stream(stream, *region, region_offset, request.size, process_name, error);
        response.count = error < 0 ? static_cast<u64>(-error) : consumed;
        return error == 0 ? kernel_dispatch::method_result::success()
                          : kernel_dispatch::method_result::error_reply(error);
    }
};

template <typename Stream>
na_status_t publish_stream_call(invocation_state &state, Stream &stream, u64 method_id,
                                const freelibcxx::vector<byte> &request, capability::transfer_record_list &resources,
                                task::process_t *caller)
{
    state.mark_dispatched();
    const char *process_name = caller == nullptr || caller->name[0] == '\0' ? "process" : caller->name;
    stream_kernel_handler<Stream> handler{state, stream, process_name};
    return naos::system::Stream::StreamKernelDispatcher::dispatch(handler, state, method_id, request, resources);
}

struct memory_object_kernel_handler
{
    data_plane::memory_object &object;
    const capability::metadata &object_meta;

    na_status_t invalid_request() { return NA_STATUS_INVALID_MESSAGE; }

    bool resolve_object_offset(u64 offset, u64 length, u64 &absolute) const
    {
        if (length == 0 || offset > object_meta.view_length || length > object_meta.view_length - offset ||
            object_meta.view_offset > ~u64(0) - offset)
            return false;
        absolute = object_meta.view_offset + offset;
        return true;
    }

    kernel_dispatch::method_result get_info(const naos::system::MemoryObject::get_info_request &,
                                            naos::system::MemoryObject::get_info_response &response,
                                            const capability::transfer_record_list &,
                                            capability::transfer_record_list &)
    {
        response.flags = object.flags();
        response.size = object_meta.view_length;
        response.max_size = NA_MEMORY_OBJECT_MAX_BYTES;
        return kernel_dispatch::method_result::success();
    }

    kernel_dispatch::method_result read(const naos::system::MemoryObject::read_request &request,
                                        naos::system::MemoryObject::read_response &response,
                                        capability::transfer_record_list &resources, capability::transfer_record_list &)
    {
        u64 region_offset = 0;
        auto *region =
            request_region_object(resources, request.buffer.value, NA_MEMORY_RIGHT_MAP | NA_MEMORY_RIGHT_WRITE, 0,
                                  request.size, region_offset);
        u64 object_offset = 0;
        if (region == nullptr || !region->writable(region_offset, request.size) ||
            !resolve_object_offset(request.offset, request.size, object_offset) ||
            !object.readable(object_offset, request.size))
            return kernel_dispatch::method_result::error(EINVAL);

        u64 actual = 0;
        if (copy_object_span(object, object_offset, *region, region_offset, request.size, actual) != NA_STATUS_OK)
            return kernel_dispatch::method_result::error(EINVAL);
        response.count = actual;
        return kernel_dispatch::method_result::success();
    }

    kernel_dispatch::method_result write(const naos::system::MemoryObject::write_request &request,
                                         naos::system::MemoryObject::write_response &response,
                                         capability::transfer_record_list &resources,
                                         capability::transfer_record_list &)
    {
        u64 region_offset = 0;
        auto *region =
            request_region_object(resources, request.buffer.value, NA_MEMORY_RIGHT_MAP | NA_MEMORY_RIGHT_READ, 0,
                                  request.size, region_offset);
        u64 object_offset = 0;
        if (region == nullptr || !region->readable(region_offset, request.size) ||
            !resolve_object_offset(request.offset, request.size, object_offset))
            return kernel_dispatch::method_result::error(EINVAL);
        if (!object.writable(object_offset, request.size))
            return kernel_dispatch::method_result::error((object.flags() & NA_MEMORY_FLAG_READ_ONLY) != 0 ? EACCES
                                                                                                          : EINVAL);

        u64 actual = 0;
        const auto status = copy_object_span(*region, region_offset, object, object_offset, request.size, actual);
        if (status != NA_STATUS_OK)
            return kernel_dispatch::method_result::error(status == NA_STATUS_ACCESS_DENIED ? EACCES : EINVAL);
        response.count = actual;
        return kernel_dispatch::method_result::success();
    }
};

na_status_t publish_memory_object_call(invocation_state &state, data_plane::memory_object &memory_object,
                                       const capability::metadata &object_meta, u64 method_id,
                                       const freelibcxx::vector<byte> &request,
                                       capability::transfer_record_list &resources)
{
    memory_object_kernel_handler handler{memory_object, object_meta};
    return naos::system::MemoryObject::MemoryObjectKernelDispatcher::dispatch(handler, state, method_id, request,
                                                                              resources);
}

struct framebuffer_kernel_handler
{
    dev::framebuffer::framebuffer_service &framebuffer;
    invocation_state &state;

    na_status_t invalid_request()
    {
        return state.complete_reply(empty_bytes(), empty_resources(), EINVAL) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
    }

    kernel_dispatch::method_result get(const naos::system::Framebuffer::get_request &,
                                       naos::system::Framebuffer::get_response &response,
                                       const capability::transfer_record_list &,
                                       capability::transfer_record_list &response_resources)
    {
        if (!term::try_acquire_framebuffer_user_writer())
            return kernel_dispatch::method_result::error(EBUSY);

        auto object = handle_t<data_plane::memory_object>::make(
            framebuffer.physical_base(), static_cast<byte *>(framebuffer.kernel_view()), framebuffer.frame_bytes(),
            &term::release_framebuffer_user_writer);
        if (!object)
        {
            term::release_framebuffer_user_writer();
            return kernel_dispatch::method_result::error(ENOMEM);
        }

        auto object_meta =
            kernel_view_metadata(NA_SCOPE_MEMORY_OBJECT, naos::system::MemoryObject::protocol_uuid,
                                 NA_RIGHT_TRANSFER | NA_RIGHT_WAIT | NA_RIGHT_INSPECT,
                                 NA_MEMORY_RIGHT_READ | NA_MEMORY_RIGHT_WRITE | NA_MEMORY_RIGHT_MAP,
                                 naos::system::MemoryObject::revision, naos::system::MemoryObject::features);
        object_meta.binding = NA_BINDING_MEMORY_OBJECT;
        object_meta.view_offset = 0;
        object_meta.view_length = framebuffer.frame_bytes();

        response.info.width = framebuffer.width();
        response.info.height = framebuffer.height();
        response.info.pitch = framebuffer.pitch();
        response.info.bpp = framebuffer.bpp();
        response.info.type = framebuffer.type();
        response.info.smem_bytes = framebuffer.frame_bytes();
        response.framebuffer.value = 0;

        response_resources.push_back(capability::transfer_record(
            NA_HANDLE_INVALID, true, capability::transferred_resource(std::move(object), object_meta)));
        return kernel_dispatch::method_result::success();
    }
};

na_status_t publish_framebuffer_call(invocation_state &state, dev::framebuffer::framebuffer_service &framebuffer,
                                     u64 method_id, const freelibcxx::vector<byte> &wire)
{
    capability::transfer_record_list resources(memory::KernelCommonAllocatorV);
    framebuffer_kernel_handler handler{framebuffer, state};
    return naos::system::Framebuffer::FramebufferKernelDispatcher::dispatch(handler, state, method_id, wire, resources);
}

struct input_event_source_kernel_handler
{
    dev::input::input_event_source &source;
    invocation_state &state;
    kobject *subscription_object = nullptr;

    na_status_t invalid_request()
    {
        return state.complete_reply(empty_bytes(), empty_resources(), EINVAL) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
    }

    static void finish_subscription(void *context, bool committed)
    {
        auto &handler = *static_cast<input_event_source_kernel_handler *>(context);
        if (!committed && handler.subscription_object != nullptr)
            (void)handler.source.rollback_subscription(handler.subscription_object);
    }

    kernel_dispatch::method_result subscribe(const naos::system::InputEventSource::subscribe_request &request,
                                             naos::system::InputEventSource::subscribe_response &response,
                                             const capability::transfer_record_list &,
                                             capability::transfer_record_list &response_resources)
    {
        khandle receiver;
        if (!source.subscribe(receiver, request.max_events))
            return kernel_dispatch::method_result::error(EAGAIN);

        subscription_object = receiver.operator&();
        auto meta = kernel_view_metadata(NA_SCOPE_INPUT_EVENT_SOURCE, naos::system::InputEventSource::protocol_uuid,
                                         NA_RIGHT_TRANSFER | NA_RIGHT_WAIT | NA_RIGHT_INSPECT,
                                         NA_RIGHT_TRANSFER | NA_RIGHT_WAIT | NA_RIGHT_INSPECT);
        meta.binding = NA_BINDING_RAW_CHANNEL_END;
        meta.scope = 0;

        response.receiver.value = 0;
        response_resources.push_back(
            capability::transfer_record(NA_HANDLE_INVALID, true, capability::transferred_resource(receiver, meta)));
        return kernel_dispatch::method_result::success_with_completion(this, finish_subscription);
    }
};

na_status_t publish_input_event_source_call(invocation_state &state, dev::input::input_event_source &source,
                                            u64 method_id, const freelibcxx::vector<byte> &request)
{
    capability::transfer_record_list resources(memory::KernelCommonAllocatorV);
    input_event_source_kernel_handler handler{source, state};
    return naos::system::InputEventSource::InputEventSourceKernelDispatcher::dispatch(handler, state, method_id,
                                                                                      request, resources);
}

struct terminal_job_control_kernel_handler
{
    dev::tty::terminal_job_control &view;
    task::process_t *process;

    na_status_t invalid_request() { return NA_STATUS_INVALID_MESSAGE; }

    kernel_dispatch::method_result check_io(const naos::system::TerminalJobControl::check_io_request &request,
                                            naos::system::TerminalJobControl::check_io_response &,
                                            const capability::transfer_record_list &,
                                            capability::transfer_record_list &)
    {
        const auto result =
            task::check_terminal_job_control(process, view.identity(), request.direction == 1, request.tostop != 0);
        return result == 0 ? kernel_dispatch::method_result::success() : kernel_dispatch::method_result::error(result);
    }

    kernel_dispatch::method_result attach(const naos::system::TerminalJobControl::attach_request &request,
                                          naos::system::TerminalJobControl::attach_response &,
                                          const capability::transfer_record_list &, capability::transfer_record_list &)
    {
        if (request.force != 0 && !process->resource.has_native_object_type(kobject::type_e::terminal_driver_factory))
            return kernel_dispatch::method_result::error(EACCES);
        const auto result = task::attach_controlling_terminal(process, view.identity_handle(), request.force != 0);
        return result == 0 ? kernel_dispatch::method_result::success() : kernel_dispatch::method_result::error(result);
    }

    kernel_dispatch::method_result detach(const naos::system::TerminalJobControl::detach_request &,
                                          naos::system::TerminalJobControl::detach_response &,
                                          const capability::transfer_record_list &, capability::transfer_record_list &)
    {
        task::detach_controlling_terminal(process);
        return kernel_dispatch::method_result::success();
    }

    kernel_dispatch::method_result get_pgrp(const naos::system::TerminalJobControl::get_pgrp_request &,
                                            naos::system::TerminalJobControl::get_pgrp_response &response,
                                            const capability::transfer_record_list &,
                                            capability::transfer_record_list &)
    {
        const auto group = task::get_foreground_process_group(view.identity());
        if (group < 0)
            return kernel_dispatch::method_result::error(static_cast<i64>(-group));
        response.group = static_cast<u64>(group);
        return kernel_dispatch::method_result::success();
    }

    kernel_dispatch::method_result set_pgrp(const naos::system::TerminalJobControl::set_pgrp_request &request,
                                            naos::system::TerminalJobControl::set_pgrp_response &,
                                            const capability::transfer_record_list &,
                                            capability::transfer_record_list &)
    {
        const auto result =
            task::set_foreground_process_group(process, view.identity(), static_cast<group_id>(request.group));
        return result == 0 ? kernel_dispatch::method_result::success() : kernel_dispatch::method_result::error(result);
    }

    kernel_dispatch::method_result get_sid(const naos::system::TerminalJobControl::get_sid_request &,
                                           naos::system::TerminalJobControl::get_sid_response &response,
                                           const capability::transfer_record_list &, capability::transfer_record_list &)
    {
        response.session = view.identity()->session_id();
        return kernel_dispatch::method_result::success();
    }

    kernel_dispatch::method_result query(const naos::system::TerminalJobControl::query_request &,
                                         naos::system::TerminalJobControl::query_response &response,
                                         const capability::transfer_record_list &, capability::transfer_record_list &)
    {
        auto *identity = view.identity();
        response.state.session = identity->session_id();
        response.state.process_group = process->process_group_id;
        response.state.foreground_process_group = identity->foreground_process_group();
        response.state.has_controlling_terminal = task::get_controlling_terminal(process) == identity ? 1 : 0;
        response.state.generation = identity->generation();
        return kernel_dispatch::method_result::success();
    }
};

na_status_t publish_terminal_job_control_call(invocation_state &state, dev::tty::terminal_job_control &view,
                                              u64 method_id, const freelibcxx::vector<byte> &request,
                                              task::process_t *caller)
{
    auto *identity = view.identity();
    if (identity == nullptr || !identity->live() || caller == nullptr)
        return NA_STATUS_OBJECT_REVOKED;

    capability::transfer_record_list resources(memory::KernelCommonAllocatorV);
    terminal_job_control_kernel_handler handler{view, caller};
    return naos::system::TerminalJobControl::TerminalJobControlKernelDispatcher::dispatch(handler, state, method_id,
                                                                                          request, resources);
}

struct terminal_driver_control_kernel_handler
{
    dev::tty::terminal_driver_control &view;

    na_status_t invalid_request() { return NA_STATUS_INVALID_MESSAGE; }

    kernel_dispatch::method_result
    raise_foreground(const naos::system::TerminalDriverControl::raise_foreground_request &request,
                     naos::system::TerminalDriverControl::raise_foreground_response &,
                     const capability::transfer_record_list &, capability::transfer_record_list &)
    {
        task::signal_num_t signal_number = task::signal::sigint;
        if (request.action == naos::system::TerminalDriverControl::DriverAction::quit)
            signal_number = task::signal::sigquit;
        else if (request.action == naos::system::TerminalDriverControl::DriverAction::suspend)
            signal_number = task::signal::sigtstp;
        group_id foreground = 0;
        if (!view.identity()->foreground_if_live(foreground))
            return kernel_dispatch::method_result::error(EIO);
        const auto signal_result =
            foreground > 0 ? task::send_signal_to_process_group(static_cast<group_id>(foreground), signal_number)
                           : ENOENT;
        return signal_result < 0 ? kernel_dispatch::method_result::error(signal_result)
                                 : kernel_dispatch::method_result::success();
    }

    kernel_dispatch::method_result
    notify_winsize_changed(const naos::system::TerminalDriverControl::notify_winsize_changed_request &,
                           naos::system::TerminalDriverControl::notify_winsize_changed_response &,
                           const capability::transfer_record_list &, capability::transfer_record_list &)
    {
        group_id foreground = 0;
        if (!view.identity()->foreground_if_live(foreground))
            // Resizing still succeeds when there is no foreground process;
            // there is simply nobody to notify.
            return kernel_dispatch::method_result::success();
        const auto signal_result =
            foreground > 0
                ? task::send_signal_to_process_group(static_cast<group_id>(foreground), task::signal::sigwinch)
                : ENOENT;
        const i64 notification_error = signal_result == ENOENT ? 0 : (signal_result < 0 ? signal_result : 0);
        return notification_error == 0 ? kernel_dispatch::method_result::success()
                                       : kernel_dispatch::method_result::error(notification_error);
    }

    kernel_dispatch::method_result hangup(const naos::system::TerminalDriverControl::hangup_request &,
                                          naos::system::TerminalDriverControl::hangup_response &,
                                          const capability::transfer_record_list &, capability::transfer_record_list &)
    {
        return revoke_foreground();
    }

    kernel_dispatch::method_result revoke(const naos::system::TerminalDriverControl::revoke_request &,
                                          naos::system::TerminalDriverControl::revoke_response &,
                                          const capability::transfer_record_list &, capability::transfer_record_list &)
    {
        return revoke_foreground();
    }

  private:
    kernel_dispatch::method_result revoke_foreground()
    {
        auto *identity = view.identity();
        group_id foreground = 0;
        if (identity->revoke_and_take_foreground(&foreground))
        {
            i64 signal_error = 0;
            if (foreground > 0)
            {
                const auto hangup =
                    task::send_signal_to_process_group(static_cast<group_id>(foreground), task::signal::sighup);
                const auto continue_result =
                    task::send_signal_to_process_group(static_cast<group_id>(foreground), task::signal::sigcont);
                if (hangup < 0)
                    signal_error = hangup;
                else if (continue_result < 0)
                    signal_error = continue_result;
            }
            task::detach_session_terminal(identity);
            return signal_error == 0 ? kernel_dispatch::method_result::success()
                                     : kernel_dispatch::method_result::error(signal_error);
        }
        return kernel_dispatch::method_result::success();
    }
};

na_status_t publish_terminal_driver_control_call(invocation_state &state, dev::tty::terminal_driver_control &view,
                                                 u64 method_id, const freelibcxx::vector<byte> &request)
{
    auto *identity = view.identity();
    if (identity == nullptr || !identity->live())
        return NA_STATUS_OBJECT_REVOKED;

    capability::transfer_record_list resources(memory::KernelCommonAllocatorV);
    terminal_driver_control_kernel_handler handler{view};
    return naos::system::TerminalDriverControl::TerminalDriverControlKernelDispatcher::dispatch(
        handler, state, method_id, request, resources);
}

struct terminal_driver_factory_kernel_handler
{
    dev::tty::terminal_driver_factory &factory;
    u64 rollback_terminal_id = 0;
    u64 rollback_generation = 0;

    na_status_t invalid_request() { return NA_STATUS_INVALID_MESSAGE; }

    static void finish_create(void *context, bool committed)
    {
        auto &handler = *static_cast<terminal_driver_factory_kernel_handler *>(context);
        if (!committed && handler.rollback_terminal_id != 0)
            handler.factory.rollback(handler.rollback_terminal_id, handler.rollback_generation);
    }

    kernel_dispatch::method_result create(const naos::system::TerminalDriverFactory::create_request &request,
                                          naos::system::TerminalDriverFactory::create_response &response,
                                          const capability::transfer_record_list &,
                                          capability::transfer_record_list &response_resources)
    {
        khandle identity;
        khandle job_control;
        khandle driver_control;
        if (!factory.create(request.mode, identity, job_control, driver_control))
            return kernel_dispatch::method_result::error(EAGAIN);

        auto driver_meta =
            kernel_view_metadata(NA_SCOPE_TERMINAL_DRIVER_CONTROL, naos::system::TerminalDriverControl::protocol_uuid,
                                 NA_RIGHT_DUPLICATE | NA_RIGHT_TRANSFER | NA_RIGHT_WAIT | NA_RIGHT_INSPECT,
                                 NA_RIGHT_TRANSFER | NA_RIGHT_WAIT | NA_RIGHT_INSPECT);
        auto job_meta = kernel_view_metadata(
            NA_SCOPE_TERMINAL_JOB_CONTROL, naos::system::TerminalJobControl::protocol_uuid,
            NA_RIGHT_DUPLICATE | NA_RIGHT_TRANSFER | NA_RIGHT_WAIT | NA_RIGHT_INSPECT,
            NA_RIGHT_TRANSFER | NA_RIGHT_WAIT | NA_RIGHT_INSPECT, naos::system::TerminalJobControl::revision,
            naos::system::TerminalJobControl::features);
        auto *typed_identity = identity.as<dev::tty::terminal_identity>().operator&();
        if (typed_identity == nullptr)
        {
            factory.rollback(0, 0);
            return kernel_dispatch::method_result::error(EIO);
        }
        response.locator.terminal_id = typed_identity->id();
        response.locator.generation = typed_identity->generation();
        response.locator.token = typed_identity->token();
        response.driver.value = 0;
        response.job_control.value = 1;
        rollback_terminal_id = response.locator.terminal_id;
        rollback_generation = response.locator.generation;

        response_resources.push_back(capability::transfer_record(
            NA_HANDLE_INVALID, true, capability::transferred_resource(std::move(driver_control), driver_meta)));
        response_resources.push_back(capability::transfer_record(
            NA_HANDLE_INVALID, true, capability::transferred_resource(std::move(job_control), job_meta)));
        return kernel_dispatch::method_result::success_with_completion(this, finish_create);
    }

    kernel_dispatch::method_result
    validate_locator(const naos::system::TerminalDriverFactory::validate_locator_request &request,
                     naos::system::TerminalDriverFactory::validate_locator_response &response,
                     const capability::transfer_record_list &, capability::transfer_record_list &)
    {
        response.valid = factory.validate_locator(request.locator.terminal_id, request.locator.generation,
                                                  request.locator.token.data(), request.locator.token.size())
                             ? 1
                             : 0;
        return kernel_dispatch::method_result::success();
    }
};

na_status_t publish_terminal_driver_factory_call(invocation_state &state, dev::tty::terminal_driver_factory &factory,
                                                 u64 method_id, const freelibcxx::vector<byte> &request)
{
    capability::transfer_record_list resources(memory::KernelCommonAllocatorV);
    terminal_driver_factory_kernel_handler handler{factory};
    return naos::system::TerminalDriverFactory::TerminalDriverFactoryKernelDispatcher::dispatch(
        handler, state, method_id, request, resources);
}

} // namespace

namespace
{
na_status_t dispatch_console_stream(kernel_dispatch::kernel_view_context &context, dev::tty::console_stream &object)
{
    return publish_stream_call(context.state, object, context.method_id, context.request, context.resources,
                               context.caller);
}

na_status_t dispatch_klog_stream(kernel_dispatch::kernel_view_context &context, dev::tty::klog_stream &object)
{
    return publish_stream_call(context.state, object, context.method_id, context.request, context.resources,
                               context.caller);
}

na_status_t dispatch_memory_object(kernel_dispatch::kernel_view_context &context, data_plane::memory_object &object)
{
    auto &target = context.target;
    if (context.method_id == NA_METHOD_MEMORY_OBJECT_GET_INFO && (target.meta.meta_rights & NA_RIGHT_INSPECT) == 0)
        return NA_STATUS_ACCESS_DENIED;
    if ((context.method_id == NA_METHOD_MEMORY_OBJECT_READ || context.method_id == NA_METHOD_MEMORY_OBJECT_WRITE) &&
        !data_plane::memory_object_scalar_ipc_is_allowed(object.flags()))
        return NA_STATUS_ACCESS_DENIED;
    return publish_memory_object_call(context.state, object, target.meta, context.method_id, context.request,
                                      context.resources);
}

na_status_t dispatch_framebuffer(kernel_dispatch::kernel_view_context &context,
                                 dev::framebuffer::framebuffer_service &object)
{
    return publish_framebuffer_call(context.state, object, context.method_id, context.request);
}

na_status_t dispatch_input_event_source(kernel_dispatch::kernel_view_context &context,
                                        dev::input::input_event_source &object)
{
    return publish_input_event_source_call(context.state, object, context.method_id, context.request);
}

na_status_t dispatch_terminal_job_control(kernel_dispatch::kernel_view_context &context,
                                          dev::tty::terminal_job_control &object)
{
    return publish_terminal_job_control_call(context.state, object, context.method_id, context.request, context.caller);
}

na_status_t dispatch_terminal_driver_control(kernel_dispatch::kernel_view_context &context,
                                             dev::tty::terminal_driver_control &object)
{
    return publish_terminal_driver_control_call(context.state, object, context.method_id, context.request);
}

na_status_t dispatch_terminal_driver_factory(kernel_dispatch::kernel_view_context &context,
                                             dev::tty::terminal_driver_factory &object)
{
    return publish_terminal_driver_factory_call(context.state, object, context.method_id, context.request);
}

constexpr kernel_dispatch::kernel_view_route io_kernel_view_route_entries[] = {
    kernel_dispatch::make_kernel_view_route<dev::tty::console_stream, dispatch_console_stream>(
        NA_SCOPE_STREAM, naos::system::Stream::descriptor),
    kernel_dispatch::make_kernel_view_route<dev::tty::klog_stream, dispatch_klog_stream>(
        NA_SCOPE_STREAM, naos::system::Stream::descriptor),
    kernel_dispatch::make_kernel_view_route<data_plane::memory_object, dispatch_memory_object>(
        NA_SCOPE_MEMORY_OBJECT, naos::system::MemoryObject::descriptor),
    kernel_dispatch::make_kernel_view_route<dev::framebuffer::framebuffer_service, dispatch_framebuffer>(
        NA_SCOPE_FRAMEBUFFER, naos::system::Framebuffer::descriptor),
    kernel_dispatch::make_kernel_view_route<dev::input::input_event_source, dispatch_input_event_source>(
        NA_SCOPE_INPUT_EVENT_SOURCE, naos::system::InputEventSource::descriptor),
    kernel_dispatch::make_kernel_view_route<dev::tty::terminal_job_control, dispatch_terminal_job_control>(
        NA_SCOPE_TERMINAL_JOB_CONTROL, naos::system::TerminalJobControl::descriptor),
    kernel_dispatch::make_kernel_view_route<dev::tty::terminal_driver_control, dispatch_terminal_driver_control>(
        NA_SCOPE_TERMINAL_DRIVER_CONTROL, naos::system::TerminalDriverControl::descriptor),
    kernel_dispatch::make_kernel_view_route<dev::tty::terminal_driver_factory, dispatch_terminal_driver_factory>(
        NA_SCOPE_TERMINAL_DRIVER_FACTORY, naos::system::TerminalDriverFactory::descriptor),
};
} // namespace

kernel_dispatch::kernel_view_route_table io_kernel_view_routes() noexcept
{
    return {io_kernel_view_route_entries,
            sizeof(io_kernel_view_route_entries) / sizeof(io_kernel_view_route_entries[0])};
}
} // namespace naos::ipc
