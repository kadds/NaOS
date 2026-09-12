#include "kernel/ipc/invocation.hpp"
#include "kernel/ipc/channel.hpp"

#include "kernel/arch/klib.hpp"
#include "kernel/dev/framebuffer.hpp"
#include "kernel/errno.hpp"
#include "kernel/input_event_source.hpp"
#include "kernel/log.hpp"
#include "kernel/mm/data_plane.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/service_directory.hpp"
#include "kernel/task.hpp"
#include "kernel/terminal.hpp"
#include "kernel/terminal_views.hpp"
#include "kernel/timer.hpp"
#include "kernel/ucontext.hpp"
#include "kernel/usercopy.hpp"
#include "naos/generated/system/Framebuffer.hpp"
#include "naos/generated/system/InputEventSource.hpp"
#include "naos/generated/system/MemoryObject.hpp"
#include "naos/generated/system/Process.hpp"
#include "naos/generated/system/ServiceDirectory.hpp"
#include "naos/generated/system/Stream.hpp"
#include "naos/generated/system/TerminalDriverControl.hpp"
#include "naos/generated/system/TerminalDriverFactory.hpp"
#include "naos/generated/system/TerminalJobControl.hpp"
#include "naos/generated/system_uapi.h"
#include <limits>

KLOG_MODULE(ipc);
namespace naos::ipc
{
namespace
{
constexpr u64 max_kernel_payload = NA_CHANNEL_MAX_MESSAGE_BYTES;
constexpr u64 no_deadline = 0;

struct deadline_watch
{
    handle_t<invocation_state> state;

    explicit deadline_watch(const handle_t<invocation_state> &state)
        : state(state)
    {
    }

    void invoke(timeclock::microsecond_t) noexcept
    {
        if (state)
            state->expire_deadline();
        state.reset();
        memory::Delete<>(memory::KernelCommonAllocatorV, this);
    }
};

std::atomic_uint64_t global_protocol_messages{0};
std::atomic_uint64_t global_protocol_bytes{0};
std::atomic_uint64_t global_protocol_resources{0};

struct kernel_dispatch_request
{
    handle_t<invocation_state> state;
    capability::entry target;
    handle_t<task::process_object> caller;
    u64 method_id;
    freelibcxx::vector<byte> bytes;
    capability::transfer_record_list resources;
    kernel_dispatch_request *prev = nullptr;
    kernel_dispatch_request *next = nullptr;

    kernel_dispatch_request(handle_t<invocation_state> state, capability::entry target,
                            handle_t<task::process_object> caller, u64 method_id)
        : state(std::move(state))
        , target(std::move(target))
        , caller(std::move(caller))
        , method_id(method_id)
        , bytes(memory::MemoryAllocatorV)
        , resources(memory::KernelCommonAllocatorV)
    {
    }

    kernel_dispatch_request(const kernel_dispatch_request &) = delete;
    kernel_dispatch_request &operator=(const kernel_dispatch_request &) = delete;
};

struct kernel_dispatch_queue
{
    lock::spinlock_t lock;
    task::wait_queue_t wait;
    kernel_dispatch_request *head = nullptr;
    kernel_dispatch_request *tail = nullptr;
    std::atomic_uint64_t pending{0};

    void enqueue(kernel_dispatch_request *request)
    {
        {
            uctx::RawSpinLockUninterruptibleContext guard(lock);
            request->prev = tail;
            request->next = nullptr;
            if (tail != nullptr)
                tail->next = request;
            else
                head = request;
            tail = request;
            pending.fetch_add(1, std::memory_order_release);
        }
        wait.do_wake_up(1);
    }

