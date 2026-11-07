#include "kernel/ipc/invocation.hpp"
#include "invocation_handlers.hpp"
#include "invocation_io.hpp"
#include "invocation_pci.hpp"
#include "invocation_services.hpp"
#include "invocation_support.hpp"
#include "kernel/ipc/channel.hpp"

#include "kernel/arch/klib.hpp"
#include "kernel/errno.hpp"
#include "kernel/log.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/task.hpp"
#include "kernel/ucontext.hpp"
#include "kernel/usercopy.hpp"
#include <limits>

KLOG_MODULE(ipc);
namespace naos::ipc
{
namespace
{
using dispatch_detail::empty_bytes;
using dispatch_detail::empty_resources;
using dispatch_detail::kernel_view_metadata;
using dispatch_detail::max_kernel_payload;

std::atomic_uint64_t global_protocol_messages{0};
std::atomic_uint64_t global_protocol_bytes{0};
std::atomic_uint64_t global_protocol_resources{0};

struct kernel_dispatch_request
{
    handle_t<invocation_state> state;
    capability::entry target;
    handle_t<task::process_object> caller;
    thread_id caller_tid = 0;
    u64 caller_io_lease_generation = 0;
    u64 method_id;
    freelibcxx::vector<byte> bytes;
    capability::transfer_record_list resources;
    kernel_dispatch_request *prev = nullptr;
    kernel_dispatch_request *next = nullptr;