    kernel_dispatch_request *pop()
    {
        uctx::RawSpinLockUninterruptibleContext guard(lock);
        auto *request = head;
        if (request == nullptr)
            return nullptr;
        head = request->next;
        if (head != nullptr)
            head->prev = nullptr;
        else
            tail = nullptr;
        request->prev = nullptr;
        request->next = nullptr;
        pending.fetch_sub(1, std::memory_order_release);
        return request;
    }
};

kernel_dispatch_queue *kernel_dispatcher = nullptr;

void kernel_dispatch_worker(task::thread_start_info_t *info);

bool reserve_protocol_global(u64 bytes, u64 resources)
{
    auto reserve = [](std::atomic_uint64_t &counter, u64 amount, u64 limit) {
        if (amount > limit)
            return false;
        auto current = counter.load(std::memory_order_acquire);
        for (;;)
        {
            if (current > limit - amount)
                return false;
            if (counter.compare_exchange_weak(current, current + amount, std::memory_order_acq_rel))
                return true;
        }
    };
    if (!reserve(global_protocol_messages, 1, NA_CHANNEL_GLOBAL_MAX_MESSAGES))
        return false;
    if (!reserve(global_protocol_bytes, bytes, NA_CHANNEL_GLOBAL_MAX_BYTES))
    {
        global_protocol_messages.fetch_sub(1, std::memory_order_acq_rel);
        return false;
    }
    if (!reserve(global_protocol_resources, resources, NA_CHANNEL_GLOBAL_MAX_RESOURCES))
    {
        global_protocol_bytes.fetch_sub(bytes, std::memory_order_acq_rel);
        global_protocol_messages.fetch_sub(1, std::memory_order_acq_rel);
        return false;
    }
    return true;
}

void release_protocol_global(u64 bytes, u64 resources)
{
    global_protocol_messages.fetch_sub(1, std::memory_order_acq_rel);
    global_protocol_bytes.fetch_sub(bytes, std::memory_order_acq_rel);
    global_protocol_resources.fetch_sub(resources, std::memory_order_acq_rel);
}

freelibcxx::vector<byte> empty_bytes() { return freelibcxx::vector<byte>(memory::MemoryAllocatorV); }

capability::transfer_record_list empty_resources()
{
    return capability::transfer_record_list(memory::KernelCommonAllocatorV);
}

bool checked_multiply(u64 left, u64 right, u64 &result)
{
    if (left != 0 && right > std::numeric_limits<u64>::max() / left)
        return false;
    result = left * right;
    return true;
}

u64 calculate_deadline(u64 budget)
{
    if (budget == 0)
        return no_deadline;
    const u64 now = timer::get_high_resolution_time();
    if (budget > std::numeric_limits<u64>::max() - now)
        return std::numeric_limits<u64>::max();
    return now + budget;
}

bool valid_frame_size(u32 actual, u64 expected) { return naos::usercopy::valid_struct_size(actual, expected); }

template <typename T> na_status_t copy_frame(T &destination, const T *source)
{
    return naos::usercopy::copy_versioned(destination, source);
}

template <typename T> na_status_t write_frame(T *destination, const T &source)
{
    return naos::usercopy::copy_to(reinterpret_cast<u64>(destination), &source, sizeof(T));
}

template <typename T> bool valid_user_output(T *output)
{
    return output != nullptr && is_user_space_range(output, sizeof(T));
}

template <typename Message, typename Encoder>
bool encode_message(freelibcxx::vector<byte> &destination, const Message &message, Encoder encoder)
{
    // Probe the encoder for the exact wire size first.  A zero-capacity call
    // cannot touch a buffer and reports the required length in `written`, so
    // the scratch is sized to the message instead of a fixed per-call window.
    u64 written = 0;
    if (!encoder(nullptr, 0, message, written) && written == 0)
        return false;
    destination.resize(written, byte{});
    if (written != 0 && destination.data() == nullptr)
        return false;
    u64 encoded = 0;
    if (!encoder(reinterpret_cast<u8 *>(destination.data()), destination.size(), message, encoded) || encoded != written)
    {
        destination.clear();
        return false;
    }
    return true;
}

template <typename Message, typename Decoder>
bool decode_message(const freelibcxx::vector<byte> &source, Message &message, Decoder decoder)
{
    return decoder(reinterpret_cast<const u8 *>(source.data()), source.size(), message);
}

capability::metadata kernel_view_metadata(u64 scope, const na_uuid_t &uuid, na_meta_rights_t rights,
                                          u64 protocol_rights, u64 revision = 1, u64 features = 0)
{
    capability::metadata metadata;
    metadata.binding = NA_BINDING_KERNEL_VIEW;
    metadata.protocol_uuid = uuid;
    metadata.scope = scope;
    metadata.revision = revision;
    metadata.features = features;
    metadata.meta_rights = rights;
    metadata.protocol_rights = protocol_rights | NA_PROTOCOL_RIGHT_INVOKE;
    return metadata;
}

na_status_t validate_submit_frame(const na_submit_frame_t &frame, bool oneway)
{
    if (!valid_frame_size(frame.struct_size, sizeof(frame)) ||
        (frame.flags & ~(NA_CALL_FLAG_ONEWAY | NA_CALL_FLAG_FLEXIBLE)) != 0 ||
        (!oneway && (frame.flags & NA_CALL_FLAG_ONEWAY) != 0) || frame.reserved0 != 0 || frame.reserved1 != 0)
        return NA_STATUS_INVALID_ARGUMENT;
    if (frame.request_bytes > max_kernel_payload || frame.resource_count > NA_CHANNEL_MAX_RESOURCES)
        return NA_STATUS_INVALID_MESSAGE;
    u64 disposition_bytes = 0;
    if (!checked_multiply(frame.resource_count, sizeof(na_resource_disposition_t), disposition_bytes))
        return NA_STATUS_INVALID_ARGUMENT;
    if (!naos::usercopy::valid_range(frame.request, frame.request_bytes) ||
        !naos::usercopy::valid_range(frame.resources, disposition_bytes))
        return NA_STATUS_FAULT;
    if (frame.request_bytes != 0 && frame.request == 0)
        return NA_STATUS_FAULT;
    if (frame.resource_count != 0 && frame.resources == 0)
        return NA_STATUS_FAULT;
    return NA_STATUS_OK;
}

na_status_t snapshot_request(const na_submit_frame_t &frame, freelibcxx::vector<byte> &bytes,
                             freelibcxx::vector<na_resource_disposition_t> &dispositions)
{
    bytes.resize(frame.request_bytes, byte{});
    if (frame.request_bytes != 0)
    {
        const auto status = naos::usercopy::copy_from(bytes.data(), frame.request, frame.request_bytes);
        if (status != NA_STATUS_OK)
            return status;
    }

    if (frame.resource_count == 0)
        return NA_STATUS_OK;
    dispositions.resize(frame.resource_count, na_resource_disposition_t{});
    const u64 byte_count = frame.resource_count * sizeof(na_resource_disposition_t);
    const auto status = naos::usercopy::copy_from(dispositions.data(), frame.resources, byte_count);
    if (status != NA_STATUS_OK)
        return status;
    return NA_STATUS_OK;
}

/// Every segment of an iovec layout is a slice of the caller's bulk region,
/// so the payload size is bounded by the region window, not by the control
/// message envelope.
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
naos::data_plane::memory_object *request_region_object(capability::transfer_record_list &resources, u64 index,
                                                       u64 required_rights, u64 view_offset, u64 view_length,
                                                       u64 &absolute_offset)
{
    if (index >= resources.size())
        return nullptr;
    auto &record = resources[index];
    if (record.moved || !record.resource.valid())
        return nullptr;
    const auto &meta = record.resource.meta();
    if (meta.binding != NA_BINDING_MEMORY_OBJECT || meta.scope != NA_SCOPE_MEMORY_OBJECT)
        return nullptr;
    if ((meta.meta_rights & NA_RIGHT_TRANSFER) == 0)
        return nullptr;
    if ((meta.protocol_rights & required_rights) != required_rights)
        return nullptr;
    if (view_length == 0 || view_offset > meta.view_length || view_length > meta.view_length - view_offset ||
        meta.view_offset > ~u64(0) - view_offset)
        return nullptr;
    absolute_offset = meta.view_offset + view_offset;
    return record.resource.object()->get<naos::data_plane::memory_object>();
}

/// Move `size` bytes between two kernel-owned memory objects.  Both ends are
/// kernel objects, so the copy never goes through IPC and never allocates per
/// call; the chunked bounce keeps the object lock hold times short.  Aliasing
/// would defeat the chunking, so it is rejected instead of silently
/// mis-copying.
na_status_t copy_object_span(data_plane::memory_object &source, u64 source_offset, data_plane::memory_object &destination,
                             u64 destination_offset, u64 size, u64 &actual)
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
u64 fill_region_from_stream(Stream &stream, data_plane::memory_object &region, u64 region_offset,
                            u64 size, i64 &error)
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

template <typename Stream>
na_status_t publish_stream_call(invocation_state &state, Stream &stream, u64 method_id,
                                const freelibcxx::vector<byte> &request, capability::transfer_record_list &resources,
                                task::process_t *caller)
{
    state.mark_dispatched();
    auto response = freelibcxx::vector<byte>(memory::MemoryAllocatorV);
    const char *process_name = caller == nullptr || caller->name[0] == '\0' ? "process" : caller->name;

    if (method_id == NA_METHOD_STREAM_READ)
    {
        naos::system::Stream::read_request decoded{};
        if (!decode_message(request, decoded, naos::system::Stream::decode_read_request) ||
            !naos::system::Stream::validate_read_request_resources(decoded, resources.size()))
            return state.complete_failure(NA_EXECUTION_NOT_DELIVERED, NA_OUTCOME_REASON_PROTOCOL_VIOLATION)
                       ? NA_STATUS_OK
                       : NA_STATUS_INVALID_MESSAGE;
        u64 region_offset = 0;
        auto *region = request_region_object(resources, decoded.buffer.value,
                                             NA_MEMORY_RIGHT_MAP | NA_MEMORY_RIGHT_WRITE, 0,
                                             decoded.size, region_offset);
        if (region == nullptr ||
            !region->writable(region_offset, decoded.size))
            return state.complete_reply(empty_bytes(), empty_resources(), EINVAL) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
        i64 error = 0;
        naos::system::Stream::read_response value{};
        const u64 count = fill_region_from_stream(stream, *region, region_offset, decoded.size, error);
        value.count = error < 0 ? static_cast<u64>(-error) : count;
        if (!encode_message(response, value, naos::system::Stream::encode_read_response))
            return NA_STATUS_RESOURCE_EXHAUSTED;
        return state.complete_reply(std::move(response), empty_resources(), error) ? NA_STATUS_OK
                                                                                   : NA_STATUS_PEER_CLOSED;
    }

    if (method_id == NA_METHOD_STREAM_WRITE)
    {
        naos::system::Stream::write_request decoded{};
        if (!decode_message(request, decoded, naos::system::Stream::decode_write_request) ||
            !naos::system::Stream::validate_write_request_resources(decoded, resources.size()))
            return state.complete_failure(NA_EXECUTION_NOT_DELIVERED, NA_OUTCOME_REASON_PROTOCOL_VIOLATION)
                       ? NA_STATUS_OK
                       : NA_STATUS_INVALID_MESSAGE;
        u64 region_offset = 0;
        auto *region = request_region_object(resources, decoded.buffer.value,
                                             NA_MEMORY_RIGHT_MAP | NA_MEMORY_RIGHT_READ, 0,
                                             decoded.size, region_offset);
        if (region == nullptr ||
            !region->readable(region_offset, decoded.size))
            return state.complete_reply(empty_bytes(), empty_resources(), EINVAL) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
        i64 error = 0;
        naos::system::Stream::write_response value{};
        const u64 consumed = drain_region_to_stream(stream, *region, region_offset, decoded.size, process_name, error);
        value.count = error < 0 ? static_cast<u64>(-error) : consumed;
        if (!encode_message(response, value, naos::system::Stream::encode_write_response))
            return NA_STATUS_RESOURCE_EXHAUSTED;
        return state.complete_reply(std::move(response), empty_resources(), error) ? NA_STATUS_OK
                                                                                   : NA_STATUS_PEER_CLOSED;
    }

    if (method_id == NA_METHOD_STREAM_READV)
    {
        naos::system::Stream::readv_request decoded{};
        if (!decode_message(request, decoded, naos::system::Stream::decode_readv_request) ||
            !valid_iov_layout(decoded.layout, decoded.size) ||
            !naos::system::Stream::validate_readv_request_resources(decoded, resources.size()))
            return state.complete_failure(NA_EXECUTION_NOT_DELIVERED, NA_OUTCOME_REASON_PROTOCOL_VIOLATION)
                       ? NA_STATUS_OK
                       : NA_STATUS_INVALID_MESSAGE;
        u64 region_offset = 0;
        auto *region = request_region_object(resources, decoded.buffer.value,
                                             NA_MEMORY_RIGHT_MAP | NA_MEMORY_RIGHT_WRITE, 0,
                                             decoded.size, region_offset);
        if (region == nullptr ||
            !region->writable(region_offset, decoded.size))
            return state.complete_reply(empty_bytes(), empty_resources(), EINVAL) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
        i64 error = 0;
        naos::system::Stream::readv_response value{};
        const u64 count = fill_region_from_stream(stream, *region, region_offset, decoded.size, error);
        value.count = error < 0 ? static_cast<u64>(-error) : count;
        if (!encode_message(response, value, naos::system::Stream::encode_readv_response))
            return NA_STATUS_RESOURCE_EXHAUSTED;
        return state.complete_reply(std::move(response), empty_resources(), error) ? NA_STATUS_OK
                                                                                   : NA_STATUS_PEER_CLOSED;
    }

    if (method_id == NA_METHOD_STREAM_WRITEV)
    {
        naos::system::Stream::writev_request decoded{};
        if (!decode_message(request, decoded, naos::system::Stream::decode_writev_request) ||
            !valid_iov_layout(decoded.layout, decoded.size) ||
            !naos::system::Stream::validate_writev_request_resources(decoded, resources.size()))
            return state.complete_failure(NA_EXECUTION_NOT_DELIVERED, NA_OUTCOME_REASON_PROTOCOL_VIOLATION)
                       ? NA_STATUS_OK
                       : NA_STATUS_INVALID_MESSAGE;
        u64 region_offset = 0;
        auto *region = request_region_object(resources, decoded.buffer.value,
                                             NA_MEMORY_RIGHT_MAP | NA_MEMORY_RIGHT_READ, 0,
                                             decoded.size, region_offset);
        if (region == nullptr ||
            !region->readable(region_offset, decoded.size))
            return state.complete_reply(empty_bytes(), empty_resources(), EINVAL) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
        i64 error = 0;
        naos::system::Stream::writev_response value{};
        const u64 consumed = drain_region_to_stream(stream, *region, region_offset, decoded.size, process_name, error);
        value.count = error < 0 ? static_cast<u64>(-error) : consumed;
        if (!encode_message(response, value, naos::system::Stream::encode_writev_response))
            return NA_STATUS_RESOURCE_EXHAUSTED;
        return state.complete_reply(std::move(response), empty_resources(), error) ? NA_STATUS_OK
                                                                                   : NA_STATUS_PEER_CLOSED;
    }

    return NA_STATUS_NOT_SUPPORTED;
}

bool service_directory_mutation_allowed(const char *data, u32 size, u64 protocol_rights)
{
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

na_status_t publish_service_directory_register_call(invocation_state &state, service::directory &directory,
                                                    const freelibcxx::vector<byte> &request,
                                                    capability::transfer_record_list &resources, u64 protocol_rights,
                                                    task::process_t *caller)
{
    naos::system::ServiceDirectory::register_request decoded{};
    if (!decode_message(request, decoded, naos::system::ServiceDirectory::decode_register_request) ||
        !naos::system::ServiceDirectory::validate_register_request_resources(decoded, resources.size()) ||
        decoded.service.value >= resources.size())
        return state.complete_reply(empty_bytes(), empty_resources(), EINVAL) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
    if (!service_directory_mutation_allowed(decoded.uri.data, decoded.uri.size, protocol_rights))
        return state.complete_reply(empty_bytes(), empty_resources(), EACCES) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;

    const auto result = directory.register_service(reinterpret_cast<const char *>(decoded.uri.data), decoded.uri.size,
                                                   resources[decoded.service.value], false, caller->pid);
    return state.complete_reply(empty_bytes(), empty_resources(), result) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
}

na_status_t publish_service_directory_resolve_call(invocation_state &state, service::directory &directory,
                                                   const freelibcxx::vector<byte> &request, u64 protocol_rights)
{
    naos::system::ServiceDirectory::resolve_request decoded{};
    if (!decode_message(request, decoded, naos::system::ServiceDirectory::decode_resolve_request))
        return state.complete_reply(empty_bytes(), empty_resources(), EINVAL) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;

    capability::transferred_resource resource;
    const auto result =
        directory.resolve_service(reinterpret_cast<const char *>(decoded.uri.data), decoded.uri.size, resource);
    if (result != 0)
        return state.complete_reply(empty_bytes(), empty_resources(), result) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;

    naos::system::ServiceDirectory::resolve_response response{};
    response.service.value = 0;
    freelibcxx::vector<byte> encoded_response(memory::MemoryAllocatorV);
    if (!encode_message(encoded_response, response, naos::system::ServiceDirectory::encode_resolve_response))
        return state.complete_reply(empty_bytes(), empty_resources(), ENOMEM) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
    capability::transfer_record_list response_resources(memory::KernelCommonAllocatorV);
    response_resources.push_back(capability::transfer_record(NA_HANDLE_INVALID, true, std::move(resource)));
    return state.complete_reply(std::move(encoded_response), std::move(response_resources)) ? NA_STATUS_OK
                                                                                            : NA_STATUS_PEER_CLOSED;
}

na_status_t publish_service_directory_listen_call(invocation_state &state, service::directory &directory,
                                                  const freelibcxx::vector<byte> &request,
                                                  capability::transfer_record_list &resources, u64 protocol_rights,
                                                  task::process_t *caller)
{
    naos::system::ServiceDirectory::listen_request decoded{};
    if (!decode_message(request, decoded, naos::system::ServiceDirectory::decode_listen_request) ||
        !naos::system::ServiceDirectory::validate_listen_request_resources(decoded, resources.size()) ||
        decoded.listener.value >= resources.size() || decoded.descriptor.value >= resources.size() ||
        decoded.listener.value == decoded.descriptor.value)
        return state.complete_reply(empty_bytes(), empty_resources(), EINVAL) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
    if (!service_directory_mutation_allowed(decoded.uri.data, decoded.uri.size, protocol_rights))
        return state.complete_reply(empty_bytes(), empty_resources(), EACCES) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;

    khandle send_endpoint = resources[decoded.listener.value].resource.take_object_to_table();
    khandle descriptor = resources[decoded.descriptor.value].resource.take_object_to_table();
    const auto result =
        directory.listen_service(reinterpret_cast<const char *>(decoded.uri.data), decoded.uri.size,
                                 std::move(send_endpoint), std::move(descriptor), decoded.max_pending, caller->pid);
    return state.complete_reply(empty_bytes(), empty_resources(), result) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
}

na_status_t publish_service_directory_connect_call(invocation_state &state, service::directory &directory,
                                                   const freelibcxx::vector<byte> &request, task::process_t *caller)
{
    naos::system::ServiceDirectory::connect_request decoded{};
    if (!decode_message(request, decoded, naos::system::ServiceDirectory::decode_connect_request))
        return state.complete_reply(empty_bytes(), empty_resources(), EINVAL) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;

    na_uuid_t expected_uuid{};
    memcpy(expected_uuid.bytes, decoded.expected_uuid.data(), sizeof(expected_uuid.bytes));
    capability::transferred_resource client_resource;
    naos::system::ServiceDirectory::connect_response response{};
    response.client.value = 0;
    const auto result =
        directory.connect_service(reinterpret_cast<const char *>(decoded.uri.data), decoded.uri.size, expected_uuid,
                                  decoded.requested_rights, decoded.requested_revision, decoded.requested_features,
                                  caller, client_resource, response.revision, response.features);
    if (result != 0)
        return state.complete_reply(empty_bytes(), empty_resources(), result) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;

    freelibcxx::vector<byte> encoded_response(memory::MemoryAllocatorV);
    if (!encode_message(encoded_response, response, naos::system::ServiceDirectory::encode_connect_response))
        return state.complete_reply(empty_bytes(), empty_resources(), ENOMEM) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
    capability::transfer_record_list response_resources(memory::KernelCommonAllocatorV);
    response_resources.push_back(capability::transfer_record(NA_HANDLE_INVALID, true, std::move(client_resource)));
    return state.complete_reply(std::move(encoded_response), std::move(response_resources)) ? NA_STATUS_OK
                                                                                            : NA_STATUS_PEER_CLOSED;
}

na_status_t publish_service_directory_unregister_call(invocation_state &state, service::directory &directory,
                                                      const freelibcxx::vector<byte> &request, u64 protocol_rights,
                                                      task::process_t *caller)
{
    naos::system::ServiceDirectory::unregister_request decoded{};
    if (!decode_message(request, decoded, naos::system::ServiceDirectory::decode_unregister_request))
        return state.complete_reply(empty_bytes(), empty_resources(), EINVAL) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
    if (!service_directory_mutation_allowed(decoded.uri.data, decoded.uri.size, protocol_rights))
        return state.complete_reply(empty_bytes(), empty_resources(), EACCES) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
    const auto result =
        directory.unregister_service(reinterpret_cast<const char *>(decoded.uri.data), decoded.uri.size, caller->pid);
    return state.complete_reply(empty_bytes(), empty_resources(), result) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
}

na_status_t publish_service_directory_list_call(invocation_state &state, service::directory &directory,
                                                const freelibcxx::vector<byte> &request,
                                                capability::transfer_record_list &resources)
{
    naos::system::ServiceDirectory::list_request decoded{};
    if (!decode_message(request, decoded, naos::system::ServiceDirectory::decode_list_request) ||
        !naos::system::ServiceDirectory::validate_list_request_resources(decoded, resources.size()) ||
        decoded.requested_bytes > max_kernel_payload)
        return state.complete_reply(empty_bytes(), empty_resources(), EINVAL) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
    u64 region_offset = 0;
    auto *region = request_region_object(resources, decoded.buffer.value,
                                         NA_MEMORY_RIGHT_MAP | NA_MEMORY_RIGHT_WRITE, 0,
                                         decoded.requested_bytes, region_offset);
    if (region == nullptr ||
        !region->writable(region_offset, decoded.requested_bytes))
        return state.complete_reply(empty_bytes(), empty_resources(), EINVAL) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;

    freelibcxx::vector<byte> records(memory::MemoryAllocatorV);
    u64 next = 0;
    u64 count = 0;
    const auto result =
        directory.list_services(nullptr, 0, decoded.offset, decoded.requested_bytes, records, next, count);
    if (result != 0)
        return state.complete_reply(empty_bytes(), empty_resources(), result) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;

    u64 written_bytes = 0;
    if (region->write(region_offset, records.data(), records.size(), written_bytes) != NA_STATUS_OK)
        return state.complete_reply(empty_bytes(), empty_resources(), EINVAL) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;

    naos::system::ServiceDirectory::list_response response{};
    response.next = next;
    response.count = count;
    response.bytes = written_bytes;
    freelibcxx::vector<byte> encoded_response(memory::MemoryAllocatorV);
    if (!encode_message(encoded_response, response, naos::system::ServiceDirectory::encode_list_response))
        return state.complete_reply(empty_bytes(), empty_resources(), ENOMEM) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
    return state.complete_reply(std::move(encoded_response), empty_resources()) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
}

na_status_t publish_service_directory_list_prefix_call(invocation_state &state, service::directory &directory,
                                                       const freelibcxx::vector<byte> &request,
                                                       capability::transfer_record_list &resources)
{
    naos::system::ServiceDirectory::list_prefix_request decoded{};
    if (!decode_message(request, decoded, naos::system::ServiceDirectory::decode_list_prefix_request) ||
        !naos::system::ServiceDirectory::validate_list_prefix_request_resources(decoded, resources.size()) ||
        decoded.requested_bytes > max_kernel_payload)
        return state.complete_reply(empty_bytes(), empty_resources(), EINVAL) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
    u64 region_offset = 0;
    auto *region = request_region_object(resources, decoded.buffer.value,
                                         NA_MEMORY_RIGHT_MAP | NA_MEMORY_RIGHT_WRITE, 0,
                                         decoded.requested_bytes, region_offset);
    if (region == nullptr ||
        !region->writable(region_offset, decoded.requested_bytes))
        return state.complete_reply(empty_bytes(), empty_resources(), EINVAL) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;

    freelibcxx::vector<byte> records(memory::MemoryAllocatorV);
    u64 next = 0;
    u64 count = 0;
    const auto result =
        directory.list_services(reinterpret_cast<const char *>(decoded.prefix.data), decoded.prefix.size,
                                decoded.offset, decoded.requested_bytes, records, next, count);
    if (result != 0)
        return state.complete_reply(empty_bytes(), empty_resources(), result) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;

    u64 written_bytes = 0;
    if (region->write(region_offset, records.data(), records.size(), written_bytes) != NA_STATUS_OK)
        return state.complete_reply(empty_bytes(), empty_resources(), EINVAL) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;

    naos::system::ServiceDirectory::list_prefix_response response{};
    response.next = next;
    response.count = count;
    response.bytes = written_bytes;
    freelibcxx::vector<byte> encoded_response(memory::MemoryAllocatorV);
    if (!encode_message(encoded_response, response, naos::system::ServiceDirectory::encode_list_prefix_response))
        return state.complete_reply(empty_bytes(), empty_resources(), ENOMEM) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
    return state.complete_reply(std::move(encoded_response), empty_resources()) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
}

na_status_t publish_service_directory_call(invocation_state &state, service::directory &directory, u64 method_id,
                                           const freelibcxx::vector<byte> &request,
                                           capability::transfer_record_list &resources, u64 protocol_rights,
                                           task::process_t *caller)
{
    if (method_id == NA_METHOD_SERVICE_DIRECTORY_REGISTER)
    {
        return publish_service_directory_register_call(state, directory, request, resources, protocol_rights, caller);
    }

    if (method_id == NA_METHOD_SERVICE_DIRECTORY_RESOLVE)
    {
        return publish_service_directory_resolve_call(state, directory, request, protocol_rights);
    }

    if (method_id == NA_METHOD_SERVICE_DIRECTORY_LISTEN)
    {
        return publish_service_directory_listen_call(state, directory, request, resources, protocol_rights, caller);
    }

    if (method_id == NA_METHOD_SERVICE_DIRECTORY_CONNECT)
    {
        return publish_service_directory_connect_call(state, directory, request, caller);
    }

    if (method_id == NA_METHOD_SERVICE_DIRECTORY_UNREGISTER)
    {
        return publish_service_directory_unregister_call(state, directory, request, protocol_rights, caller);
    }

    if (method_id == NA_METHOD_SERVICE_DIRECTORY_LIST)
    {
        return publish_service_directory_list_call(state, directory, request, resources);
    }

    if (method_id == NA_METHOD_SERVICE_DIRECTORY_LIST_PREFIX)
    {
        return publish_service_directory_list_prefix_call(state, directory, request, resources);
    }

    return NA_STATUS_NOT_SUPPORTED;
}

na_status_t publish_process_call(invocation_state &state, task::process_object &process, u64 method_id,
                                 const freelibcxx::vector<byte> &request, task::process_t *caller)
{
    auto response = freelibcxx::vector<byte>(memory::MemoryAllocatorV);
    auto *target = process.process();
    if (target == nullptr)
        return NA_STATUS_OBJECT_REVOKED;

    if (method_id == NA_METHOD_PROCESS_WAIT)
    {
        naos::system::Process::wait_request decoded{};
        if (!decode_message(request, decoded, naos::system::Process::decode_wait_request))
            return NA_STATUS_INVALID_MESSAGE;
        i64 status = 0;
        process_id waited_pid = 0;
        const auto result = static_cast<i64>(task::wait_process_handle(
            caller, target, static_cast<flag_t>(decoded.flags), status, waited_pid,
            [&state] { return state.execution_interrupted(); },
            [&state](task::wait_queue_t *queue) { state.set_execution_wait_queue(queue); }));
        if (state.execution_interrupted())
            return NA_STATUS_PEER_CLOSED;
        if (result != 0)
            return state.complete_reply(empty_bytes(), empty_resources(), result) ? NA_STATUS_OK
                                                                                  : NA_STATUS_PEER_CLOSED;
        naos::system::Process::wait_response encoded{};
        encoded.status = status;
        encoded.pid = waited_pid;
        if (!encode_message(response, encoded, naos::system::Process::encode_wait_response))
            return NA_STATUS_RESOURCE_EXHAUSTED;
        return state.complete_reply(std::move(response), empty_resources()) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
    }

    if (method_id == NA_METHOD_PROCESS_WAIT_CHILDREN)
    {
        if (target != caller)
            return NA_STATUS_ACCESS_DENIED;
        naos::system::Process::wait_children_request decoded{};
        if (!decode_message(request, decoded, naos::system::Process::decode_wait_children_request))
            return NA_STATUS_INVALID_MESSAGE;
        i64 status = 0;
        process_id waited_pid = 0;
        const auto result = task::wait_process_children(
            caller, decoded.pid, static_cast<flag_t>(decoded.flags), status, waited_pid,
            [&state] { return state.execution_interrupted(); },
            [&state](task::wait_queue_t *queue) { state.set_execution_wait_queue(queue); });
        if (state.execution_interrupted())
            return NA_STATUS_PEER_CLOSED;
        if (result != 0)
            return state.complete_reply(empty_bytes(), empty_resources(), result) ? NA_STATUS_OK
                                                                                  : NA_STATUS_PEER_CLOSED;
        naos::system::Process::wait_children_response encoded{};
        encoded.status = status;
        encoded.pid = waited_pid;
        if (!encode_message(response, encoded, naos::system::Process::encode_wait_children_response))
            return NA_STATUS_RESOURCE_EXHAUSTED;
        return state.complete_reply(std::move(response), empty_resources()) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
    }

    if (method_id == NA_METHOD_PROCESS_GET_INFO)
    {
        naos::system::Process::get_info_request decoded{};
        if (!decode_message(request, decoded, naos::system::Process::decode_get_info_request))
            return NA_STATUS_INVALID_MESSAGE;
        naos::system::Process::get_info_response encoded{};
        encoded.pid = target->pid;
        encoded.parent_pid = target->parent_pid;
        encoded.attributes = target->attributes.load();
        encoded.return_value = target->ret_val;
        if (!encode_message(response, encoded, naos::system::Process::encode_get_info_response))
            return NA_STATUS_RESOURCE_EXHAUSTED;
        return state.complete_reply(std::move(response), empty_resources()) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
    }

    if (method_id == NA_METHOD_PROCESS_GET_JOB_CONTROL_INFO)
    {
        naos::system::Process::get_job_control_info_request decoded{};
        if (!decode_message(request, decoded, naos::system::Process::decode_get_job_control_info_request))
            return NA_STATUS_INVALID_MESSAGE;
        task::job_control_info info{};
        if (!task::get_job_control_info(target, info))
            return NA_STATUS_OBJECT_REVOKED;
        naos::system::Process::get_job_control_info_response encoded{};
        encoded.session = static_cast<i64>(info.session);
        encoded.process_group = static_cast<i64>(info.process_group);
        encoded.foreground_process_group = static_cast<i64>(info.foreground_process_group);
        encoded.has_controlling_tty = info.has_controlling_tty ? 1 : 0;
        if (!encode_message(response, encoded, naos::system::Process::encode_get_job_control_info_response))
            return NA_STATUS_RESOURCE_EXHAUSTED;
        return state.complete_reply(std::move(response), empty_resources()) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
    }

    if (method_id == NA_METHOD_PROCESS_GET_CONTROLLING_TERMINAL)
    {
        if (target != caller)
            return NA_STATUS_ACCESS_DENIED;
        naos::system::Process::get_controlling_terminal_request decoded{};
        if (!decode_message(request, decoded, naos::system::Process::decode_get_controlling_terminal_request))
            return NA_STATUS_INVALID_MESSAGE;
        na_terminal_locator_t locator{};
        if (!task::get_controlling_terminal_locator(target, locator))
            return state.complete_reply(empty_bytes(), empty_resources(), ENXIO) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
        naos::system::Process::get_controlling_terminal_response encoded{};
        encoded.terminal_id = locator.terminal_id;
        encoded.generation = locator.generation;
        for (u64 i = 0; i < encoded.token.size(); i++)
            encoded.token[i] = locator.token[i];
        if (!encode_message(response, encoded, naos::system::Process::encode_get_controlling_terminal_response))
            return NA_STATUS_RESOURCE_EXHAUSTED;
        return state.complete_reply(std::move(response), empty_resources()) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
    }

    if (method_id == NA_METHOD_PROCESS_SET_SESSION)
    {
        if (target != caller)
            return NA_STATUS_ACCESS_DENIED;
        naos::system::Process::set_session_request decoded{};
        if (!decode_message(request, decoded, naos::system::Process::decode_set_session_request))
            return NA_STATUS_INVALID_MESSAGE;
        const auto session = task::setsid(target);
        if (session < 0)
            return state.complete_reply(empty_bytes(), empty_resources(), session) ? NA_STATUS_OK
                                                                                   : NA_STATUS_PEER_CLOSED;
        naos::system::Process::set_session_response encoded{};
        encoded.session = session;
        if (!encode_message(response, encoded, naos::system::Process::encode_set_session_response))
            return NA_STATUS_RESOURCE_EXHAUSTED;
        return state.complete_reply(std::move(response), empty_resources()) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
    }

    if (method_id == NA_METHOD_PROCESS_GET_PROCESS_GROUP || method_id == NA_METHOD_PROCESS_GET_SESSION)
    {
        task::job_control_info info{};
        if (!task::get_job_control_info(target, info))
            return NA_STATUS_OBJECT_REVOKED;
        if (method_id == NA_METHOD_PROCESS_GET_PROCESS_GROUP)
        {
            naos::system::Process::get_process_group_request decoded{};
            if (!decode_message(request, decoded, naos::system::Process::decode_get_process_group_request))
                return NA_STATUS_INVALID_MESSAGE;
            naos::system::Process::get_process_group_response encoded{};
            encoded.process_group = static_cast<i64>(info.process_group);
            if (!encode_message(response, encoded, naos::system::Process::encode_get_process_group_response))
                return NA_STATUS_RESOURCE_EXHAUSTED;
        }
        else
        {
            naos::system::Process::get_session_request decoded{};
            if (!decode_message(request, decoded, naos::system::Process::decode_get_session_request))
                return NA_STATUS_INVALID_MESSAGE;
            naos::system::Process::get_session_response encoded{};
            encoded.session = static_cast<i64>(info.session);
            if (!encode_message(response, encoded, naos::system::Process::encode_get_session_response))
                return NA_STATUS_RESOURCE_EXHAUSTED;
        }
        return state.complete_reply(std::move(response), empty_resources()) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
    }

    if (method_id == NA_METHOD_PROCESS_SET_PROCESS_GROUP)
    {
        naos::system::Process::set_process_group_request decoded{};
        if (!decode_message(request, decoded, naos::system::Process::decode_set_process_group_request))
            return NA_STATUS_INVALID_MESSAGE;
        if (decoded.process_group < 0)
            return NA_STATUS_INVALID_ARGUMENT;
        const auto result = task::setpgid(caller, target->pid, static_cast<group_id>(decoded.process_group));
        return state.complete_reply(empty_bytes(), empty_resources(), result) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
    }

    if (method_id == NA_METHOD_PROCESS_START)
    {
        if (caller == nullptr || target->parent_pid != caller->pid)
        {
            return NA_STATUS_ACCESS_DENIED;
        }
        naos::system::Process::start_request decoded{};
        if (!decode_message(request, decoded, naos::system::Process::decode_start_request))
        {
            return NA_STATUS_INVALID_MESSAGE;
        }
        if (target->main_thread_started.load())
        {
            return state.complete_reply(empty_bytes(), empty_resources(), EBUSY) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
        }
        task::start_process(target);
        return state.complete_reply(empty_bytes(), empty_resources(), 0) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
    }

    return NA_STATUS_NOT_SUPPORTED;
}

na_status_t publish_memory_object_call(invocation_state &state, data_plane::memory_object &memory_object,
                                       const capability::metadata &object_meta, u64 method_id,
                                       const freelibcxx::vector<byte> &request,
                                       capability::transfer_record_list &resources)
{
    auto response = freelibcxx::vector<byte>(memory::MemoryAllocatorV);
    const auto resolve_object_offset = [&](u64 offset, u64 length, u64 &absolute) {
        if (length == 0 || offset > object_meta.view_length || length > object_meta.view_length - offset ||
            object_meta.view_offset > ~u64(0) - offset)
            return false;
        absolute = object_meta.view_offset + offset;
        return true;
    };
    if (method_id == NA_METHOD_MEMORY_OBJECT_GET_INFO)
    {
        naos::system::MemoryObject::get_info_request decoded{};
        if (!decode_message(request, decoded, naos::system::MemoryObject::decode_get_info_request))
            return NA_STATUS_INVALID_MESSAGE;
        naos::system::MemoryObject::get_info_response encoded{};
        encoded.flags = memory_object.flags();
        encoded.size = object_meta.view_length;
        encoded.max_size = NA_MEMORY_OBJECT_MAX_BYTES;
        if (!encode_message(response, encoded, naos::system::MemoryObject::encode_get_info_response))
            return NA_STATUS_RESOURCE_EXHAUSTED;
        return state.complete_reply(std::move(response), empty_resources()) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
    }

    if (method_id == NA_METHOD_MEMORY_OBJECT_READ)
    {
        naos::system::MemoryObject::read_request decoded{};
        if (!decode_message(request, decoded, naos::system::MemoryObject::decode_read_request) ||
            !naos::system::MemoryObject::validate_read_request_resources(decoded, resources.size()))
            return NA_STATUS_INVALID_MESSAGE;
        u64 region_offset = 0;
        auto *region = request_region_object(resources, decoded.buffer.value,
                                             NA_MEMORY_RIGHT_MAP | NA_MEMORY_RIGHT_WRITE, 0,
                                             decoded.size, region_offset);
        u64 object_offset = 0;
        if (region == nullptr ||
            !region->writable(region_offset, decoded.size) ||
            !resolve_object_offset(decoded.offset, decoded.size, object_offset) ||
            !memory_object.readable(object_offset, decoded.size))
            return state.complete_reply(empty_bytes(), empty_resources(), EINVAL) ? NA_STATUS_OK
                                                                                 : NA_STATUS_PEER_CLOSED;

        u64 actual = 0;
        const auto status =
            copy_object_span(memory_object, object_offset, *region, region_offset, decoded.size, actual);
        if (status != NA_STATUS_OK)
            return state.complete_reply(empty_bytes(), empty_resources(), EINVAL) ? NA_STATUS_OK
                                                                                 : NA_STATUS_PEER_CLOSED;

        naos::system::MemoryObject::read_response encoded{};
        encoded.count = actual;
        if (!encode_message(response, encoded, naos::system::MemoryObject::encode_read_response))
            return NA_STATUS_RESOURCE_EXHAUSTED;
        return state.complete_reply(std::move(response), empty_resources()) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
    }

    if (method_id == NA_METHOD_MEMORY_OBJECT_WRITE)
    {
        naos::system::MemoryObject::write_request decoded{};
        if (!decode_message(request, decoded, naos::system::MemoryObject::decode_write_request) ||
            !naos::system::MemoryObject::validate_write_request_resources(decoded, resources.size()))
            return NA_STATUS_INVALID_MESSAGE;
        u64 region_offset = 0;
        auto *region = request_region_object(resources, decoded.buffer.value,
                                             NA_MEMORY_RIGHT_MAP | NA_MEMORY_RIGHT_READ, 0,
                                             decoded.size, region_offset);
        u64 object_offset = 0;
        if (region == nullptr ||
            !region->readable(region_offset, decoded.size) ||
            !resolve_object_offset(decoded.offset, decoded.size, object_offset))
            return state.complete_reply(empty_bytes(), empty_resources(), EINVAL) ? NA_STATUS_OK
                                                                                 : NA_STATUS_PEER_CLOSED;
        if (!memory_object.writable(object_offset, decoded.size))
            return state.complete_reply(empty_bytes(), empty_resources(),
                                        (memory_object.flags() & NA_MEMORY_FLAG_READ_ONLY) != 0 ? EACCES : EINVAL)
                       ? NA_STATUS_OK
                       : NA_STATUS_PEER_CLOSED;

        u64 actual = 0;
        const auto status =
            copy_object_span(*region, region_offset, memory_object, object_offset, decoded.size, actual);
        if (status != NA_STATUS_OK)
            return state.complete_reply(empty_bytes(), empty_resources(),
                                        status == NA_STATUS_ACCESS_DENIED ? EACCES : EINVAL)
                       ? NA_STATUS_OK
                       : NA_STATUS_PEER_CLOSED;

        naos::system::MemoryObject::write_response encoded{};
        encoded.count = actual;
        if (!encode_message(response, encoded, naos::system::MemoryObject::encode_write_response))
            return NA_STATUS_RESOURCE_EXHAUSTED;
        return state.complete_reply(std::move(response), empty_resources()) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
    }

    return NA_STATUS_NOT_SUPPORTED;
}

na_status_t publish_framebuffer_call(invocation_state &state, dev::framebuffer::framebuffer_service &framebuffer,
                                     u64 method_id, const freelibcxx::vector<byte> &wire)
{
    if (method_id != NA_METHOD_FRAMEBUFFER_GET)
        return NA_STATUS_NOT_SUPPORTED;

    naos::system::Framebuffer::get_request request{};
    // Framebuffer::get has no request fields, but still validate its wire
    // representation so the service has the same strict contract as every
    // generated protocol.
    if (!decode_message(wire, request, naos::system::Framebuffer::decode_get_request))
        return state.complete_reply(empty_bytes(), empty_resources(), EINVAL) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
    if (!term::try_acquire_framebuffer_user_writer())
        return state.complete_reply(empty_bytes(), empty_resources(), EBUSY) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;

    auto object = handle_t<data_plane::memory_object>::make(
        framebuffer.physical_base(), static_cast<byte *>(framebuffer.kernel_view()), framebuffer.frame_bytes(),
        &term::release_framebuffer_user_writer);
    if (!object)
    {
        term::release_framebuffer_user_writer();
        return state.complete_reply(empty_bytes(), empty_resources(), ENOMEM) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
    }

    auto object_meta = kernel_view_metadata(NA_SCOPE_MEMORY_OBJECT, naos::system::MemoryObject::protocol_uuid,
                                            NA_RIGHT_TRANSFER | NA_RIGHT_WAIT | NA_RIGHT_INSPECT,
                                            NA_MEMORY_RIGHT_READ | NA_MEMORY_RIGHT_WRITE | NA_MEMORY_RIGHT_MAP,
                                            naos::system::MemoryObject::revision, naos::system::MemoryObject::features);
    object_meta.binding = NA_BINDING_MEMORY_OBJECT;
    object_meta.view_offset = 0;
    object_meta.view_length = framebuffer.frame_bytes();

    naos::system::Framebuffer::get_response response{};
    response.info.width = framebuffer.width();
    response.info.height = framebuffer.height();
    response.info.pitch = framebuffer.pitch();
    response.info.bpp = framebuffer.bpp();
    response.info.type = framebuffer.type();
    response.info.smem_bytes = framebuffer.frame_bytes();
    response.framebuffer.value = 0;

    freelibcxx::vector<byte> encoded(memory::MemoryAllocatorV);
    if (!encode_message(encoded, response, naos::system::Framebuffer::encode_get_response))
    {
        object.reset();
        return NA_STATUS_RESOURCE_EXHAUSTED;
    }

    capability::transfer_record_list response_resources(memory::KernelCommonAllocatorV);
    response_resources.push_back(capability::transfer_record(
        NA_HANDLE_INVALID, true, capability::transferred_resource(std::move(object), object_meta)));
    return state.complete_reply(std::move(encoded), std::move(response_resources)) ? NA_STATUS_OK
                                                                                   : NA_STATUS_PEER_CLOSED;
}

na_status_t publish_input_event_source_call(invocation_state &state, dev::input::input_event_source &source,
                                            u64 method_id, const freelibcxx::vector<byte> &request)
{
    if (method_id != NA_METHOD_INPUT_EVENT_SOURCE_SUBSCRIBE)
        return NA_STATUS_NOT_SUPPORTED;

    naos::system::InputEventSource::subscribe_request decoded{};
    if (!decode_message(request, decoded, naos::system::InputEventSource::decode_subscribe_request))
        return state.complete_reply(empty_bytes(), empty_resources(), EINVAL) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;

    khandle receiver;
    if (!source.subscribe(receiver, decoded.max_events))
        return state.complete_reply(empty_bytes(), empty_resources(), EAGAIN) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;

    auto meta = kernel_view_metadata(NA_SCOPE_INPUT_EVENT_SOURCE, naos::system::InputEventSource::protocol_uuid,
                                     NA_RIGHT_TRANSFER | NA_RIGHT_WAIT | NA_RIGHT_INSPECT,
                                     NA_RIGHT_TRANSFER | NA_RIGHT_WAIT | NA_RIGHT_INSPECT);
    meta.binding = NA_BINDING_RAW_CHANNEL_END;
    meta.scope = 0;

    naos::system::InputEventSource::subscribe_response response{};
    response.receiver.value = 0;
    freelibcxx::vector<byte> encoded_response(memory::MemoryAllocatorV);
    if (!encode_message(encoded_response, response, naos::system::InputEventSource::encode_subscribe_response))
    {
        (void)source.rollback_subscription(receiver.operator&());
        return NA_STATUS_RESOURCE_EXHAUSTED;
    }

    capability::transfer_record_list response_resources(memory::KernelCommonAllocatorV);
    auto *receiver_object = receiver.operator&();
    response_resources.push_back(
        capability::transfer_record(NA_HANDLE_INVALID, true, capability::transferred_resource(receiver, meta)));
    if (!state.complete_reply(std::move(encoded_response), std::move(response_resources)))
    {
        (void)source.rollback_subscription(receiver_object);
        return NA_STATUS_PEER_CLOSED;
    }
    return NA_STATUS_OK;
}

na_status_t publish_terminal_job_control_call(invocation_state &state, dev::tty::terminal_job_control &view,
                                              u64 method_id, const freelibcxx::vector<byte> &request,
                                              task::process_t *caller)
{
    auto *identity = view.identity();
    auto *process = caller;
    if (identity == nullptr || !identity->live() || process == nullptr)
        return NA_STATUS_OBJECT_REVOKED;

    if (method_id == NA_METHOD_TERMINAL_JOB_CONTROL_CHECK_IO)
    {
        return [&]() __attribute__((noinline)) -> na_status_t {
            naos::system::TerminalJobControl::check_io_request decoded{};
            if (!decode_message(request, decoded, naos::system::TerminalJobControl::decode_check_io_request))
                return state.complete_reply(empty_bytes(), empty_resources(), EINVAL) ? NA_STATUS_OK
                                                                                      : NA_STATUS_PEER_CLOSED;
            const auto result =
                task::check_terminal_job_control(process, identity, decoded.direction == 1, decoded.tostop != 0);
            if (result != 0)
                return state.complete_reply(empty_bytes(), empty_resources(), result) ? NA_STATUS_OK
                                                                                      : NA_STATUS_PEER_CLOSED;
            return state.complete_reply(empty_bytes(), empty_resources(), 0) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
        }();
    }
    if (method_id == NA_METHOD_TERMINAL_JOB_CONTROL_ATTACH)
    {
        return [&]() __attribute__((noinline)) -> na_status_t {
            naos::system::TerminalJobControl::attach_request decoded{};
            if (!decode_message(request, decoded, naos::system::TerminalJobControl::decode_attach_request))
                return state.complete_reply(empty_bytes(), empty_resources(), EINVAL) ? NA_STATUS_OK
                                                                                      : NA_STATUS_PEER_CLOSED;
            if (decoded.force != 0 &&
                !caller->resource.has_native_object_type(kobject::type_e::terminal_driver_factory))
                return state.complete_reply(empty_bytes(), empty_resources(), EACCES) ? NA_STATUS_OK
                                                                                      : NA_STATUS_PEER_CLOSED;
            const auto result = task::attach_controlling_terminal(process, view.identity_handle(), decoded.force != 0);
            return state.complete_reply(empty_bytes(), empty_resources(), result) ? NA_STATUS_OK
                                                                                  : NA_STATUS_PEER_CLOSED;
        }();
    }
    if (method_id == NA_METHOD_TERMINAL_JOB_CONTROL_DETACH)
    {
        return [&]() __attribute__((noinline)) -> na_status_t {
            naos::system::TerminalJobControl::detach_request decoded{};
            if (!decode_message(request, decoded, naos::system::TerminalJobControl::decode_detach_request))
                return state.complete_reply(empty_bytes(), empty_resources(), EINVAL) ? NA_STATUS_OK
                                                                                      : NA_STATUS_PEER_CLOSED;
            task::detach_controlling_terminal(process);
            return state.complete_reply(empty_bytes(), empty_resources(), 0) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
        }();
    }
    if (method_id == NA_METHOD_TERMINAL_JOB_CONTROL_GET_PGRP)
    {
        return [&]() __attribute__((noinline)) -> na_status_t {
            naos::system::TerminalJobControl::get_pgrp_request decoded{};
            if (!decode_message(request, decoded, naos::system::TerminalJobControl::decode_get_pgrp_request))
                return state.complete_reply(empty_bytes(), empty_resources(), EINVAL) ? NA_STATUS_OK
                                                                                      : NA_STATUS_PEER_CLOSED;
            const auto group = task::get_foreground_process_group(identity);
            if (group < 0)
                return state.complete_reply(empty_bytes(), empty_resources(), static_cast<i64>(-group))
                           ? NA_STATUS_OK
                           : NA_STATUS_PEER_CLOSED;
            naos::system::TerminalJobControl::get_pgrp_response response{};
            response.group = static_cast<u64>(group);
            freelibcxx::vector<byte> encoded(memory::MemoryAllocatorV);
            if (!encode_message(encoded, response, naos::system::TerminalJobControl::encode_get_pgrp_response))
                return NA_STATUS_RESOURCE_EXHAUSTED;
            return state.complete_reply(std::move(encoded), empty_resources()) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
        }();
    }
    if (method_id == NA_METHOD_TERMINAL_JOB_CONTROL_SET_PGRP)
    {
        return [&]() __attribute__((noinline)) -> na_status_t {
            naos::system::TerminalJobControl::set_pgrp_request decoded{};
            if (!decode_message(request, decoded, naos::system::TerminalJobControl::decode_set_pgrp_request))
                return state.complete_reply(empty_bytes(), empty_resources(), EINVAL) ? NA_STATUS_OK
                                                                                      : NA_STATUS_PEER_CLOSED;
            const auto result =
                task::set_foreground_process_group(process, identity, static_cast<group_id>(decoded.group));
            return state.complete_reply(empty_bytes(), empty_resources(), result) ? NA_STATUS_OK
                                                                                  : NA_STATUS_PEER_CLOSED;
        }();
    }
    if (method_id == NA_METHOD_TERMINAL_JOB_CONTROL_GET_SID)
    {
        return [&]() __attribute__((noinline)) -> na_status_t {
            naos::system::TerminalJobControl::get_sid_request decoded{};
            if (!decode_message(request, decoded, naos::system::TerminalJobControl::decode_get_sid_request))
                return state.complete_reply(empty_bytes(), empty_resources(), EINVAL) ? NA_STATUS_OK
                                                                                      : NA_STATUS_PEER_CLOSED;
            naos::system::TerminalJobControl::get_sid_response response{};
            response.session = identity->session_id();
            freelibcxx::vector<byte> encoded(memory::MemoryAllocatorV);
            if (!encode_message(encoded, response, naos::system::TerminalJobControl::encode_get_sid_response))
                return NA_STATUS_RESOURCE_EXHAUSTED;
            return state.complete_reply(std::move(encoded), empty_resources()) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
        }();
    }
    if (method_id == NA_METHOD_TERMINAL_JOB_CONTROL_QUERY)
    {
        return [&]() __attribute__((noinline)) -> na_status_t {
            naos::system::TerminalJobControl::query_request decoded{};
            if (!decode_message(request, decoded, naos::system::TerminalJobControl::decode_query_request))
                return state.complete_reply(empty_bytes(), empty_resources(), EINVAL) ? NA_STATUS_OK
                                                                                      : NA_STATUS_PEER_CLOSED;
            naos::system::TerminalJobControl::query_response response{};
            response.state.session = identity->session_id();
            response.state.process_group = process->process_group_id;
            response.state.foreground_process_group = identity->foreground_process_group();
            response.state.has_controlling_terminal = task::get_controlling_terminal(process) == identity ? 1 : 0;
            response.state.generation = identity->generation();
            freelibcxx::vector<byte> encoded(memory::MemoryAllocatorV);
            if (!encode_message(encoded, response, naos::system::TerminalJobControl::encode_query_response))
                return NA_STATUS_RESOURCE_EXHAUSTED;
            return state.complete_reply(std::move(encoded), empty_resources()) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
        }();
    }
    return NA_STATUS_NOT_SUPPORTED;
}

na_status_t publish_terminal_driver_control_call(invocation_state &state, dev::tty::terminal_driver_control &view,
                                                 u64 method_id, const freelibcxx::vector<byte> &request)
{
    auto *identity = view.identity();
    if (identity == nullptr || !identity->live())
        return NA_STATUS_OBJECT_REVOKED;

    if (method_id == NA_METHOD_TERMINAL_DRIVER_CONTROL_RAISE_FOREGROUND)
    {
        naos::system::TerminalDriverControl::raise_foreground_request decoded{};
        if (!decode_message(request, decoded, naos::system::TerminalDriverControl::decode_raise_foreground_request))
            return state.complete_reply(empty_bytes(), empty_resources(), EINVAL) ? NA_STATUS_OK
                                                                                  : NA_STATUS_PEER_CLOSED;
        task::signal_num_t signal_number = task::signal::sigint;
        if (decoded.action == naos::system::TerminalDriverControl::DriverAction::quit)
            signal_number = task::signal::sigquit;
        else if (decoded.action == naos::system::TerminalDriverControl::DriverAction::suspend)
            signal_number = task::signal::sigtstp;
        group_id foreground = 0;
        if (!identity->foreground_if_live(foreground))
            return state.complete_reply(empty_bytes(), empty_resources(), EIO) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
        const auto signal_result =
            foreground > 0 ? task::send_signal_to_process_group(static_cast<group_id>(foreground), signal_number)
                           : ENOENT;
        return state.complete_reply(empty_bytes(), empty_resources(), signal_result < 0 ? signal_result : 0)
                   ? NA_STATUS_OK
                   : NA_STATUS_PEER_CLOSED;
    }
    if (method_id == NA_METHOD_TERMINAL_DRIVER_CONTROL_NOTIFY_WINSIZE_CHANGED)
    {
        naos::system::TerminalDriverControl::notify_winsize_changed_request decoded{};
        if (!decode_message(request, decoded,
                            naos::system::TerminalDriverControl::decode_notify_winsize_changed_request))
            return state.complete_reply(empty_bytes(), empty_resources(), EINVAL) ? NA_STATUS_OK
                                                                                  : NA_STATUS_PEER_CLOSED;
        group_id foreground = 0;
        if (!identity->foreground_if_live(foreground))
            // Resizing still succeeds when there is no foreground process;
            // there is simply nobody to notify.
            return state.complete_reply(empty_bytes(), empty_resources(), 0) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
        const auto signal_result =
            foreground > 0
                ? task::send_signal_to_process_group(static_cast<group_id>(foreground), task::signal::sigwinch)
                : ENOENT;
        const i64 notification_error = signal_result == ENOENT ? 0 : (signal_result < 0 ? signal_result : 0);
        return state.complete_reply(empty_bytes(), empty_resources(), notification_error) ? NA_STATUS_OK
                                                                                          : NA_STATUS_PEER_CLOSED;
    }
    if (method_id == NA_METHOD_TERMINAL_DRIVER_CONTROL_HANGUP)
    {
        naos::system::TerminalDriverControl::hangup_request decoded{};
        if (!decode_message(request, decoded, naos::system::TerminalDriverControl::decode_hangup_request))
            return state.complete_reply(empty_bytes(), empty_resources(), EINVAL) ? NA_STATUS_OK
                                                                                  : NA_STATUS_PEER_CLOSED;
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
            return state.complete_reply(empty_bytes(), empty_resources(), signal_error) ? NA_STATUS_OK
                                                                                        : NA_STATUS_PEER_CLOSED;
        }
        return state.complete_reply(empty_bytes(), empty_resources(), 0) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
    }
    if (method_id == NA_METHOD_TERMINAL_DRIVER_CONTROL_REVOKE)
    {
        naos::system::TerminalDriverControl::revoke_request decoded{};
        if (!decode_message(request, decoded, naos::system::TerminalDriverControl::decode_revoke_request))
            return state.complete_reply(empty_bytes(), empty_resources(), EINVAL) ? NA_STATUS_OK
                                                                                  : NA_STATUS_PEER_CLOSED;
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
            return state.complete_reply(empty_bytes(), empty_resources(), signal_error) ? NA_STATUS_OK
                                                                                        : NA_STATUS_PEER_CLOSED;
        }
        return state.complete_reply(empty_bytes(), empty_resources(), 0) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
    }
    return NA_STATUS_NOT_SUPPORTED;
}

na_status_t publish_terminal_driver_factory_call(invocation_state &state, dev::tty::terminal_driver_factory &factory,
                                                 u64 method_id, const freelibcxx::vector<byte> &request)
{
    if (method_id == NA_METHOD_TERMINAL_DRIVER_FACTORY_CREATE)
    {
        return [&]() __attribute__((noinline)) -> na_status_t {
            naos::system::TerminalDriverFactory::create_request decoded{};
            if (!decode_message(request, decoded, naos::system::TerminalDriverFactory::decode_create_request))
                return state.complete_reply(empty_bytes(), empty_resources(), EINVAL) ? NA_STATUS_OK
                                                                                      : NA_STATUS_PEER_CLOSED;
            khandle identity;
            khandle job_control;
            khandle driver_control;
            if (!factory.create(decoded.mode, identity, job_control, driver_control))
                return state.complete_reply(empty_bytes(), empty_resources(), EAGAIN) ? NA_STATUS_OK
                                                                                      : NA_STATUS_PEER_CLOSED;

            auto driver_meta = kernel_view_metadata(
                NA_SCOPE_TERMINAL_DRIVER_CONTROL, naos::system::TerminalDriverControl::protocol_uuid,
                NA_RIGHT_DUPLICATE | NA_RIGHT_TRANSFER | NA_RIGHT_WAIT | NA_RIGHT_INSPECT,
                NA_RIGHT_TRANSFER | NA_RIGHT_WAIT | NA_RIGHT_INSPECT);
            auto job_meta = kernel_view_metadata(
                NA_SCOPE_TERMINAL_JOB_CONTROL, naos::system::TerminalJobControl::protocol_uuid,
                NA_RIGHT_DUPLICATE | NA_RIGHT_TRANSFER | NA_RIGHT_WAIT | NA_RIGHT_INSPECT,
                NA_RIGHT_TRANSFER | NA_RIGHT_WAIT | NA_RIGHT_INSPECT, naos::system::TerminalJobControl::revision,
                naos::system::TerminalJobControl::features);
            naos::system::TerminalDriverFactory::create_response response{};
            auto *typed_identity = identity.as<dev::tty::terminal_identity>().operator&();
            if (typed_identity == nullptr)
            {
                factory.rollback(0, 0);
                return state.complete_reply(empty_bytes(), empty_resources(), EIO) ? NA_STATUS_OK
                                                                                   : NA_STATUS_PEER_CLOSED;
            }
            response.locator.terminal_id = typed_identity == nullptr ? 0 : typed_identity->id();
            response.locator.generation = typed_identity == nullptr ? 0 : typed_identity->generation();
            response.locator.token = typed_identity == nullptr ? std::array<u8, 16>{} : typed_identity->token();
            response.driver.value = 0;
            response.job_control.value = 1;
            freelibcxx::vector<byte> encoded(memory::MemoryAllocatorV);
            if (!encode_message(encoded, response, naos::system::TerminalDriverFactory::encode_create_response))
            {
                factory.rollback(response.locator.terminal_id, response.locator.generation);
                return NA_STATUS_RESOURCE_EXHAUSTED;
            }

            capability::transfer_record_list response_resources(memory::KernelCommonAllocatorV);
            response_resources.push_back(capability::transfer_record(
                NA_HANDLE_INVALID, true, capability::transferred_resource(std::move(driver_control), driver_meta)));
            response_resources.push_back(capability::transfer_record(
                NA_HANDLE_INVALID, true, capability::transferred_resource(std::move(job_control), job_meta)));
            if (!state.complete_reply(std::move(encoded), std::move(response_resources)))
            {
                factory.rollback(response.locator.terminal_id, response.locator.generation);
                return NA_STATUS_PEER_CLOSED;
            }
            return NA_STATUS_OK;
        }();
    }
    if (method_id == NA_METHOD_TERMINAL_DRIVER_FACTORY_VALIDATE_LOCATOR)
    {
        naos::system::TerminalDriverFactory::validate_locator_request decoded{};
        if (!decode_message(request, decoded, naos::system::TerminalDriverFactory::decode_validate_locator_request))
            return state.complete_reply(empty_bytes(), empty_resources(), EINVAL) ? NA_STATUS_OK
                                                                                  : NA_STATUS_PEER_CLOSED;
        naos::system::TerminalDriverFactory::validate_locator_response response{};
        response.valid = factory.validate_locator(decoded.locator.terminal_id, decoded.locator.generation,
                                                  decoded.locator.token.data(), decoded.locator.token.size())
                             ? 1
                             : 0;
        freelibcxx::vector<byte> encoded(memory::MemoryAllocatorV);
        if (!encode_message(encoded, response, naos::system::TerminalDriverFactory::encode_validate_locator_response))
            return NA_STATUS_RESOURCE_EXHAUSTED;
        return state.complete_reply(std::move(encoded), empty_resources()) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
    }
    return NA_STATUS_NOT_SUPPORTED;
}
na_status_t dispatch_kernel_view(capability::entry &target, invocation_state &state, u64 method_id,
                                 const freelibcxx::vector<byte> &request, capability::transfer_record_list &resources,
                                 task::process_t *caller, handle_t<invocation_state> &state_handle)
{
    state.mark_dispatched();
    if (auto *stream = target.object->get<dev::tty::console_stream>())
    {
        if (target.meta.scope != NA_SCOPE_STREAM)
            return NA_STATUS_WRONG_SCOPE;
        return publish_stream_call(state, *stream, method_id, request, resources, caller);
    }
    if (auto *stream = target.object->get<dev::tty::klog_stream>())
    {
        if (target.meta.scope != NA_SCOPE_STREAM)
            return NA_STATUS_WRONG_SCOPE;
        return publish_stream_call(state, *stream, method_id, request, resources, caller);
    }
    if (auto *process = target.object->get<task::process_object>())
    {
        if (target.meta.scope != NA_SCOPE_PROCESS)
            return NA_STATUS_WRONG_SCOPE;
        if (method_id == NA_METHOD_PROCESS_WAIT &&
            (target.meta.protocol_rights & static_cast<u64>(NA_PROCESS_RIGHT_WAIT)) == 0)
            return NA_STATUS_ACCESS_DENIED;
        if (method_id == NA_METHOD_PROCESS_WAIT_CHILDREN &&
            (target.meta.protocol_rights & static_cast<u64>(NA_PROCESS_RIGHT_WAIT)) == 0)
            return NA_STATUS_ACCESS_DENIED;
        if (method_id == NA_METHOD_PROCESS_GET_INFO &&
            (target.meta.protocol_rights & static_cast<u64>(NA_PROCESS_RIGHT_INSPECT)) == 0)
            return NA_STATUS_ACCESS_DENIED;
        if ((method_id == NA_METHOD_PROCESS_GET_JOB_CONTROL_INFO || method_id == NA_METHOD_PROCESS_GET_PROCESS_GROUP ||
             method_id == NA_METHOD_PROCESS_GET_SESSION) &&
            (target.meta.protocol_rights & static_cast<u64>(NA_PROCESS_RIGHT_INSPECT)) == 0)
            return NA_STATUS_ACCESS_DENIED;
        if ((method_id == NA_METHOD_PROCESS_SET_SESSION || method_id == NA_METHOD_PROCESS_SET_PROCESS_GROUP) &&
            (target.meta.protocol_rights & static_cast<u64>(NA_PROCESS_RIGHT_JOB_CONTROL)) == 0)
            return NA_STATUS_ACCESS_DENIED;
        if (method_id == NA_METHOD_PROCESS_START &&
            (target.meta.protocol_rights & static_cast<u64>(NA_PROCESS_RIGHT_START)) == 0)
            return NA_STATUS_ACCESS_DENIED;
        return publish_process_call(state, *process, method_id, request, caller);
    }
    if (auto *memory_object = target.object->get<data_plane::memory_object>())
    {
        if (target.meta.scope != NA_SCOPE_MEMORY_OBJECT)
            return NA_STATUS_WRONG_SCOPE;
        if (method_id == NA_METHOD_MEMORY_OBJECT_GET_INFO &&
            ((target.meta.meta_rights & NA_RIGHT_INSPECT) == 0 ||
             (target.meta.protocol_rights & NA_MEMORY_RIGHT_INFO) == 0))
            return NA_STATUS_ACCESS_DENIED;
        if (method_id == NA_METHOD_MEMORY_OBJECT_READ && (target.meta.protocol_rights & NA_MEMORY_RIGHT_READ) == 0)
            return NA_STATUS_ACCESS_DENIED;
        if (method_id == NA_METHOD_MEMORY_OBJECT_WRITE && (target.meta.protocol_rights & NA_MEMORY_RIGHT_WRITE) == 0)
            return NA_STATUS_ACCESS_DENIED;
        return publish_memory_object_call(state, *memory_object, target.meta, method_id, request, resources);
    }
    if (auto *framebuffer = target.object->get<dev::framebuffer::framebuffer_service>())
    {
        if (target.meta.scope != NA_SCOPE_FRAMEBUFFER)
            return NA_STATUS_WRONG_SCOPE;
        if (method_id == NA_METHOD_FRAMEBUFFER_GET && (target.meta.protocol_rights & NA_DISPLAY_RIGHT_WRITER) == 0)
            return NA_STATUS_ACCESS_DENIED;
        return publish_framebuffer_call(state, *framebuffer, method_id, request);
    }
    if (auto *directory = target.object->get<service::directory>())
    {
        if (target.meta.scope != NA_SCOPE_SERVICE_DIRECTORY)
            return NA_STATUS_WRONG_SCOPE;
        return publish_service_directory_call(state, *directory, method_id, request, resources,
                                              target.meta.protocol_rights, caller);
    }
    if (auto *source = target.object->get<dev::input::input_event_source>())
    {
        if (target.meta.scope != NA_SCOPE_INPUT_EVENT_SOURCE)
            return NA_STATUS_WRONG_SCOPE;
        return publish_input_event_source_call(state, *source, method_id, request);
    }
    if (auto *view = target.object->get<dev::tty::terminal_job_control>())
    {
        if (target.meta.scope != NA_SCOPE_TERMINAL_JOB_CONTROL)
            return NA_STATUS_WRONG_SCOPE;
        return publish_terminal_job_control_call(state, *view, method_id, request, caller);
    }
    if (auto *view = target.object->get<dev::tty::terminal_driver_control>())
    {
        if (target.meta.scope != NA_SCOPE_TERMINAL_DRIVER_CONTROL)
            return NA_STATUS_WRONG_SCOPE;
        return publish_terminal_driver_control_call(state, *view, method_id, request);
    }
    if (auto *factory = target.object->get<dev::tty::terminal_driver_factory>())
    {
        if (target.meta.scope != NA_SCOPE_TERMINAL_DRIVER_FACTORY)
            return NA_STATUS_WRONG_SCOPE;
        return publish_terminal_driver_factory_call(state, *factory, method_id, request);
    }
    return NA_STATUS_WRONG_BINDING;
}

void execute_kernel_dispatch(kernel_dispatch_request &request)
{
    auto &state = *request.state;
    auto *caller = request.caller ? request.caller->process() : nullptr;

    if (!state.begin_receive())
    {
        if (caller != nullptr)
            (void)caller->resource.restore_native_batch(request.resources);
        return;
    }

    if (caller == nullptr)
    {
        state.complete_reply(empty_bytes(), empty_resources(), ECHILD);
        return;
    }
    const auto status = dispatch_kernel_view(request.target, state, request.method_id, request.bytes, request.resources,
                                             caller, request.state);
    const auto restore_status = caller->resource.restore_native_batch(request.resources);
    if (restore_status != NA_STATUS_OK)
    {
        state.complete_reply(empty_bytes(), empty_resources(), EIO);
        return;
    }

    if (state.cancellation_requested())
    {
        state.complete_failure(NA_EXECUTION_OUTCOME_UNKNOWN, NA_OUTCOME_REASON_CANCEL_REQUESTED);
        return;
    }
    if (state.execution_interrupted())
        return;

    if (status == NA_STATUS_NOT_SUPPORTED)
    {
        state.complete_reply(empty_bytes(), empty_resources(), ENOTSUP);
    }
    else if (status == NA_STATUS_ACCESS_DENIED)
    {
        // Admission rights rejections must reach clients as EACCES
        // (VFS_BLOCK_DEVICE_ADR §6.1), not the generic EINVAL collapse.
        state.complete_reply(empty_bytes(), empty_resources(), EACCES);
    }
    else if (status != NA_STATUS_OK)
    {
        state.complete_reply(empty_bytes(), empty_resources(), EINVAL);
    }
}

void kernel_dispatch_worker(task::thread_start_info_t *)
{
    for (;;)
    {
        kernel_dispatcher->wait.do_wait([] { return kernel_dispatcher->pending.load(std::memory_order_acquire) != 0; });
        auto *request = kernel_dispatcher->pop();
        if (request == nullptr)
            continue;
        execute_kernel_dispatch(*request);
        memory::Delete<>(memory::KernelCommonAllocatorV, request);
    }
}

bool endpoint_is_client(capability::entry &entry, protocol_endpoint *&endpoint)
{
    if (entry.meta.binding != NA_BINDING_CLIENT_END || !entry.object)
        return false;
    endpoint = entry.object->get_unsafe<protocol_endpoint>();
    return endpoint != nullptr && endpoint->role() == endpoint_role::client;
}

bool endpoint_is_server(capability::entry &entry, protocol_endpoint *&endpoint)
{
    if (entry.meta.binding != NA_BINDING_SERVER_END || !entry.object)
        return false;
    endpoint = entry.object->get_unsafe<protocol_endpoint>();
    return endpoint != nullptr && endpoint->role() == endpoint_role::server;
}

bool descriptor_allows_method(const na_protocol_descriptor_t &descriptor, u64 method_id)
{
    if (method_id == 0 || method_id > descriptor.method_count || method_id > NA_PROTOCOL_MAX_METHOD_ID)
        return false;
    const u64 word = (method_id - 1) / 64;
    const u64 bit = (method_id - 1) % 64;
    return (descriptor.method_bitmap[word] & (1ULL << bit)) != 0;
}

bool descriptor_allows_oneway(const na_protocol_descriptor_t &descriptor, u64 method_id)
{
    if (!descriptor_allows_method(descriptor, method_id))
        return false;
    const u64 word = (method_id - 1) / 64;
    const u64 bit = (method_id - 1) % 64;
    return (descriptor.oneway_bitmap[word] & (1ULL << bit)) != 0;
}

bool descriptor_allows_method_rights(const na_protocol_descriptor_t &descriptor, u64 method_id, u64 rights)
{
    return descriptor_allows_method(descriptor, method_id) &&
           (rights & descriptor.method_rights[method_id - 1]) == descriptor.method_rights[method_id - 1];
}

na_status_t validate_descriptor(const na_protocol_descriptor_t &descriptor)
{
    bool uuid_is_zero = true;
    for (auto value : descriptor.uuid.bytes)
        uuid_is_zero = uuid_is_zero && value == 0;
    if (!valid_frame_size(descriptor.struct_size, sizeof(descriptor)) ||
        (descriptor.flags & ~NA_PROTOCOL_FLAG_ALLOW_ONEWAY) != 0 || descriptor.reserved0 != 0 ||
        descriptor.reserved1 != 0 || uuid_is_zero || descriptor.scope == NA_SCOPE_NONE ||
        descriptor.method_count == 0 || descriptor.method_count > NA_PROTOCOL_MAX_METHOD_ID ||
        descriptor.max_request_bytes > max_kernel_payload || descriptor.max_response_bytes > max_kernel_payload ||
        descriptor.max_resources > NA_CHANNEL_MAX_RESOURCES ||
        (descriptor.protocol_rights & NA_PROTOCOL_RIGHT_INVOKE) == 0 ||
        (descriptor.method_bitmap[(descriptor.method_count - 1) / 64] &
         (1ULL << ((descriptor.method_count - 1) % 64))) == 0)
        return NA_STATUS_INVALID_ARGUMENT;
    for (u64 method_id = descriptor.method_count + 1; method_id <= NA_PROTOCOL_MAX_METHOD_ID; method_id++)
    {
        const u64 word = (method_id - 1) / 64;
        const u64 bit = (method_id - 1) % 64;
        if ((descriptor.method_bitmap[word] & (1ULL << bit)) != 0)
            return NA_STATUS_INVALID_ARGUMENT;
        if ((descriptor.oneway_bitmap[word] & (1ULL << bit)) != 0)
            return NA_STATUS_INVALID_ARGUMENT;
        if (descriptor.method_rights[method_id - 1] != 0)
            return NA_STATUS_INVALID_ARGUMENT;
    }
    for (u64 method_id = 1; method_id <= descriptor.method_count; method_id++)
    {
        const u64 word = (method_id - 1) / 64;
        const u64 bit = (method_id - 1) % 64;
        if ((descriptor.oneway_bitmap[word] & (1ULL << bit)) != 0 &&
            (descriptor.method_bitmap[word] & (1ULL << bit)) == 0)
            return NA_STATUS_INVALID_ARGUMENT;
        if ((descriptor.oneway_bitmap[word] & (1ULL << bit)) != 0 &&
            (descriptor.flags & NA_PROTOCOL_FLAG_ALLOW_ONEWAY) == 0)
            return NA_STATUS_INVALID_ARGUMENT;
        if ((descriptor.method_bitmap[word] & (1ULL << bit)) != 0 &&
            (descriptor.method_rights[method_id - 1] == 0 ||
             (descriptor.method_rights[method_id - 1] & ~descriptor.protocol_rights) != 0))
            return NA_STATUS_INVALID_ARGUMENT;
    }
    return NA_STATUS_OK;
}

} // namespace

void init_kernel_dispatch_worker()
{
    if (kernel_dispatcher != nullptr)
        return;
    kernel_dispatcher = memory::New<kernel_dispatch_queue>(memory::KernelCommonAllocatorV);
    if (kernel_dispatcher == nullptr)
        KLOG_PANIC("Unable to allocate kernel invocation dispatcher");
    constexpr u64 worker_count = 4;
    for (u64 worker = 0; worker < worker_count; worker++)
    {
        if (task::create_kernel_process(kernel_dispatch_worker, nullptr, 0) == nullptr)
            KLOG_PANIC("Unable to create kernel invocation dispatcher");
    }
}

protocol_descriptor::protocol_descriptor(const na_protocol_descriptor_t &descriptor)
    : kobject(type_e::protocol_descriptor)
    , descriptor_(descriptor)
{
}

bool protocol_descriptor::matches(const na_uuid_t &uuid) const
{
    return memcmp(descriptor_.uuid.bytes, uuid.bytes, sizeof(uuid.bytes)) == 0;
}

protocol_endpoint::protocol_endpoint(handle_t<protocol_state> state, endpoint_role role)
    : kobject(type_of(role))
    , state_(std::move(state))
    , role_(role)
{
    if (state_)
        state_->endpoint_object_created(this);
}