    kernel_dispatch_request(handle_t<invocation_state> state, capability::entry target,
                            handle_t<task::process_object> caller, thread_id caller_tid, u64 caller_io_lease_generation,
                            u64 method_id)
        : state(std::move(state))
        , target(std::move(target))
        , caller(std::move(caller))
        , caller_tid(caller_tid)
        , caller_io_lease_generation(caller_io_lease_generation)
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

bool checked_multiply(u64 left, u64 right, u64 &result)
{
    if (left != 0 && right > std::numeric_limits<u64>::max() / left)
        return false;
    result = left * right;
    return true;
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
na_status_t dispatch_kernel_view(capability::entry &target, invocation_state &state, u64 method_id,
                                 const freelibcxx::vector<byte> &request, capability::transfer_record_list &resources,
                                 task::process_t *caller, thread_id caller_tid, u64 caller_io_lease_generation,
                                 handle_t<invocation_state> &)
{
    state.mark_dispatched();
    kernel_dispatch::kernel_view_context context{target,    state,  method_id,  request,
                                                 resources, caller, caller_tid, caller_io_lease_generation};
    const kernel_dispatch::kernel_view_route_table routes[] = {
        io_kernel_view_routes(),      process_kernel_view_routes(),
        service_kernel_view_routes(), firmware_root_kernel_view_routes(),
        acpi_kernel_view_routes(),    pci_kernel_view_routes(),
    };
    return kernel_dispatch::dispatch_kernel_view(context, routes);
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
    const auto status =
        dispatch_kernel_view(request.target, state, request.method_id, request.bytes, request.resources, caller,
                             request.caller_tid, request.caller_io_lease_generation, request.state);
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

void protocol_endpoint::acquire_kernel_reference()
{
    if (state_)
        state_->endpoint_acquired(role_, capability::location::in_transit);
}

void protocol_endpoint::release_kernel_reference()
{
    if (state_)
        state_->endpoint_released(role_, capability::location::in_transit);
}

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

handle_t<invocation_state> submit_kernel_invocation(const khandle &target, u64 method_id,
                                                    freelibcxx::vector<byte> &&bytes,
                                                    capability::transfer_record_list &&resources)
{
    if (!target)
        return {};
    if (target->get_ktype() != protocol_endpoint::type_of(endpoint_role::client))
        return {};
    auto *endpoint = const_cast<protocol_endpoint *>(target->get_unsafe<protocol_endpoint>());
    if (endpoint == nullptr || endpoint->role() != endpoint_role::client || endpoint->state() == nullptr ||
        !endpoint->state()->valid())
    {
        return {};
    }

    endpoint->begin_operation();
    struct operation_guard
    {
        protocol_endpoint *endpoint;
        ~operation_guard() { endpoint->end_operation(); }
    } guard{endpoint};

    const auto &descriptor = endpoint->state()->descriptor();
    if (!kernel_dispatch::descriptor_allows_method(descriptor, method_id) ||
        kernel_dispatch::descriptor_allows_oneway(descriptor, method_id) ||
        (descriptor.max_request_bytes != 0 && bytes.size() > descriptor.max_request_bytes) ||
        (descriptor.max_resources != 0 && resources.size() > descriptor.max_resources))
    {
        return {};
    }

    auto state =
        handle_t<invocation_state>::make(method_id, 0, descriptor.max_response_bytes, descriptor.max_resources);
    if (!state || !state->reserve_result_budget() || state->arm_deadline(state) != NA_STATUS_OK)
    {
        return {};
    }
    state->set_queue_owner(endpoint->state_ref());

    auto *request = memory::New<invocation_request>(memory::KernelCommonAllocatorV, memory::MemoryAllocatorV, state,
                                                    method_id, 0, 0);
    if (request == nullptr)
    {
        return {};
    }
    request->bytes = std::move(bytes);
    request->resources = std::move(resources);

    auto responder = handle_t<responder_object>::make(state);
    if (!responder)
    {
        memory::Delete<>(memory::KernelCommonAllocatorV, request);
        return {};
    }
    capability::metadata responder_metadata;
    responder_metadata.binding = NA_BINDING_RESPONDER;
    responder_metadata.protocol_uuid = descriptor.uuid;
    responder_metadata.scope = descriptor.scope;
    responder_metadata.revision = descriptor.revision;
    responder_metadata.features = descriptor.features;
    responder_metadata.meta_rights = NA_RIGHT_TRANSFER | NA_RIGHT_WAIT | NA_RIGHT_INSPECT;
    request->responder = capability::transferred_resource(std::move(responder), responder_metadata);

    bool queued = false;
    const auto status = endpoint->state()->enqueue(request, &queued);
    if (status != NA_STATUS_OK || !queued)
    {
        memory::Delete<>(memory::KernelCommonAllocatorV, request);
        return {};
    }
    return state;
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

    // TRANSFER on the target governs delegation of the target capability; it
    // must not gate request arguments. Each argument's own TRANSFER right is
    // checked atomically by snapshot_request/prepare_batch, then the target's
    // protocol contract validates the method-specific resource set.

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
    if (is_client &&
        !kernel_dispatch::descriptor_allows_method(client_endpoint->state()->descriptor(), values.method_id))
        return NA_STATUS_INVALID_MESSAGE;
    if (is_client)
    {
        const auto &descriptor = client_endpoint->state()->descriptor();
        if (!kernel_dispatch::descriptor_allows_method_rights(descriptor, values.method_id,
                                                              target.meta.protocol_rights))
            return NA_STATUS_ACCESS_DENIED;
        if ((descriptor.max_request_bytes != 0 && values.request_bytes > descriptor.max_request_bytes) ||
            (descriptor.max_resources != 0 && values.resource_count > descriptor.max_resources))
            return NA_STATUS_INVALID_MESSAGE;
    }
    if (oneway)
    {
        if (!is_client || (client_endpoint->state()->descriptor().flags & NA_PROTOCOL_FLAG_ALLOW_ONEWAY) == 0 ||
            !kernel_dispatch::descriptor_allows_oneway(client_endpoint->state()->descriptor(), values.method_id))
            return NA_STATUS_ACCESS_DENIED;
    }
    else if (is_client &&
             kernel_dispatch::descriptor_allows_oneway(client_endpoint->state()->descriptor(), values.method_id))
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

    auto *current_thread = task::current();
    auto *request = memory::New<kernel_dispatch_request>(
        memory::KernelCommonAllocatorV, state, std::move(target), std::move(caller), current_thread->tid,
        task::direct_io_lease_generation(current_thread), values.method_id);
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
        const bool method_valid = kernel_dispatch::descriptor_allows_method(descriptor, request->method_id);
        const bool request_size_valid =
            descriptor.max_request_bytes == 0 || request->bytes.size() <= descriptor.max_request_bytes;
        const bool resource_count_valid =
            descriptor.max_resources == 0 || request->resources.size() <= descriptor.max_resources;
        const bool oneway = kernel_dispatch::descriptor_allows_oneway(descriptor, request->method_id);
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