protocol_endpoint::~protocol_endpoint()
{
    if (state_)
        state_->endpoint_object_destroyed(this);
    if (role_ == endpoint_role::server && state_)
        state_->close_server_queue();
}

void protocol_endpoint::on_capability_acquire(capability::location where)
{
    if (state_)
        state_->endpoint_acquired(role_, where);
}

void protocol_endpoint::on_capability_release(capability::location where)
{
    if (state_)
        state_->endpoint_released(role_, where);
}

void protocol_endpoint::on_capability_handoff(capability::location from, capability::location to)
{
    (void)to;
    if (state_)
        state_->endpoint_released(role_, from);
}

na_signal_t protocol_endpoint::capability_signals() const { return state_ ? state_->signals(role_) : 0; }

u64 protocol_endpoint::capability_state() const { return static_cast<u64>(role_); }

void protocol_endpoint::begin_operation()
{
    if (state_)
        state_->begin_operation();
}

void protocol_endpoint::end_operation()
{
    if (state_)
        state_->end_operation();
}

namespace
{
std::atomic<naos_ipc_domain_t *> invocation_core_domain{nullptr};
core_lock invocation_core_domain_lock;

void *invocation_control_allocate(void *, std::size_t size, std::size_t alignment)
{
    return memory::KernelCommonAllocatorV->allocate(size, alignment);
}

void invocation_control_deallocate(void *, void *pointer, std::size_t, std::size_t)
{
    if (pointer != nullptr)
        memory::KernelCommonAllocatorV->deallocate(pointer);
}

void *invocation_payload_allocate(void *, std::size_t size, std::size_t alignment)
{
    return memory::MemoryAllocatorV->allocate(size, alignment);
}

void invocation_payload_deallocate(void *, void *pointer, std::size_t, std::size_t)
{
    if (pointer != nullptr)
        memory::MemoryAllocatorV->deallocate(pointer);
}

std::uint64_t invocation_clock_now(void *) { return timer::get_high_resolution_time(); }

naos_ipc_allocator_t invocation_control_allocator{nullptr, invocation_control_allocate, invocation_control_deallocate};
naos_ipc_allocator_t invocation_payload_allocator{nullptr, invocation_payload_allocate, invocation_payload_deallocate};
naos_ipc_lock_t invocation_domain_lock_api{&invocation_core_domain_lock, core_lock_acquire, core_lock_release};

naos_ipc_domain_t *get_invocation_core_domain()
{
    auto *domain = invocation_core_domain.load(std::memory_order_acquire);
    if (domain != nullptr)
        return domain;

    naos_ipc_domain_config_t config{};
    config.memory = &invocation_control_allocator;
    config.synchronization = &invocation_domain_lock_api;
    config.max_messages = NA_CHANNEL_GLOBAL_MAX_MESSAGES;
    config.max_bytes = NA_CHANNEL_GLOBAL_MAX_BYTES;
    config.max_resources = NA_CHANNEL_GLOBAL_MAX_RESOURCES;
    auto *candidate = naos_ipc_domain_create(&config);
    if (candidate == nullptr)
        return nullptr;
    domain = nullptr;
    if (invocation_core_domain.compare_exchange_strong(domain, candidate, std::memory_order_release,
                                                       std::memory_order_acquire))
        return candidate;
    naos_ipc_domain_destroy(candidate);
    return domain;
}

void kernel_invocation_notify(void *context) noexcept
{
    static_cast<invocation_state *>(context)->notify_core_waiters();
}

void kernel_invocation_wake_execution(void *context) noexcept
{
    static_cast<invocation_state *>(context)->wake_core_execution();
}

int kernel_invocation_remove_queued(void *context) noexcept
{
    return static_cast<invocation_state *>(context)->remove_queued_from_core() ? 1 : 0;
}

struct invocation_resource_holder
{
    capability::transferred_resource resource;
};

void release_invocation_resource(void *, void *value) noexcept
{
    auto *holder = static_cast<invocation_resource_holder *>(value);
    if (holder != nullptr)
        memory::Delete<>(memory::KernelCommonAllocatorV, holder);
}

void clear_invocation_resource(naos_ipc_resource_t &resource)
{
    resource.context = nullptr;
    resource.value = nullptr;
    resource.release = nullptr;
}

bool make_invocation_resource_batch(capability::transfer_record_list &records,
                                    freelibcxx::vector<naos_ipc_resource_t> &resources)
{
    resources.ensure(records.size());
    if (records.size() != 0 && resources.data() == nullptr)
        return false;
    for (u64 index = 0; index < records.size(); index++)
    {
        if (!records[index].resource.valid())
        {
            resources.push_back(naos_ipc_resource_t{});
            continue;
        }
        auto *holder = memory::New<invocation_resource_holder>(memory::KernelCommonAllocatorV);
        if (holder == nullptr)
        {
            for (u64 previous_index = 0; previous_index < resources.size(); previous_index++)
            {
                auto *previous = static_cast<invocation_resource_holder *>(resources[previous_index].value);
                if (previous == nullptr)
                    continue;
                clear_invocation_resource(resources[previous_index]);
                records[previous_index].resource = std::move(previous->resource);
                memory::Delete<>(memory::KernelCommonAllocatorV, previous);
            }
            return false;
        }
        holder->resource = std::move(records[index].resource);
        resources.push_back(naos_ipc_resource_t{nullptr, holder, release_invocation_resource});
    }
    return true;
}

void restore_invocation_resource_batch(capability::transfer_record_list &records,
                                       freelibcxx::vector<naos_ipc_resource_t> &resources)
{
    for (u64 index = 0; index < resources.size(); index++)
    {
        auto *holder = static_cast<invocation_resource_holder *>(resources[index].value);
        if (holder == nullptr)
            continue;
        clear_invocation_resource(resources[index]);
        records[index].resource = std::move(holder->resource);
        memory::Delete<>(memory::KernelCommonAllocatorV, holder);
    }
}

} // namespace

invocation_state::invocation_state(u64 method_id, u64 operation_budget, u64 max_response_bytes,
                                   u64 max_response_resources)
    : lock_()
    , wait_queue_()
    , execution_wait_queue_(nullptr)
    , queue_owner_(nullptr)
    , core_lock_()
    , core_lock_api_{&core_lock_, core_lock_acquire, core_lock_release}
    , core_notifier_api_{this, kernel_invocation_notify}
    , core_clock_api_{nullptr, invocation_clock_now}
    , core_callbacks_{this, kernel_invocation_remove_queued, kernel_invocation_wake_execution}
    , core_(nullptr)
{
    auto *domain = get_invocation_core_domain();
    if (domain == nullptr)
        return;
    naos_ipc_invocation_config_t config{};
    config.owner_domain = domain;
    config.control_memory = &invocation_control_allocator;
    config.payload_memory = &invocation_payload_allocator;
    config.synchronization = &core_lock_api_;
    config.notifier = &core_notifier_api_;
    config.clock = &core_clock_api_;
    config.callbacks = &core_callbacks_;
    config.method_id = method_id;
    config.operation_deadline = calculate_deadline(operation_budget);
    config.max_response_bytes = max_response_bytes;
    config.max_response_resources = max_response_resources;
    core_ = naos_ipc_invocation_create(&config);
}

invocation_state::~invocation_state()
{
    if (core_ != nullptr)
        naos_ipc_invocation_destroy(core_);
    core_ = nullptr;
}

na_signal_t invocation_state::signals() const { return core_ == nullptr ? 0 : naos_ipc_invocation_signals(core_); }

u64 invocation_state::method_id() const { return core_ == nullptr ? 0 : naos_ipc_invocation_method_id(core_); }

u64 invocation_state::operation_deadline() const
{
    return core_ == nullptr ? 0 : naos_ipc_invocation_operation_deadline(core_);
}

bool invocation_state::begin_receive() { return core_ != nullptr && naos_ipc_invocation_begin_receive(core_) != 0; }

void invocation_state::rollback_receive()
{
    if (core_ != nullptr)
        naos_ipc_invocation_rollback_receive(core_);
}

bool invocation_state::finish_dispatch() { return core_ != nullptr && naos_ipc_invocation_finish_dispatch(core_) != 0; }

void invocation_state::set_queue_owner(const handle_t<protocol_state> &owner)
{
    uctx::RawSpinLockUninterruptibleContext guard(lock_);
    if (core_ != nullptr && naos_ipc_invocation_signals(core_) == 0)
        queue_owner_ = owner;
}

void invocation_state::clear_queue_owner()
{
    uctx::RawSpinLockUninterruptibleContext guard(lock_);
    queue_owner_.reset();
}

bool invocation_state::cancellation_requested() const
{
    return core_ != nullptr && naos_ipc_invocation_cancellation_requested(core_) != 0;
}

bool invocation_state::execution_interrupted() const
{
    return core_ != nullptr && naos_ipc_invocation_execution_interrupted(core_) != 0;
}

void invocation_state::set_execution_wait_queue(task::wait_queue_t *queue)
{
    uctx::RawSpinLockUninterruptibleContext guard(lock_);
    execution_wait_queue_ = queue;
}

void invocation_state::clear_execution_wait_queue(task::wait_queue_t *queue)
{
    uctx::RawSpinLockUninterruptibleContext guard(lock_);
    if (execution_wait_queue_ == queue)
        execution_wait_queue_ = nullptr;
}

void invocation_state::mark_dispatched()
{
    if (core_ != nullptr)
        naos_ipc_invocation_mark_dispatched(core_);
}

bool invocation_state::remove_queued_from_core()
{
    handle_t<protocol_state> owner;
    {
        uctx::RawSpinLockUninterruptibleContext guard(lock_);
        owner = queue_owner_;
    }
    if (!owner)
        return false;
    auto *request = owner->remove_queued(this);
    if (request == nullptr)
        return false;
    clear_queue_owner();
    memory::Delete<>(memory::KernelCommonAllocatorV, request);
    return true;
}

void invocation_state::notify_core_waiters()
{
    wait_queue_.do_wake_up();
    notify_readiness();
}

void invocation_state::notify_readiness()
{
    // Keep the state lock while dereferencing both registrations.  Their
    // destructors clear the same slots under this lock, so a copied pointer
    // used after unlock would be a use-after-free window.
    uctx::RawSpinLockUninterruptibleContext guard(lock_);
    if (invocation_object_ != nullptr)
        invocation_object_->notify_readiness();
    if (responder_object_ != nullptr)
        responder_object_->notify_readiness();
}

void invocation_state::invocation_object_created(invocation_object *object)
{
    uctx::RawSpinLockUninterruptibleContext guard(lock_);
    invocation_object_ = object;
}

void invocation_state::invocation_object_destroyed(invocation_object *object)
{
    uctx::RawSpinLockUninterruptibleContext guard(lock_);
    if (invocation_object_ == object)
        invocation_object_ = nullptr;
}

void invocation_state::responder_object_created(responder_object *object)
{
    uctx::RawSpinLockUninterruptibleContext guard(lock_);
    responder_object_ = object;
}

void invocation_state::responder_object_destroyed(responder_object *object)
{
    uctx::RawSpinLockUninterruptibleContext guard(lock_);
    if (responder_object_ == object)
        responder_object_ = nullptr;
}

void invocation_state::wake_core_execution()
{
    task::wait_queue_t *queue = nullptr;
    {
        uctx::RawSpinLockUninterruptibleContext guard(lock_);
        queue = execution_wait_queue_;
    }
    if (queue != nullptr)
        queue->do_wake_up();
}

bool invocation_state::cancel(protocol_state *queue_owner)
{
    (void)queue_owner;
    return core_ != nullptr && naos_ipc_invocation_cancel(core_) != 0;
}

na_status_t invocation_state::arm_deadline(const handle_t<invocation_state> &self)
{
    if (operation_deadline() == no_deadline)
        return NA_STATUS_OK;
    auto *watch = memory::New<deadline_watch>(memory::KernelCommonAllocatorV, self);
    if (watch == nullptr)
        return NA_STATUS_RESOURCE_EXHAUSTED;
    if (timer::schedule_at(operation_deadline(), timer::timer_handler::bind<&deadline_watch::invoke>(*watch)) ==
        timer::invalid_watcher_id)
    {
        memory::Delete<>(memory::KernelCommonAllocatorV, watch);
        expire_deadline();
    }
    return NA_STATUS_OK;
}

void invocation_state::expire_deadline()
{
    if (core_ != nullptr)
        naos_ipc_invocation_expire_if_due(core_);
}

void invocation_state::close_client()
{
    if (core_ != nullptr)
        naos_ipc_invocation_close_client(core_);
}

void invocation_state::abandon_responder()
{
    if (core_ != nullptr)
        naos_ipc_invocation_abandon_responder(core_);
}

bool invocation_state::consume_responder()
{
    return core_ != nullptr && naos_ipc_invocation_consume_responder(core_) != 0;
}

bool invocation_state::reserve_result_budget()
{
    return core_ != nullptr && naos_ipc_invocation_reserve_result_budget(core_) != 0;
}

bool invocation_state::complete_reply(freelibcxx::vector<byte> &bytes, capability::transfer_record_list &resources,
                                      i64 protocol_error, task::resource_table_t *source_resources)
{
    if (core_ == nullptr || !response_within_limits(bytes.size(), resources.size()))
        return false;
    freelibcxx::vector<naos_ipc_resource_t> native_resources(memory::KernelCommonAllocatorV);
    if (!make_invocation_resource_batch(resources, native_resources))
        return false;
    const int completed =
        naos_ipc_invocation_complete_reply(core_, reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size(),
                                           native_resources.data(), native_resources.size(), protocol_error);
    if (completed == 0)
    {
        restore_invocation_resource_batch(resources, native_resources);
        return false;
    }
    if (source_resources != nullptr)
        source_resources->commit_native_batch(resources);
    bytes.clear();
    resources.clear();
    return true;
}

bool invocation_state::complete_reply(freelibcxx::vector<byte> &&bytes, capability::transfer_record_list &&resources,
                                      i64 protocol_error, task::resource_table_t *source_resources)
{
    return complete_reply(bytes, resources, protocol_error, source_resources);
}

bool invocation_state::complete_failure(na_execution_outcome_t outcome, na_outcome_reason_t reason, i64 protocol_error)
{
    return core_ != nullptr && naos_ipc_invocation_complete_failure(core_, static_cast<u32>(outcome),
                                                                    static_cast<u32>(reason), protocol_error) != 0;
}

bool invocation_state::complete_not_delivered(na_outcome_reason_t reason)
{
    return core_ != nullptr && naos_ipc_invocation_complete_not_delivered(core_, static_cast<u32>(reason)) != 0;
}

bool invocation_state::deadline_expired(bool)
{
    if (core_ == nullptr)
        return false;
    naos_ipc_invocation_expire_if_due(core_);
    const auto state_signals = naos_ipc_invocation_signals(core_);
    return (state_signals & NA_SIGNAL_COMPLETED) != 0;
}

bool invocation_state::response_within_limits(u64 bytes, u64 resources) const
{
    return core_ != nullptr && naos_ipc_invocation_response_within_limits(core_, bytes, resources) != 0;
}

na_status_t invocation_state::claim_result(na_result_frame_t &frame, freelibcxx::vector<byte> &bytes,
                                           capability::transfer_record_list &records)
{
    if (core_ == nullptr)
        return NA_STATUS_RESOURCE_EXHAUSTED;
    naos_ipc_invocation_result_info_t info{};
    const auto claim_status = static_cast<na_status_t>(
        naos_ipc_invocation_claim_result(core_, frame.byte_capacity, frame.resource_capacity, &info));
    frame.method_id = info.method_id;
    frame.actual_bytes = info.actual_bytes;
    frame.actual_resources = info.actual_resources;
    frame.required_bytes = info.required_bytes;
    frame.required_resources = info.required_resources;
    frame.execution_outcome = info.execution_outcome;
    frame.outcome_reason = info.outcome_reason;
    frame.protocol_error = info.protocol_error;
    if (claim_status != NA_STATUS_OK)
        return claim_status;

    std::uint8_t *detached_bytes = nullptr;
    std::uint64_t detached_byte_count = 0;
    auto status =
        static_cast<na_status_t>(naos_ipc_invocation_take_result_bytes(core_, &detached_bytes, &detached_byte_count));
    if (status != NA_STATUS_OK)
        return status;
    if (detached_byte_count != 0)
    {
        bytes.resize(detached_byte_count, byte{});
        if (bytes.data() == nullptr)
        {
            naos_ipc_invocation_restore_result(core_, nullptr, 0, nullptr, 0);
            return NA_STATUS_RESOURCE_EXHAUSTED;
        }
        memcpy(bytes.data(), detached_bytes, detached_byte_count);
    }

    freelibcxx::vector<naos_ipc_resource_t> native_resources(memory::KernelCommonAllocatorV);
    native_resources.ensure(info.actual_resources);
    if (info.actual_resources != 0 && native_resources.data() == nullptr)
    {
        naos_ipc_invocation_restore_result(core_, nullptr, 0, nullptr, 0);
        bytes.clear();
        return NA_STATUS_RESOURCE_EXHAUSTED;
    }
    for (u64 index = 0; index < info.actual_resources; index++)
    {
        naos_ipc_resource_t resource{};
        status = static_cast<na_status_t>(naos_ipc_invocation_take_result_resource(core_, index, &resource));
        if (status != NA_STATUS_OK)
        {
            naos_ipc_invocation_restore_result(core_, nullptr, 0, native_resources.data(), native_resources.size());
            bytes.clear();
            return status;
        }
        native_resources.push_back(resource);
    }

    records.ensure(info.actual_resources);
    if (info.actual_resources != 0 && records.data() == nullptr)
    {
        naos_ipc_invocation_restore_result(core_, nullptr, 0, native_resources.data(), native_resources.size());
        bytes.clear();
        return NA_STATUS_RESOURCE_EXHAUSTED;
    }
    for (u64 index = 0; index < info.actual_resources; index++)
    {
        auto *holder = static_cast<invocation_resource_holder *>(native_resources[index].value);
        if (holder == nullptr)
        {
            naos_ipc_invocation_restore_result(core_, nullptr, 0, native_resources.data(), native_resources.size());
            bytes.clear();
            return NA_STATUS_IO_ERROR;
        }
        clear_invocation_resource(native_resources[index]);
        records.push_back(capability::transfer_record(NA_HANDLE_INVALID, false, std::move(holder->resource)));
        memory::Delete<>(memory::KernelCommonAllocatorV, holder);
    }
    return NA_STATUS_OK;
}

void invocation_state::restore_result(freelibcxx::vector<byte> &&bytes, capability::transfer_record_list &&resources)
{
    if (core_ == nullptr)
        return;
    freelibcxx::vector<naos_ipc_resource_t> native_resources(memory::KernelCommonAllocatorV);
    if (!make_invocation_resource_batch(resources, native_resources))
        return;
    const auto status =
        naos_ipc_invocation_restore_result(core_, nullptr, 0, native_resources.data(), native_resources.size());
    if (status != NA_STATUS_OK)
        restore_invocation_resource_batch(resources, native_resources);
    else
        resources.clear();
    bytes.clear();
}

na_status_t invocation_state::commit_result()
{
    return core_ == nullptr ? NA_STATUS_RESOURCE_EXHAUSTED
                            : static_cast<na_status_t>(naos_ipc_invocation_commit_result(core_));
}

invocation_object::invocation_object(handle_t<invocation_state> state)
    : kobject(type_e::invocation)
    , state_(std::move(state))
{
    if (state_)
        state_->invocation_object_created(this);
}

invocation_object::~invocation_object()
{
    if (state_)
        state_->invocation_object_destroyed(this);
    if (state_)
        state_->close_client();
}

void invocation_object::on_capability_release(capability::location where)
{
    if (where == capability::location::table_root && state_)
        state_->close_client();
}

na_signal_t invocation_object::capability_signals() const { return state_ ? state_->signals() : 0; }

u64 invocation_object::capability_state() const { return state_ ? static_cast<u64>(state_->signals()) : 0; }

responder_object::responder_object(handle_t<invocation_state> state)
    : kobject(type_e::responder)
    , state_(std::move(state))
    , consumed_(false)
{
    if (state_)
        state_->responder_object_created(this);
}

responder_object::~responder_object()
{
    if (state_)
        state_->responder_object_destroyed(this);
    if (!consumed_ && state_)
        state_->abandon_responder();
}

void responder_object::on_capability_release(capability::location where)
{
    if (where == capability::location::table_root && !consumed_ && state_)
        state_->abandon_responder();
}

na_signal_t responder_object::capability_signals() const { return state_ ? state_->signals() : 0; }

u64 responder_object::capability_state() const { return state_ ? static_cast<u64>(state_->signals()) : 0; }

protocol_state::protocol_state(const na_protocol_descriptor_t &descriptor, u64 max_messages, u64 max_bytes,
                               u64 max_resources)
    : descriptor_(descriptor)
    , queue_(nullptr)
    , max_messages_(max_messages)
    , max_bytes_(max_bytes)
    , max_resources_(max_resources)
    , owners_{0, 0}
    , roots_{0, 0}
    , active_operations_(0)
    , active_claims_(0)
    , valid_(false)
    , server_closed_(false)
{
    auto **storage = reinterpret_cast<invocation_request **>(memory::KernelCommonAllocatorV->allocate(
        sizeof(invocation_request *) * max_messages_, alignof(invocation_request *)));
    if (storage != nullptr)
        memset(storage, 0, sizeof(invocation_request *) * max_messages_);
    queue_ = memory::New<queue>(memory::KernelCommonAllocatorV, storage, max_messages_);
    valid_ = queue_ != nullptr && storage != nullptr;
}

protocol_state::~protocol_state()
{
    if (queue_ == nullptr)
        return;
    while (!queue_->fifo.empty())
    {
        if (!queue_->fifo.claim_front())
            break;
        auto *request = queue_->fifo.front();
        queue_->fifo.commit_claim();
        if (request != nullptr)
        {
            release_protocol_global(request->bytes.size(), request->queued_resource_count);
            request->state->clear_queue_owner();
            memory::Delete<>(memory::KernelCommonAllocatorV, request);
        }
    }
    if (queue_->storage != nullptr)
        memory::KernelCommonAllocatorV->deallocate(queue_->storage);
    memory::Delete<>(memory::KernelCommonAllocatorV, queue_);
}

na_signal_t protocol_state::signals(endpoint_role role) const
{
    if (!valid_ || queue_ == nullptr)
        return 0;
    uctx::RawSpinLockUninterruptibleContext guard(const_cast<lock::spinlock_t &>(lock_));
    const u8 index = role == endpoint_role::client ? 0 : 1;
    const u8 peer = 1 - index;
    na_signal_t result = 0;
    if (role == endpoint_role::server && !queue_->fifo.empty())
        result |= NA_SIGNAL_READABLE;
    if (role == endpoint_role::client && owners_[peer].load() != 0 && !queue_->fifo.full() &&
        queue_->bytes < max_bytes_ && queue_->resources < max_resources_)
        result |= NA_SIGNAL_WRITABLE;
    if (owners_[peer].load() == 0 || (role == endpoint_role::client && server_closed_))
        result |= NA_SIGNAL_PEER_CLOSED;
    return result;
}

void protocol_state::endpoint_object_created(protocol_endpoint *endpoint)
{
    if (endpoint == nullptr)
        return;
    uctx::RawSpinLockUninterruptibleContext guard(lock_);
    endpoints_[endpoint->role() == endpoint_role::client ? 0 : 1] = endpoint;
}

void protocol_state::endpoint_object_destroyed(protocol_endpoint *endpoint)
{
    if (endpoint == nullptr)
        return;
    uctx::RawSpinLockUninterruptibleContext guard(lock_);
    auto &slot = endpoints_[endpoint->role() == endpoint_role::client ? 0 : 1];
    if (slot == endpoint)
        slot = nullptr;
}

void protocol_state::notify_readiness()
{
    // Endpoint destruction clears endpoints_ while holding lock_.  Hold that
    // lock across the notification so the raw endpoint pointers cannot outlive
    // the registration that pins them.
    uctx::RawSpinLockUninterruptibleContext guard(lock_);
    for (auto *endpoint : endpoints_)
        if (endpoint != nullptr)
            endpoint->notify_readiness();
}

na_status_t protocol_state::enqueue(invocation_request *request, bool *queued)
{
    if (queued != nullptr)
        *queued = false;
    if (!valid_ || queue_ == nullptr || request == nullptr || request->bytes.size() > max_bytes_ ||
        request->resources.size() > max_resources_)
        return NA_STATUS_INVALID_MESSAGE;
    // Once the request is published, the peer may consume and destroy it
    // before this function finishes. Keep the invocation state alive locally
    // instead of reading it through the queued request after unlocking.
    auto state = request->state;
    na_status_t result = NA_STATUS_OK;
    {
        uctx::RawSpinLockUninterruptibleContext guard(lock_);
        if (owners_[1].load() == 0 || server_closed_)
            result = NA_STATUS_PEER_CLOSED;
        else
        {
            const u64 resource_count = request->resources.size() + (request->responder.valid() ? 1 : 0);
            if (resource_count > max_resources_)
                result = NA_STATUS_INVALID_MESSAGE;
            else if (queue_->fifo.full() || queue_->bytes > max_bytes_ - request->bytes.size() ||
                     queue_->resources > max_resources_ - resource_count)
                result = NA_STATUS_WOULD_BLOCK;
            else if (!reserve_protocol_global(request->bytes.size(), resource_count))
                result = NA_STATUS_RESOURCE_EXHAUSTED;
            else if (!queue_->fifo.try_push(request))
            {
                release_protocol_global(request->bytes.size(), resource_count);
                result = NA_STATUS_WOULD_BLOCK;
            }
            else
            {
                request->queued_resource_count = resource_count;
                queue_->bytes += request->bytes.size();
                queue_->resources += resource_count;
                if (request->source_resources != nullptr)
                {
                    auto *source_resources = request->source_resources;
                    request->source_resources = nullptr;
                    source_resources->commit_native_batch(request->resources);
                }
                if (queued != nullptr)
                    *queued = true;
            }
        }
    }
    if (result == NA_STATUS_OK)
    {
        if (state->cancellation_requested())
            state->cancel(this);
        if (queued != nullptr && *queued && (state->signals() & NA_SIGNAL_COMPLETED) != 0)
        {
            auto *removed = remove_queued(state.operator&());
            if (removed != nullptr)
            {
                removed->state->clear_queue_owner();
                memory::Delete<>(memory::KernelCommonAllocatorV, removed);
                *queued = false;
            }
        }
        notify_readiness();
    }
    return result;
}

invocation_request *protocol_state::remove_queued(invocation_state *state)
{
    if (!valid_ || queue_ == nullptr || state == nullptr)
        return nullptr;

    invocation_request *removed = nullptr;
    {
        uctx::RawSpinLockUninterruptibleContext guard(lock_);
        for (u64 i = 0; i < queue_->fifo.size(); i++)
        {
            auto *request = queue_->fifo.at(i);
            if (request == nullptr || request->state.operator&() != state)
                continue;
            if (!queue_->fifo.remove(request))
                return nullptr;
            queue_->bytes -= request->bytes.size();
            queue_->resources -= request->queued_resource_count;
            release_protocol_global(request->bytes.size(), request->queued_resource_count);
            removed = request;
            break;
        }
    }
    if (removed != nullptr)
    {
        notify_readiness();
    }
    return removed;
}

na_status_t protocol_state::claim(invocation_request *&request)
{
    request = nullptr;
    if (!valid_ || queue_ == nullptr)
        return NA_STATUS_RESOURCE_EXHAUSTED;
    uctx::RawSpinLockUninterruptibleContext guard(lock_);
    if (queue_->fifo.empty())
        return owners_[0].load() == 0 ? NA_STATUS_PEER_CLOSED : NA_STATUS_WOULD_BLOCK;
    if (!queue_->fifo.claim_front())
        return NA_STATUS_WOULD_BLOCK;
    request = queue_->fifo.front();
    active_claims_.fetch_add(1);
    return NA_STATUS_OK;
}

bool protocol_state::cancel_claim(invocation_request *request)
{
    if (!valid_ || queue_ == nullptr)
        return false;
    bool cancelled = false;
    {
        uctx::RawSpinLockUninterruptibleContext guard(lock_);
        if (!queue_->fifo.is_claimed() || queue_->fifo.front() != request)
            return false;
        queue_->fifo.cancel_claim();
        active_claims_.fetch_sub(1);
        cancelled = true;
    }
    if (cancelled)
    {
        notify_readiness();
    }
    return cancelled;
}

bool protocol_state::commit_claim(invocation_request *request)
{
    if (!valid_ || queue_ == nullptr)
        return false;
    bool committed = false;
    {
        uctx::RawSpinLockUninterruptibleContext guard(lock_);
        if (!queue_->fifo.is_claimed() || queue_->fifo.front() != request)
            return false;
        queue_->fifo.commit_claim();
        const u64 resource_count = request->queued_resource_count;
        queue_->bytes -= request->bytes.size();
        queue_->resources -= resource_count;
        active_claims_.fetch_sub(1);
        release_protocol_global(request->bytes.size(), resource_count);
        committed = true;
    }
    if (committed)
    {
        notify_readiness();
    }
    return committed;
}

bool protocol_state::abort_claim(invocation_request *request)
{
    if (!valid_ || queue_ == nullptr)
        return false;
    bool aborted = false;
    {
        uctx::RawSpinLockUninterruptibleContext guard(lock_);
        if (!queue_->fifo.is_claimed() || queue_->fifo.front() != request)
            return false;
        queue_->fifo.commit_claim();
        queue_->bytes -= request->bytes.size();
        queue_->resources -= request->queued_resource_count;
        active_claims_.fetch_sub(1);
        release_protocol_global(request->bytes.size(), request->queued_resource_count);
        aborted = true;
    }
    if (aborted)
    {
        notify_readiness();
    }
    return aborted;
}

void protocol_state::endpoint_acquired(endpoint_role role, capability::location where)
{
    const u8 index = role == endpoint_role::client ? 0 : 1;
    owners_[index].fetch_add(1);
    if (where == capability::location::table_root)
        roots_[index].fetch_add(1);
    notify_readiness();
}

void protocol_state::endpoint_released(endpoint_role role, capability::location where)
{
    const u8 index = role == endpoint_role::client ? 0 : 1;
    owners_[index].fetch_sub(1);
    if (where == capability::location::table_root)
        roots_[index].fetch_sub(1);
    if (role == endpoint_role::server && owners_[index].load() == 0)
        close_server_queue();
    notify_readiness();
}

void protocol_state::begin_operation() { active_operations_.fetch_add(1); }

void protocol_state::end_operation() { active_operations_.fetch_sub(1); }

void protocol_state::close_server_queue()
{
    invocation_request *discarded_head = nullptr;
    invocation_request *discarded_tail = nullptr;
    {
        uctx::RawSpinLockUninterruptibleContext guard(lock_);
        if (queue_ == nullptr)
            return;
        server_closed_ = true;
        while (!queue_->fifo.empty())
        {
            if (!queue_->fifo.claim_front())
                break;
            auto *request = queue_->fifo.front();
            queue_->fifo.commit_claim();
            if (request != nullptr)
            {
                queue_->bytes -= request->bytes.size();
                queue_->resources -= request->queued_resource_count;
                release_protocol_global(request->bytes.size(), request->queued_resource_count);
                request->discard_next = nullptr;
                if (discarded_tail != nullptr)
                    discarded_tail->discard_next = request;
                else
                    discarded_head = request;
                discarded_tail = request;
            }
        }
    }
    while (discarded_head != nullptr)
    {
        auto *request = discarded_head;
        discarded_head = request->discard_next;
        request->discard_next = nullptr;
        request->state->clear_queue_owner();
        request->state->complete_not_delivered(NA_OUTCOME_REASON_PEER_CLOSED);
        memory::Delete<>(memory::KernelCommonAllocatorV, request);
    }
    notify_readiness();
}

void protocol_state::protocol_violation() { close_server_queue(); }

na_status_t create_protocol_descriptor(task::resource_table_t &resources, const na_protocol_descriptor_t *input,
                                       na_handle_t *output)
{
    if (!valid_user_output(output))
        return NA_STATUS_FAULT;
    auto *descriptor = memory::New<na_protocol_descriptor_t>(memory::KernelCommonAllocatorV);
    if (descriptor == nullptr)
        return NA_STATUS_RESOURCE_EXHAUSTED;
    *descriptor = {};
    auto status = copy_frame(*descriptor, input);
    if (status != NA_STATUS_OK)
    {
        memory::Delete<>(memory::KernelCommonAllocatorV, descriptor);
        return status;
    }
    if (naos::usercopy::ranges_overlap(reinterpret_cast<u64>(input), sizeof(*input), reinterpret_cast<u64>(output),
                                       sizeof(*output)))
    {
        memory::Delete<>(memory::KernelCommonAllocatorV, descriptor);
        return NA_STATUS_INVALID_ARGUMENT;
    }
    status = validate_descriptor(*descriptor);
    if (status != NA_STATUS_OK)
    {
        memory::Delete<>(memory::KernelCommonAllocatorV, descriptor);
        return status;
    }
    auto object = handle_t<protocol_descriptor>::make(*descriptor);
    capability::metadata metadata;
    metadata.binding = NA_BINDING_NONE;
    metadata.protocol_uuid = descriptor->uuid;
    metadata.scope = descriptor->scope;
    metadata.revision = descriptor->revision;
    metadata.features = descriptor->features;
    metadata.meta_rights = NA_RIGHT_DUPLICATE | NA_RIGHT_TRANSFER | NA_RIGHT_INSPECT;
    metadata.protocol_rights = descriptor->protocol_rights;
    memory::Delete<>(memory::KernelCommonAllocatorV, descriptor);
    const na_handle_t handle = resources.install_native(std::move(object), metadata);
    if (handle == NA_HANDLE_INVALID)
        return NA_STATUS_RESOURCE_EXHAUSTED;
    const auto copy_status = naos::usercopy::copy_to(reinterpret_cast<u64>(output), &handle, sizeof(handle));
    if (copy_status != NA_STATUS_OK)
        resources.close_native(handle);
    return copy_status;
}

na_status_t create_protocol_endpoint_objects(const na_protocol_descriptor_t &descriptor,
                                             const na_protocol_endpoint_options_t *options, khandle &client,
                                             khandle &server, capability::metadata &client_metadata,
                                             capability::metadata &server_metadata)
{
    na_protocol_endpoint_options_t values{};
    values.struct_size = sizeof(values);
    values.client_meta_rights = NA_RIGHT_DUPLICATE | NA_RIGHT_TRANSFER | NA_RIGHT_WAIT | NA_RIGHT_INSPECT;
    values.server_meta_rights = NA_RIGHT_TRANSFER | NA_RIGHT_WAIT | NA_RIGHT_INSPECT;
    values.client_protocol_rights = descriptor.protocol_rights;
    values.server_protocol_rights = descriptor.protocol_rights;
    values.max_messages = 64;
    values.max_bytes =
        descriptor.max_request_bytes == 0 ? NA_CHANNEL_DEFAULT_MAX_BYTES : descriptor.max_request_bytes * 4;
    // max_resources is the queue's aggregate resource budget, while the
    // descriptor's max_resources limits a single request. Every queued
    // two-way invocation owns a responder resource, so using the latter as
    // the former makes an endpoint with one request resource accept only one
    // in-flight call. Keep enough budget for the endpoint's bounded queue.
    values.max_resources = NA_CHANNEL_MAX_RESOURCES;
    if (options != nullptr)
    {
        auto status = copy_frame(values, options);
        if (status != NA_STATUS_OK)
            return status;
        if (!valid_frame_size(values.struct_size, sizeof(values)) || values.flags != 0 || values.reserved0 != 0)
            return NA_STATUS_INVALID_ARGUMENT;
        if (values.max_messages == 0)
            values.max_messages = 64;
        if (values.max_bytes == 0)
            values.max_bytes = NA_CHANNEL_DEFAULT_MAX_BYTES;
        if (values.max_resources == 0)
            values.max_resources = NA_CHANNEL_MAX_RESOURCES;
    }
    // Terminal data endpoints are open descriptions, not freely duplicable
    // capabilities.  Cloning them must go through the protocol's
    // clone_binding method so the service can preserve pair and lifecycle
    // invariants.  Apply this after user options as well, so an endpoint
    // creator cannot opt back into the unsafe source-handle operation.
    if (descriptor.scope == NA_SCOPE_TERMINAL_MASTER || descriptor.scope == NA_SCOPE_TERMINAL_SLAVE)
        values.client_meta_rights &= ~NA_RIGHT_DUPLICATE;
    if (values.client_protocol_rights == 0)
        values.client_protocol_rights = descriptor.protocol_rights;
    if (values.server_protocol_rights == 0)
        values.server_protocol_rights = descriptor.protocol_rights;
    if ((values.client_protocol_rights & ~descriptor.protocol_rights) != 0 ||
        (values.server_protocol_rights & ~descriptor.protocol_rights) != 0)
        return NA_STATUS_ACCESS_DENIED;
    if (values.max_messages > NA_CHANNEL_MAX_MESSAGES || values.max_bytes > max_kernel_payload * 16 ||
        values.max_resources > NA_CHANNEL_MAX_RESOURCES ||
        (values.client_meta_rights &
         ~((na_meta_rights_t)(NA_RIGHT_DUPLICATE | NA_RIGHT_TRANSFER | NA_RIGHT_WAIT | NA_RIGHT_INSPECT))) != 0 ||
        (values.server_meta_rights & ~((na_meta_rights_t)(NA_RIGHT_TRANSFER | NA_RIGHT_WAIT | NA_RIGHT_INSPECT))) != 0)
        return NA_STATUS_INVALID_ARGUMENT;

    auto state =
        handle_t<protocol_state>::make(descriptor, values.max_messages, values.max_bytes, values.max_resources);
    if (!state || !state->valid() || state->descriptor().scope == NA_SCOPE_NONE)
        return NA_STATUS_RESOURCE_EXHAUSTED;
    auto client_object = handle_t<protocol_endpoint>::make(state, endpoint_role::client);
    auto server_object = handle_t<protocol_endpoint>::make(state, endpoint_role::server);
    if (!client_object || !server_object)
        return NA_STATUS_RESOURCE_EXHAUSTED;
    capability::metadata client_meta =
        kernel_view_metadata(descriptor.scope, descriptor.uuid, values.client_meta_rights,
                             values.client_protocol_rights, descriptor.revision, descriptor.features);
    client_meta.binding = NA_BINDING_CLIENT_END;
    capability::metadata server_meta = client_meta;
    server_meta.binding = NA_BINDING_SERVER_END;
    client_meta.protocol_rights |= NA_PROTOCOL_RIGHT_INVOKE;
    server_meta.protocol_rights = values.server_protocol_rights | NA_PROTOCOL_RIGHT_INVOKE;
    client = std::move(client_object);
    server = std::move(server_object);
    client_metadata = client_meta;
    server_metadata = server_meta;
    return NA_STATUS_OK;
}

na_status_t create_protocol_endpoint(task::resource_table_t &resources, na_handle_t descriptor_handle,
                                     const na_protocol_endpoint_options_t *options, na_handle_t *client,
                                     na_handle_t *server)
{
    if (!valid_user_output(client) || !valid_user_output(server))
        return NA_STATUS_FAULT;
    if (naos::usercopy::ranges_overlap(reinterpret_cast<u64>(client), sizeof(*client), reinterpret_cast<u64>(server),
                                       sizeof(*server)))
        return NA_STATUS_INVALID_ARGUMENT;
    capability::entry descriptor_entry;
    if (!resources.lookup_native(descriptor_handle, descriptor_entry) || !descriptor_entry.object)
        return NA_STATUS_INVALID_HANDLE;
    if ((descriptor_entry.meta.meta_rights & NA_RIGHT_INSPECT) == 0)
        return NA_STATUS_ACCESS_DENIED;
    auto *descriptor = descriptor_entry.object->get<protocol_descriptor>();
    if (descriptor == nullptr)
        return NA_STATUS_WRONG_BINDING;

    khandle client_object;
    khandle server_object;
    capability::metadata client_metadata;
    capability::metadata server_metadata;
    auto status = create_protocol_endpoint_objects(descriptor->descriptor(), options, client_object, server_object,
                                                   client_metadata, server_metadata);
    if (status != NA_STATUS_OK)
        return status;
    const na_handle_t client_handle = resources.install_native(std::move(client_object), client_metadata);
    if (client_handle == NA_HANDLE_INVALID)
        return NA_STATUS_RESOURCE_EXHAUSTED;
    const na_handle_t server_handle = resources.install_native(std::move(server_object), server_metadata);
    if (server_handle == NA_HANDLE_INVALID)
    {
        resources.close_native(client_handle);
        return NA_STATUS_RESOURCE_EXHAUSTED;
    }
    if (naos::usercopy::copy_to(reinterpret_cast<u64>(client), &client_handle, sizeof(client_handle)) != NA_STATUS_OK ||
        naos::usercopy::copy_to(reinterpret_cast<u64>(server), &server_handle, sizeof(server_handle)) != NA_STATUS_OK)
    {
        resources.close_native(client_handle);
        resources.close_native(server_handle);
        return NA_STATUS_FAULT;
    }
    return NA_STATUS_OK;
}

namespace
{
na_status_t invoke_submit_impl(task::resource_table_t &resources, na_handle_t target_handle,
                               const na_submit_frame_t *frame, na_handle_t *invocation, bool oneway,
                               bool invocation_output_is_user)
{
    na_submit_frame_t values{};
    auto status = copy_frame(values, frame);
    if (status != NA_STATUS_OK)
        return status;
    status = validate_submit_frame(values, oneway);
    if (status != NA_STATUS_OK)
        return status;
    if (!oneway && invocation_output_is_user && !valid_user_output(invocation))
        return NA_STATUS_FAULT;
    if (!oneway && !invocation_output_is_user && invocation == nullptr)
        return NA_STATUS_INVALID_ARGUMENT;
    if (oneway && invocation != nullptr)
        return NA_STATUS_INVALID_ARGUMENT;
    if (naos::usercopy::ranges_overlap(reinterpret_cast<u64>(frame), sizeof(*frame), values.request,
                                       values.request_bytes) ||
        naos::usercopy::ranges_overlap(reinterpret_cast<u64>(frame), sizeof(*frame), values.resources,
                                       values.resource_count * sizeof(na_resource_disposition_t)) ||
        naos::usercopy::ranges_overlap(values.request, values.request_bytes, values.resources,
                                       values.resource_count * sizeof(na_resource_disposition_t)))
        return NA_STATUS_INVALID_ARGUMENT;

    capability::entry target;
    if (!resources.lookup_native(target_handle, target) || !target.object)
        return NA_STATUS_INVALID_HANDLE;
    if ((target.meta.meta_rights & NA_RIGHT_TRANSFER) == 0 && values.resource_count != 0)
        return NA_STATUS_ACCESS_DENIED;

    freelibcxx::vector<byte> bytes(memory::MemoryAllocatorV);
    freelibcxx::vector<na_resource_disposition_t> dispositions(memory::KernelCommonAllocatorV);
    status = snapshot_request(values, bytes, dispositions);
    if (status != NA_STATUS_OK)
        return status;

    protocol_endpoint *client_endpoint = nullptr;
    const bool is_client = endpoint_is_client(target, client_endpoint);
    // Kernel-dispatchable targets are KernelViews and bare MemoryObjects:
    // the latter lets owners fill/read data-plane buffers via the generated
    // MemoryObject client (write right enforced by publish_memory_object_call).
    const bool is_kernel =
        target.meta.binding == NA_BINDING_KERNEL_VIEW ||
        (target.meta.binding == NA_BINDING_MEMORY_OBJECT && target.meta.scope == NA_SCOPE_MEMORY_OBJECT);
    if (!is_client && !is_kernel)
        return NA_STATUS_WRONG_BINDING;
    if ((target.meta.protocol_rights & NA_PROTOCOL_RIGHT_INVOKE) == 0)
        return NA_STATUS_ACCESS_DENIED;
    if (is_kernel && values.method_id == 0)
        return NA_STATUS_INVALID_ARGUMENT;
    if (is_client && (client_endpoint->state() == nullptr || !client_endpoint->state()->valid()))
        return NA_STATUS_OBJECT_REVOKED;
    if (is_client && !descriptor_allows_method(client_endpoint->state()->descriptor(), values.method_id))
        return NA_STATUS_INVALID_MESSAGE;
    if (is_client)
    {
        const auto &descriptor = client_endpoint->state()->descriptor();
        if (!descriptor_allows_method_rights(descriptor, values.method_id, target.meta.protocol_rights))
            return NA_STATUS_ACCESS_DENIED;
        if ((descriptor.max_request_bytes != 0 && values.request_bytes > descriptor.max_request_bytes) ||
            (descriptor.max_resources != 0 && values.resource_count > descriptor.max_resources))
            return NA_STATUS_INVALID_MESSAGE;
    }
    if (oneway)
    {
        if (!is_client || (client_endpoint->state()->descriptor().flags & NA_PROTOCOL_FLAG_ALLOW_ONEWAY) == 0 ||
            !descriptor_allows_oneway(client_endpoint->state()->descriptor(), values.method_id))
            return NA_STATUS_ACCESS_DENIED;
    }
    else if (is_client && descriptor_allows_oneway(client_endpoint->state()->descriptor(), values.method_id))
        return NA_STATUS_INVALID_ARGUMENT;

    // Allocate the Invocation entry before admission, but publish its opaque
    // value to user memory only after the request has been accepted.
    u64 max_response_bytes = max_kernel_payload;
    u64 max_response_resources = NA_CHANNEL_MAX_RESOURCES;
    if (is_client)
    {
        max_response_bytes = client_endpoint->state()->descriptor().max_response_bytes;
        max_response_resources = client_endpoint->state()->descriptor().max_resources;
    }
    handle_t<invocation_state> state = handle_t<invocation_state>::make(values.method_id, values.operation_budget,
                                                                        max_response_bytes, max_response_resources);
    if (!state)
        return NA_STATUS_RESOURCE_EXHAUSTED;
    if (!oneway && !state->reserve_result_budget())
        return NA_STATUS_RESOURCE_EXHAUSTED;
    status = state->arm_deadline(state);
    if (status != NA_STATUS_OK)
        return status;
    if (is_client)
        state->set_queue_owner(client_endpoint->state_ref());

    na_handle_t invocation_handle = NA_HANDLE_INVALID;
    capability::metadata invocation_metadata;
    invocation_metadata.binding = NA_BINDING_INVOCATION;
    invocation_metadata.protocol_uuid = target.meta.protocol_uuid;
    invocation_metadata.scope = target.meta.scope;
    invocation_metadata.revision = target.meta.revision;
    invocation_metadata.features = target.meta.features;
    invocation_metadata.meta_rights = NA_RIGHT_TRANSFER | NA_RIGHT_WAIT | NA_RIGHT_INSPECT;
    handle_t<invocation_object> invocation_object_handle;
    if (!oneway)
    {
        invocation_object_handle = handle_t<invocation_object>::make(state);
        if (!invocation_object_handle)
            return NA_STATUS_RESOURCE_EXHAUSTED;
        invocation_handle = resources.install_native(std::move(invocation_object_handle), invocation_metadata);
        if (invocation_handle == NA_HANDLE_INVALID)
            return NA_STATUS_RESOURCE_EXHAUSTED;
    }

    if (is_client)
    {
        auto *request = memory::New<invocation_request>(memory::KernelCommonAllocatorV, memory::MemoryAllocatorV, state,
                                                        values.method_id, state->operation_deadline(),
                                                        task::current_process()->pid);
        if (request == nullptr)
        {
            if (!oneway)
                resources.close_native(invocation_handle);
            return NA_STATUS_RESOURCE_EXHAUSTED;
        }
        if (!oneway)
        {
            auto responder = handle_t<responder_object>::make(state);
            if (!responder)
            {
                memory::Delete<>(memory::KernelCommonAllocatorV, request);
                resources.close_native(invocation_handle);
                return NA_STATUS_RESOURCE_EXHAUSTED;
            }
            capability::metadata responder_metadata = invocation_metadata;
            responder_metadata.binding = NA_BINDING_RESPONDER;
            responder_metadata.meta_rights = NA_RIGHT_TRANSFER | NA_RIGHT_WAIT | NA_RIGHT_INSPECT;
            request->responder = capability::transferred_resource(std::move(responder), responder_metadata);
        }
        capability::transfer_record_list records(memory::KernelCommonAllocatorV);
        status = resources.take_native_batch(dispositions.data(), dispositions.size(), target_handle, records);
        if (status != NA_STATUS_OK)
        {
            memory::Delete<>(memory::KernelCommonAllocatorV, request);
            if (!oneway)
                resources.close_native(invocation_handle);
            return status;
        }
        request->bytes = std::move(bytes);
        request->resources = std::move(records);
        request->source_resources = &resources;
        bool queued = false;
        status = client_endpoint->state()->enqueue(request, &queued);
        (void)queued;
        if (status != NA_STATUS_OK)
        {
            const auto restore_status = resources.restore_native_batch(request->resources);
            memory::Delete<>(memory::KernelCommonAllocatorV, request);
            if (!oneway)
                resources.close_native(invocation_handle);
            return restore_status == NA_STATUS_OK ? status : restore_status;
        }
        if (!oneway)
        {
            if (invocation_output_is_user)
                status = naos::usercopy::copy_to(reinterpret_cast<u64>(invocation), &invocation_handle,
                                                 sizeof(invocation_handle));
            else
            {
                *invocation = invocation_handle;
                status = NA_STATUS_OK;
            }
            if (status != NA_STATUS_OK)
            {
                // The request is already committed to the client endpoint's
                // queue at this point.  A failed user-output copy must cancel
                // that queued invocation before releasing its handle; merely
                // closing the invocation would leave a request that can still
                // be delivered without an owner.
                state->cancel(client_endpoint->state());
                resources.close_native(invocation_handle);
                return status;
            }
        }
        return NA_STATUS_OK;
    }

    if (kernel_dispatcher == nullptr)
    {
        if (!oneway)
            resources.close_native(invocation_handle);
        return NA_STATUS_RESOURCE_EXHAUSTED;
    }

    auto caller = handle_t<task::process_object>::make(task::current_process());
    if (!caller)
    {
        if (!oneway)
            resources.close_native(invocation_handle);
        return NA_STATUS_RESOURCE_EXHAUSTED;
    }

    capability::transfer_record_list records(memory::KernelCommonAllocatorV);
    status = resources.take_native_batch(dispositions.data(), dispositions.size(), target_handle, records);
    if (status != NA_STATUS_OK)
    {
        if (!oneway)
            resources.close_native(invocation_handle);
        return status;
    }

    auto *request = memory::New<kernel_dispatch_request>(memory::KernelCommonAllocatorV, state, std::move(target),
                                                         std::move(caller), values.method_id);
    if (request == nullptr)
    {
        const auto restore_status = resources.restore_native_batch(records);
        if (!oneway)
            resources.close_native(invocation_handle);
        return restore_status == NA_STATUS_OK ? NA_STATUS_RESOURCE_EXHAUSTED : restore_status;
    }
    request->bytes = std::move(bytes);
    request->resources = std::move(records);
    kernel_dispatcher->enqueue(request);

    if (!oneway)
    {
        if (invocation_output_is_user)
            status = naos::usercopy::copy_to(reinterpret_cast<u64>(invocation), &invocation_handle,
                                             sizeof(invocation_handle));
        else
        {
            *invocation = invocation_handle;
            status = NA_STATUS_OK;
        }
        if (status != NA_STATUS_OK)
        {
            state->cancel(nullptr);
            resources.close_native(invocation_handle);
            return status;
        }
    }
    return NA_STATUS_OK;
}
} // namespace

na_status_t invoke_submit(task::resource_table_t &resources, na_handle_t target_handle, const na_submit_frame_t *frame,
                          na_handle_t *invocation, bool oneway)
{
    return invoke_submit_impl(resources, target_handle, frame, invocation, oneway, true);
}

na_status_t receive_protocol(task::resource_table_t &resources, na_handle_t endpoint_handle,
                             na_channel_receive_frame_t *frame)
{
    na_channel_receive_frame_t values{};
    auto status = copy_frame(values, frame);
    if (status != NA_STATUS_OK)
        return status;
    if (!valid_frame_size(values.struct_size, sizeof(values)) || values.flags != 0 || values.caller_pid != 0)
        return NA_STATUS_INVALID_ARGUMENT;
    if (values.byte_capacity > max_kernel_payload || values.resource_capacity > NA_CHANNEL_MAX_RESOURCES)
        return NA_STATUS_INVALID_ARGUMENT;

    capability::entry endpoint_entry;
    if (!resources.lookup_native(endpoint_handle, endpoint_entry) || !endpoint_entry.object)
        return NA_STATUS_INVALID_HANDLE;
    protocol_endpoint *endpoint = nullptr;
    if (!endpoint_is_server(endpoint_entry, endpoint))
        return NA_STATUS_WRONG_BINDING;
    endpoint->begin_operation();
    auto finish = [&](na_status_t result) {
        endpoint->end_operation();
        return result;
    };

    for (;;)
    {
        invocation_request *request = nullptr;
        status = endpoint->state()->claim(request);
        if (status != NA_STATUS_OK)
            return finish(status);
        if (request->state->deadline_expired(false) || request->state->signals() & NA_SIGNAL_COMPLETED)
        {
            endpoint->state()->commit_claim(request);
            memory::Delete<>(memory::KernelCommonAllocatorV, request);
            continue;
        }
        const auto &descriptor = endpoint->state()->descriptor();
        const bool method_valid = descriptor_allows_method(descriptor, request->method_id);
        const bool request_size_valid =
            descriptor.max_request_bytes == 0 || request->bytes.size() <= descriptor.max_request_bytes;
        const bool resource_count_valid =
            descriptor.max_resources == 0 || request->resources.size() <= descriptor.max_resources;
        const bool oneway = descriptor_allows_oneway(descriptor, request->method_id);
        const bool responder_valid = request->responder.valid();
        if (!method_valid || !request_size_valid || !resource_count_valid || oneway == responder_valid)
        {
            request->state->complete_failure(NA_EXECUTION_NOT_DELIVERED, NA_OUTCOME_REASON_PROTOCOL_VIOLATION);
            endpoint->state()->commit_claim(request);
            memory::Delete<>(memory::KernelCommonAllocatorV, request);
            endpoint->state()->protocol_violation();
            return finish(NA_STATUS_INVALID_MESSAGE);
        }
        values.method_id = request->method_id;
        values.required_bytes = request->bytes.size();
        values.required_resources = request->resources.size();
        values.actual_bytes = 0;
        values.actual_resources = 0;
        values.caller_pid = request->caller_pid;
        values.responder = NA_HANDLE_INVALID;
        if (values.byte_capacity < request->bytes.size() || values.resource_capacity < request->resources.size())
        {
            const auto write_status = write_frame(frame, values);
            endpoint->state()->cancel_claim(request);
            return finish(write_status == NA_STATUS_OK ? NA_STATUS_BUFFER_TOO_SMALL : write_status);
        }
        if (!naos::usercopy::valid_output_range(values.bytes, values.byte_capacity) ||
            !naos::usercopy::valid_output_range(values.resources, values.resource_capacity * sizeof(na_handle_t)))
        {
            endpoint->state()->cancel_claim(request);
            return finish(NA_STATUS_FAULT);
        }
        if (naos::usercopy::ranges_overlap(reinterpret_cast<u64>(frame), sizeof(*frame), values.bytes,
                                           request->bytes.size()) ||
            naos::usercopy::ranges_overlap(reinterpret_cast<u64>(frame), sizeof(*frame), values.resources,
                                           request->resources.size() * sizeof(na_handle_t)) ||
            naos::usercopy::ranges_overlap(values.bytes, request->bytes.size(), values.resources,
                                           request->resources.size() * sizeof(na_handle_t)))
        {
            endpoint->state()->cancel_claim(request);
            return finish(NA_STATUS_INVALID_ARGUMENT);
        }

        // Claiming the queue head is not itself the dispatch linearization
        // point: a capacity/fault path must leave the request available. Once
        // this transition succeeds, cancellation can only be observed by the
        // handler and cannot publish NOT_DELIVERED behind the receiver's back.
        if (!request->state->begin_receive())
        {
            endpoint->state()->commit_claim(request);
            memory::Delete<>(memory::KernelCommonAllocatorV, request);
            continue;
        }
        auto rollback_receive = [&] {
            request->state->rollback_receive();
            endpoint->state()->cancel_claim(request);
        };

        freelibcxx::vector<na_handle_t> reserved(memory::KernelCommonAllocatorV);
        status = resources.reserve_native(reserved, request->resources.size() + (request->responder.valid() ? 1 : 0));
        if (status != NA_STATUS_OK)
        {
            rollback_receive();
            return finish(status);
        }
        status = naos::usercopy::copy_to(values.bytes, request->bytes.data(), request->bytes.size());
        if (status == NA_STATUS_OK && !request->resources.empty())
            status = naos::usercopy::copy_to(values.resources, reserved.data(),
                                             request->resources.size() * sizeof(na_handle_t));
        if (status != NA_STATUS_OK)
        {
            resources.rollback_native(reserved);
            rollback_receive();
            return finish(status);
        }
        values.actual_bytes = request->bytes.size();
        values.actual_resources = request->resources.size();
        values.required_bytes = 0;
        values.required_resources = 0;
        if (request->responder.valid())
            values.responder = reserved[reserved.size() - 1];
        status = write_frame(frame, values);
        if (status != NA_STATUS_OK)
        {
            resources.rollback_native(reserved);
            rollback_receive();
            return finish(status);
        }
        for (u64 i = 0; i < request->resources.size(); i++)
        {
            status = resources.activate_native(reserved[i], std::move(request->resources[i].resource));
            if (status != NA_STATUS_OK)
                break;
        }
        if (status == NA_STATUS_OK && request->responder.valid())
            status = resources.activate_native(reserved[reserved.size() - 1], std::move(request->responder));
        if (status != NA_STATUS_OK)
        {
            for (auto handle : reserved)
                resources.close_native(handle);
            resources.rollback_native(reserved);
            rollback_receive();
            return finish(status);
        }
        if (!request->state->finish_dispatch())
        {
            for (auto handle : reserved)
                resources.close_native(handle);
            endpoint->state()->abort_claim(request);
            memory::Delete<>(memory::KernelCommonAllocatorV, request);
            return finish(NA_STATUS_PEER_CLOSED);
        }
        if (!endpoint->state()->commit_claim(request))
        {
            for (auto handle : reserved)
                resources.close_native(handle);
            request->state->complete_failure(NA_EXECUTION_OUTCOME_UNKNOWN, NA_OUTCOME_REASON_BROKER_FAILURE);
            endpoint->state()->abort_claim(request);
            memory::Delete<>(memory::KernelCommonAllocatorV, request);
            return finish(NA_STATUS_WOULD_BLOCK);
        }
        memory::Delete<>(memory::KernelCommonAllocatorV, request);
        return finish(NA_STATUS_OK);
    }
}

na_status_t invocation_cancel(task::resource_table_t &resources, na_handle_t invocation_handle)
{
    capability::entry entry;
    if (!resources.lookup_native(invocation_handle, entry) || !entry.object)
        return NA_STATUS_INVALID_HANDLE;
    if (entry.meta.binding != NA_BINDING_INVOCATION)
        return NA_STATUS_WRONG_BINDING;
    auto *invocation = entry.object->get<invocation_object>();
    if (invocation == nullptr)
        return NA_STATUS_WRONG_BINDING;
    return invocation->state()->cancel(nullptr) ? NA_STATUS_OK : NA_STATUS_ALREADY_CONSUMED;
}

na_status_t invocation_take_result(task::resource_table_t &resources, na_handle_t invocation_handle,
                                   na_result_frame_t *frame)
{
    na_result_frame_t values{};
    auto status = copy_frame(values, frame);
    if (status != NA_STATUS_OK)
        return status;
    if (!valid_frame_size(values.struct_size, sizeof(values)) || values.flags != 0)
        return NA_STATUS_INVALID_ARGUMENT;
    if (values.byte_capacity > max_kernel_payload || values.resource_capacity > NA_CHANNEL_MAX_RESOURCES)
        return NA_STATUS_INVALID_ARGUMENT;

    capability::entry entry;
    if (!resources.lookup_native(invocation_handle, entry) || !entry.object)
        return NA_STATUS_INVALID_HANDLE;
    if (entry.meta.binding != NA_BINDING_INVOCATION)
        return NA_STATUS_WRONG_BINDING;
    auto *invocation = entry.object->get<invocation_object>();
    if (invocation == nullptr)
        return NA_STATUS_WRONG_BINDING;

    freelibcxx::vector<byte> bytes(memory::MemoryAllocatorV);
    capability::transfer_record_list records(memory::KernelCommonAllocatorV);
    status = invocation->state()->claim_result(values, bytes, records);
    if (status != NA_STATUS_OK)
    {
        write_frame(frame, values);
        return status;
    }
    if (!naos::usercopy::valid_output_range(values.bytes, values.byte_capacity) ||
        !naos::usercopy::valid_output_range(values.resources, values.resource_capacity * sizeof(na_handle_t)))
    {
        invocation->state()->restore_result(std::move(bytes), std::move(records));
        return NA_STATUS_FAULT;
    }
    if (naos::usercopy::ranges_overlap(reinterpret_cast<u64>(frame), sizeof(*frame), values.bytes, bytes.size()) ||
        naos::usercopy::ranges_overlap(reinterpret_cast<u64>(frame), sizeof(*frame), values.resources,
                                       records.size() * sizeof(na_handle_t)) ||
        naos::usercopy::ranges_overlap(values.bytes, bytes.size(), values.resources,
                                       records.size() * sizeof(na_handle_t)))
    {
        invocation->state()->restore_result(std::move(bytes), std::move(records));
        return NA_STATUS_INVALID_ARGUMENT;
    }

    freelibcxx::vector<na_handle_t> reserved(memory::KernelCommonAllocatorV);
    status = resources.reserve_native(reserved, records.size());
    if (status == NA_STATUS_OK)
        status = naos::usercopy::copy_to(values.bytes, bytes.data(), bytes.size());
    if (status == NA_STATUS_OK && !records.empty())
        status = naos::usercopy::copy_to(values.resources, reserved.data(), records.size() * sizeof(na_handle_t));
    values.actual_bytes = bytes.size();
    values.actual_resources = records.size();
    values.required_bytes = 0;
    values.required_resources = 0;
    if (status == NA_STATUS_OK)
        status = write_frame(frame, values);
    if (status != NA_STATUS_OK)
    {
        resources.rollback_native(reserved);
        invocation->state()->restore_result(std::move(bytes), std::move(records));
        return status;
    }
    for (u64 i = 0; i < records.size(); i++)
    {
        status = resources.activate_native(reserved[i], std::move(records[i].resource));
        if (status != NA_STATUS_OK)
            break;
    }
    if (status != NA_STATUS_OK)
    {
        for (auto handle : reserved)
            resources.close_native(handle);
        resources.rollback_native(reserved);
        invocation->state()->restore_result(std::move(bytes), std::move(records));
        return status;
    }
    status = invocation->state()->commit_result();
    if (status != NA_STATUS_OK)
    {
        for (auto handle : reserved)
            resources.close_native(handle);
        return status;
    }
    return NA_STATUS_OK;
}

na_status_t responder_reply(task::resource_table_t &resources, na_handle_t responder_handle,
                            const na_reply_frame_t *frame)
{
    na_reply_frame_t values{};
    auto status = copy_frame(values, frame);
    if (status != NA_STATUS_OK)
        return status;
    if (!valid_frame_size(values.struct_size, sizeof(values)) || values.flags != 0 || values.reserved0 != 0 ||
        values.reserved1 != 0 || values.byte_count > max_kernel_payload ||
        values.resource_count > NA_CHANNEL_MAX_RESOURCES)
        return NA_STATUS_INVALID_ARGUMENT;
    u64 resources_bytes = 0;
    if (!checked_multiply(values.resource_count, sizeof(na_resource_disposition_t), resources_bytes) ||
        !naos::usercopy::valid_range(values.bytes, values.byte_count) ||
        !naos::usercopy::valid_range(values.resources, resources_bytes))
        return NA_STATUS_FAULT;
    if (naos::usercopy::ranges_overlap(reinterpret_cast<u64>(frame), sizeof(*frame), values.bytes, values.byte_count) ||
        naos::usercopy::ranges_overlap(reinterpret_cast<u64>(frame), sizeof(*frame), values.resources,
                                       resources_bytes) ||
        naos::usercopy::ranges_overlap(values.bytes, values.byte_count, values.resources, resources_bytes))
        return NA_STATUS_INVALID_ARGUMENT;
    freelibcxx::vector<byte> bytes(memory::MemoryAllocatorV);
    freelibcxx::vector<na_resource_disposition_t> dispositions(memory::KernelCommonAllocatorV);
    bytes.resize(values.byte_count, byte{});
    if (values.byte_count != 0)
    {
        status = naos::usercopy::copy_from(bytes.data(), values.bytes, values.byte_count);
        if (status != NA_STATUS_OK)
            return status;
    }
    dispositions.resize(values.resource_count, na_resource_disposition_t{});
    if (values.resource_count != 0)
    {
        status = naos::usercopy::copy_from(dispositions.data(), values.resources, resources_bytes);
        if (status != NA_STATUS_OK)
            return status;
    }

    capability::entry entry;
    if (!resources.lookup_native(responder_handle, entry) || !entry.object)
        return NA_STATUS_INVALID_HANDLE;
    if (entry.meta.binding != NA_BINDING_RESPONDER)
        return NA_STATUS_WRONG_BINDING;
    auto *responder = entry.object->get<responder_object>();
    if (responder == nullptr)
        return NA_STATUS_WRONG_BINDING;
    if (!responder->state()->response_within_limits(bytes.size(), dispositions.size()))
        return NA_STATUS_INVALID_MESSAGE;
    capability::transfer_record_list records(memory::KernelCommonAllocatorV);
    status = resources.take_native_batch(dispositions.data(), dispositions.size(), responder_handle, records);
    if (status != NA_STATUS_OK)
        return status;
    const bool replied = responder->state()->complete_reply(bytes, records, 0, &resources);
    if (!replied)
    {
        const auto restore_status = resources.restore_native_batch(records);
        resources.close_native(responder_handle);
        return restore_status == NA_STATUS_OK ? NA_STATUS_PEER_CLOSED : restore_status;
    }
    responder->consume();
    resources.close_native(responder_handle);
    return NA_STATUS_OK;
}

na_status_t responder_fail(task::resource_table_t &resources, na_handle_t responder_handle,
                           const na_fail_frame_t *frame)
{
    na_fail_frame_t values{};
    auto status = copy_frame(values, frame);
    if (status != NA_STATUS_OK)
        return status;
    if (!valid_frame_size(values.struct_size, sizeof(values)) || values.flags != 0 ||
        naos_ipc_invocation_failure_valid(values.execution_outcome, values.outcome_reason, values.protocol_error) == 0)
        return NA_STATUS_INVALID_ARGUMENT;
    capability::entry entry;
    if (!resources.lookup_native(responder_handle, entry) || !entry.object)
        return NA_STATUS_INVALID_HANDLE;
    if (entry.meta.binding != NA_BINDING_RESPONDER)
        return NA_STATUS_WRONG_BINDING;
    auto *responder = entry.object->get<responder_object>();
    if (responder == nullptr)
        return NA_STATUS_WRONG_BINDING;
    if (!responder->state()->complete_failure(static_cast<na_execution_outcome_t>(values.execution_outcome),
                                              static_cast<na_outcome_reason_t>(values.outcome_reason),
                                              values.protocol_error))
    {
        resources.close_native(responder_handle);
        return NA_STATUS_PEER_CLOSED;
    }
    responder->consume();
    resources.close_native(responder_handle);
    return NA_STATUS_OK;
}

} // namespace naos::ipc
