#include "kernel/ipc/channel.hpp"

#include "kernel/arch/klib.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/timer.hpp"
#include "kernel/ucontext.hpp"
#include "kernel/usercopy.hpp"
#include <limits>

namespace naos::ipc
{
namespace
{
lock::spinlock_t registry_lock;
freelibcxx::linked_list<channel_state *> *registry = nullptr;
lock::spinlock_t waiters_lock;
task::wait_queue_t *waiters = nullptr;

std::atomic_uint64_t global_messages{0};
std::atomic_uint64_t global_bytes{0};
std::atomic_uint64_t global_resources{0};

bool reserve_counter(std::atomic_uint64_t &counter, u64 amount, u64 limit)
{
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
}

void release_counter(std::atomic_uint64_t &counter, u64 amount)
{
    counter.fetch_sub(amount, std::memory_order_acq_rel);
}

bool reserve_global(u64 bytes, u64 resources)
{
    if (!reserve_counter(global_messages, 1, NA_CHANNEL_GLOBAL_MAX_MESSAGES))
        return false;
    if (!reserve_counter(global_bytes, bytes, NA_CHANNEL_GLOBAL_MAX_BYTES))
    {
        release_counter(global_messages, 1);
        return false;
    }
    if (!reserve_counter(global_resources, resources, NA_CHANNEL_GLOBAL_MAX_RESOURCES))
    {
        release_counter(global_bytes, bytes);
        release_counter(global_messages, 1);
        return false;
    }
    return true;
}

void release_global(u64 bytes, u64 resources)
{
    release_counter(global_messages, 1);
    release_counter(global_bytes, bytes);
    release_counter(global_resources, resources);
}

channel_message **allocate_queue_storage(u64 capacity)
{
    auto **storage = reinterpret_cast<channel_message **>(
        memory::KernelCommonAllocatorV->allocate(sizeof(channel_message *) * capacity, alignof(channel_message *)));
    if (storage != nullptr)
        memset(storage, 0, sizeof(channel_message *) * capacity);
    return storage;
}

bool register_state(channel_state *state)
{
    uctx::RawSpinLockUninterruptibleContext icu(registry_lock);
    if (registry == nullptr)
    {
        registry = memory::New<freelibcxx::linked_list<channel_state *>>(memory::KernelCommonAllocatorV,
                                                                         memory::KernelCommonAllocatorV);
        if (registry == nullptr)
            return false;
    }
    registry->push_back(state);
    return true;
}

void unregister_state(channel_state *state)
{
    uctx::RawSpinLockUninterruptibleContext icu(registry_lock);
    if (registry == nullptr)
        return;
    auto iterator = registry->begin();
    while (iterator != registry->end() && *iterator != state)
        ++iterator;
    if (iterator != registry->end())
        registry->remove(iterator);
}

void ensure_waiters()
{
    uctx::RawSpinLockUninterruptibleContext icu(waiters_lock);
    if (waiters == nullptr)
        waiters = memory::New<task::wait_queue_t>(memory::KernelCommonAllocatorV);
}

na_status_t copy_from_user(void *destination, u64 source, u64 size)
{
    return naos::usercopy::copy_from(destination, source, size);
}

na_status_t copy_to_user(u64 destination, const void *source, u64 size)
{
    return naos::usercopy::copy_to(destination, source, size);
}

bool valid_struct_size(u32 actual, u64 expected) { return actual >= expected; }

struct wait_request
{
    task::resource_table_t *resources;
    freelibcxx::vector<na_wait_item_t> *items;
};

bool wait_condition(wait_request *request)
{
    for (auto &item : *request->items)
    {
        item.observed = request->resources->native_signals(item.handle);
        if ((item.observed & item.signals) != 0)
            return true;
    }
    return false;
}

void wait_deadline_wakeup(timeclock::microsecond_t) noexcept
{
    if (waiters != nullptr)
        waiters->do_wake_up();
}

bool contains_state(const freelibcxx::vector<channel_state *> &states, channel_state *needle, u64 *index = nullptr)
{
    for (u64 i = 0; i < states.size(); i++)
    {
        if (states.data()[i] == needle)
        {
            if (index != nullptr)
                *index = i;
            return true;
        }
    }
    return false;
}
} // namespace

channel_message::channel_message(u64 byte_count, u64 resource_count)
    : bytes_(nullptr)
    , byte_count_(byte_count)
    , resource_capacity_(resource_count)
    , resources_(memory::KernelCommonAllocatorV)
{
    if (byte_count != 0)
    {
        bytes_ = reinterpret_cast<byte *>(memory::MemoryAllocatorV->allocate(byte_count, alignof(byte)));
        if (bytes_ != nullptr)
            memset(bytes_, 0, byte_count);
    }
    resources_.ensure(resource_count);
}

channel_message::~channel_message()
{
    if (bytes_ != nullptr)
        memory::MemoryAllocatorV->deallocate(bytes_);
}

bool channel_message::valid() const
{
    return (byte_count_ == 0 || bytes_ != nullptr) && (resource_capacity_ == 0 || resources_.data() != nullptr);
}

bool channel_message::append(capability::transferred_resource &&resource)
{
    if (resources_.size() >= resource_capacity_)
        return false;
    resources_.push_back(std::move(resource));
    return true;
}

channel_state::queue::~queue()
{
    if (storage != nullptr)
        memory::KernelCommonAllocatorV->deallocate(storage);
}

channel_state::channel_state(u64 max_messages, u64 max_bytes, u64 max_resources)
    : queues_{nullptr, nullptr}
    , max_messages_(max_messages)
    , max_bytes_(max_bytes)
    , max_resources_(max_resources)
    , owners_{0, 0}
    , roots_{0, 0}
    , active_operations_(0)
    , active_claims_(0)
    , endpoint_objects_(0)
    , valid_(false)
{
    queues_[0] =
        memory::New<queue>(memory::KernelCommonAllocatorV, allocate_queue_storage(max_messages_), max_messages_);
    queues_[1] =
        memory::New<queue>(memory::KernelCommonAllocatorV, allocate_queue_storage(max_messages_), max_messages_);
    valid_ = queues_[0] != nullptr && queues_[1] != nullptr && queues_[0]->storage != nullptr &&
             queues_[1]->storage != nullptr;
    if (valid_)
        valid_ = register_state(this);
}

channel_state::~channel_state()
{
    for (auto *queue : queues_)
    {
        if (queue == nullptr)
            continue;
        while (!queue->fifo.empty())
        {
            queue->fifo.claim_front();
            auto *message = queue->fifo.front();
            queue->fifo.commit_claim();
            if (message != nullptr)
            {
                release_global(message->byte_count(), message->resource_count());
                memory::Delete<>(memory::KernelCommonAllocatorV, message);
            }
        }
        memory::Delete<>(memory::KernelCommonAllocatorV, queue);
    }
}

na_signal_t channel_state::signals(u8 side) const
{
    if (!valid_ || side > 1)
        return 0;
    auto &lock = const_cast<lock::spinlock_t &>(lock_);
    uctx::RawSpinLockUninterruptibleContext icu(lock);
    const auto &queue = *queues_[side];
    const auto &send_queue = *queues_[1 - side];
    const u8 peer = 1 - side;
    na_signal_t result = 0;
    if (!queue.fifo.empty())
        result |= NA_SIGNAL_READABLE;
    if (owners_[peer].load(std::memory_order_acquire) != 0 && !send_queue.fifo.full() &&
        send_queue.bytes < max_bytes_ && send_queue.resources < max_resources_)
        result |= NA_SIGNAL_WRITABLE;
    if (owners_[peer].load(std::memory_order_acquire) == 0)
        result |= NA_SIGNAL_PEER_CLOSED;
    return result;
}

na_status_t channel_state::enqueue(u8 sender, channel_message *message, capability::transfer_record_list &records,
                                   task::resource_table_t &resources)
{
    if (!valid_ || sender > 1 || message == nullptr || !message->valid() || records.size() > max_resources_ ||
        message->resource_count() != 0 || message->resource_capacity() < records.size())
        return NA_STATUS_INVALID_ARGUMENT;
    if (message->byte_count() > max_bytes_ || message->resource_count() + records.size() > max_resources_)
        return NA_STATUS_INVALID_MESSAGE;

    const u8 receiver = 1 - sender;
    na_status_t result = NA_STATUS_OK;
    bool restore = false;
    {
        uctx::RawSpinLockUninterruptibleContext icu(lock_);
        auto &queue = *queues_[receiver];
        if (owners_[receiver].load(std::memory_order_acquire) == 0)
        {
            result = NA_STATUS_PEER_CLOSED;
            restore = true;
        }
        else if (queue.fifo.full() || queue.bytes > max_bytes_ - message->byte_count() ||
                 queue.resources > max_resources_ - records.size())
        {
            result = NA_STATUS_WOULD_BLOCK;
            restore = true;
        }
        else if (!reserve_global(message->byte_count(), records.size()))
        {
            result = NA_STATUS_RESOURCE_EXHAUSTED;
            restore = true;
        }
        else
        {
            for (auto &record : records)
            {
                if (!message->append(std::move(record.resource)))
                {
                    result = NA_STATUS_RESOURCE_EXHAUSTED;
                    restore = true;
                    for (u64 i = 0; i < message->resource_count(); i++)
                    {
                        if (i < records.size() && !records[i].resource.valid())
                            records[i].resource = std::move(message->resource(i));
                    }
                    release_global(message->byte_count(), records.size());
                    break;
                }
            }
            if (result == NA_STATUS_OK && !queue.fifo.try_push(message))
            {
                result = NA_STATUS_WOULD_BLOCK;
                restore = true;
                for (u64 i = 0; i < records.size(); i++)
                    records[i].resource = std::move(message->resource(i));
                release_global(message->byte_count(), records.size());
            }
            if (result == NA_STATUS_OK)
            {
                queue.bytes += message->byte_count();
                queue.resources += message->resource_count();
            }
        }
    }
    if (restore)
    {
        resources.restore_native_batch(records);
    }
    if (result == NA_STATUS_OK)
        notify_channel_waiters();
    return result;
}

na_status_t channel_state::claim_receive(u8 side, channel_message *&message)
{
    message = nullptr;
    if (!valid_ || side > 1)
        return NA_STATUS_INVALID_ARGUMENT;
    uctx::RawSpinLockUninterruptibleContext icu(lock_);
    auto &queue = *queues_[side];
    if (queue.fifo.empty())
        return owners_[1 - side].load(std::memory_order_acquire) == 0 ? NA_STATUS_PEER_CLOSED : NA_STATUS_WOULD_BLOCK;
    if (!queue.fifo.claim_front())
        return NA_STATUS_WOULD_BLOCK;
    message = queue.fifo.front();
    active_claims_.fetch_add(1, std::memory_order_acq_rel);
    return NA_STATUS_OK;
}

bool channel_state::cancel_receive(u8 side, channel_message *message)
{
    if (!valid_ || side > 1 || message == nullptr)
        return false;
    bool cancelled = false;
    {
        uctx::RawSpinLockUninterruptibleContext icu(lock_);
        auto &queue = *queues_[side];
        if (!queue.fifo.is_claimed() || queue.fifo.front() != message)
            return false;
        queue.fifo.cancel_claim();
        active_claims_.fetch_sub(1, std::memory_order_acq_rel);
        cancelled = true;
    }
    if (cancelled)
        notify_channel_waiters();
    return cancelled;
}

bool channel_state::commit_receive(u8 side, channel_message *message)
{
    if (!valid_ || side > 1 || message == nullptr)
        return false;
    bool committed = false;
    {
        uctx::RawSpinLockUninterruptibleContext icu(lock_);
        auto &queue = *queues_[side];
        if (!queue.fifo.is_claimed() || queue.fifo.front() != message)
            return false;
        queue.fifo.commit_claim();
        queue.bytes -= message->byte_count();
        queue.resources -= message->resource_count();
        active_claims_.fetch_sub(1, std::memory_order_acq_rel);
        release_global(message->byte_count(), message->resource_count());
        committed = true;
    }
    if (committed)
        notify_channel_waiters();
    return committed;
}

bool channel_state::discard(u8 side, channel_message *&message)
{
    message = nullptr;
    if (!valid_ || side > 1)
        return false;
    {
        uctx::RawSpinLockUninterruptibleContext icu(lock_);
        auto &queue = *queues_[side];
        if (queue.fifo.empty() || !queue.fifo.claim_front())
            return false;
        message = queue.fifo.front();
        queue.fifo.commit_claim();
        queue.bytes -= message->byte_count();
        queue.resources -= message->resource_count();
        release_global(message->byte_count(), message->resource_count());
    }
    notify_channel_waiters();
    return true;
}

void channel_state::endpoint_object_created() { endpoint_objects_.fetch_add(1, std::memory_order_acq_rel); }

void channel_state::endpoint_object_destroyed() { endpoint_objects_.fetch_sub(1, std::memory_order_acq_rel); }

void channel_state::capability_acquired(u8 side, capability::location where)
{
    if (side > 1)
        return;
    owners_[side].fetch_add(1, std::memory_order_acq_rel);
    if (where == capability::location::table_root)
        roots_[side].fetch_add(1, std::memory_order_acq_rel);
}

void channel_state::capability_released(u8 side, capability::location where)
{
    if (side > 1)
        return;
    owners_[side].fetch_sub(1, std::memory_order_acq_rel);
    if (where == capability::location::table_root)
        roots_[side].fetch_sub(1, std::memory_order_acq_rel);
}

void channel_state::begin_operation() { active_operations_.fetch_add(1, std::memory_order_acq_rel); }

void channel_state::end_operation() { active_operations_.fetch_sub(1, std::memory_order_acq_rel); }

bool channel_state::has_root() const
{
    return roots_[0].load(std::memory_order_acquire) != 0 || roots_[1].load(std::memory_order_acquire) != 0;
}

bool channel_state::can_reap() const
{
    return !has_root() && active_operations_.load(std::memory_order_acquire) == 0 &&
           active_claims_.load(std::memory_order_acquire) == 0;
}

void channel_state::collect_reachable_states(freelibcxx::vector<channel_state *> &targets) const
{
    auto &lock = const_cast<lock::spinlock_t &>(lock_);
    uctx::RawSpinLockUninterruptibleContext icu(lock);
    for (auto *queue : queues_)
    {
        const u64 count = queue->fifo.size();
        for (u64 i = 0; i < count; i++)
        {
            auto *message = queue->fifo.at(i);
            if (message == nullptr)
                continue;
            for (u64 resource_index = 0; resource_index < message->resource_count(); resource_index++)
            {
                auto &resource = message->resource(resource_index);
                if (!resource.valid() || !resource.object()->is<raw_channel_endpoint>())
                    continue;
                auto *endpoint = resource.object()->get<raw_channel_endpoint>();
                if (endpoint != nullptr)
                    targets.push_back(endpoint->state());
            }
        }
    }
}

void channel_state::discard_orphan_messages()
{
    freelibcxx::vector<channel_message *> discarded(memory::KernelCommonAllocatorV);
    discarded.ensure(max_messages_ * 2);
    if (max_messages_ != 0 && discarded.data() == nullptr)
        return;
    {
        uctx::RawSpinLockUninterruptibleContext icu(lock_);
        for (auto *queue : queues_)
        {
            while (!queue->fifo.empty())
            {
                if (!queue->fifo.claim_front())
                    break;
                auto *message = queue->fifo.front();
                queue->fifo.commit_claim();
                queue->bytes -= message->byte_count();
                queue->resources -= message->resource_count();
                release_global(message->byte_count(), message->resource_count());
                discarded.push_back(message);
            }
        }
    }
    notify_channel_waiters();
    for (auto *message : discarded)
        memory::Delete<>(memory::KernelCommonAllocatorV, message);
}

u8 channel_state::side_for(const raw_channel_endpoint *endpoint) const
{
    return endpoint == nullptr ? 0 : endpoint->side();
}

raw_channel_endpoint::raw_channel_endpoint(channel_state *state, u8 side)
    : kobject(type_e::raw_channel_end)
    , state_(state)
    , side_(side)
{
    if (state_ != nullptr)
        state_->endpoint_object_created();
}

raw_channel_endpoint::~raw_channel_endpoint()
{
    if (state_ != nullptr)
        state_->endpoint_object_destroyed();
}

void raw_channel_endpoint::on_capability_acquire(capability::location where)
{
    if (state_ != nullptr)
        state_->capability_acquired(side_, where);
}

void raw_channel_endpoint::on_capability_release(capability::location where)
{
    if (state_ != nullptr)
        state_->capability_released(side_, where);
}

void raw_channel_endpoint::on_capability_handoff(capability::location from, capability::location to)
{
    (void)to;
    if (state_ != nullptr)
        state_->capability_released(side_, from);
}

na_signal_t raw_channel_endpoint::capability_signals() const { return state_ == nullptr ? 0 : state_->signals(side_); }

void raw_channel_endpoint::begin_operation()
{
    if (state_ != nullptr)
        state_->begin_operation();
}

void raw_channel_endpoint::end_operation()
{
    if (state_ != nullptr)
        state_->end_operation();
}

na_status_t create_raw_channel(khandle &left, khandle &right, const na_channel_options_t *options)
{
    na_channel_options_t values{};
    values.struct_size = sizeof(values);
    values.max_messages = NA_CHANNEL_DEFAULT_MAX_MESSAGES;
    values.max_bytes = NA_CHANNEL_DEFAULT_MAX_BYTES;
    values.max_resources = NA_CHANNEL_DEFAULT_MAX_RESOURCES;
    if (options != nullptr)
    {
        auto status = naos::usercopy::copy_versioned(values, options);
        if (status != NA_STATUS_OK)
            return status;
        if (!valid_struct_size(values.struct_size, sizeof(values)) || values.flags != 0 || values.reserved0 != 0)
            return NA_STATUS_INVALID_ARGUMENT;
        if (values.max_messages == 0)
            values.max_messages = NA_CHANNEL_DEFAULT_MAX_MESSAGES;
        if (values.max_bytes == 0)
            values.max_bytes = NA_CHANNEL_DEFAULT_MAX_BYTES;
        if (values.max_resources == 0)
            values.max_resources = NA_CHANNEL_DEFAULT_MAX_RESOURCES;
    }
    if (values.max_messages > NA_CHANNEL_MAX_MESSAGES || values.max_bytes > NA_CHANNEL_MAX_MESSAGE_BYTES * 16 ||
        values.max_resources > NA_CHANNEL_DEFAULT_MAX_RESOURCES)
        return NA_STATUS_INVALID_ARGUMENT;

    auto *state = memory::New<channel_state>(memory::KernelCommonAllocatorV, values.max_messages, values.max_bytes,
                                             values.max_resources);
    if (state == nullptr)
        return NA_STATUS_RESOURCE_EXHAUSTED;
    if (!state->valid())
    {
        memory::Delete<>(memory::KernelCommonAllocatorV, state);
        return NA_STATUS_RESOURCE_EXHAUSTED;
    }
    auto left_endpoint = handle_t<raw_channel_endpoint>::make(state, 0);
    auto right_endpoint = handle_t<raw_channel_endpoint>::make(state, 1);
    if (!left_endpoint || !right_endpoint)
    {
        left_endpoint.reset();
        right_endpoint.reset();
        unregister_state(state);
        memory::Delete<>(memory::KernelCommonAllocatorV, state);
        return NA_STATUS_RESOURCE_EXHAUSTED;
    }
    left = left_endpoint;
    right = right_endpoint;
    return NA_STATUS_OK;
}

na_status_t send_raw_channel(task::resource_table_t &resources, na_handle_t endpoint,
                             const na_channel_send_frame_t *frame)
{
    capability::entry target;
    if (!resources.lookup_native(endpoint, target) || !target.object)
        return NA_STATUS_INVALID_HANDLE;
    if (target.meta.binding != NA_BINDING_RAW_CHANNEL_END)
        return NA_STATUS_WRONG_BINDING;
    auto *channel = target.object->get<raw_channel_endpoint>();
    if (channel == nullptr)
        return NA_STATUS_WRONG_BINDING;
    channel->begin_operation();
    auto finish = [&](na_status_t status) {
        channel->end_operation();
        return status;
    };

    na_channel_send_frame_t values{};
    auto status = naos::usercopy::copy_versioned(values, frame);
    if (status != NA_STATUS_OK)
        return finish(status);
    if (!valid_struct_size(values.struct_size, sizeof(values)) || values.flags != 0 || values.reserved0 != 0 ||
        values.reserved1 != 0)
        return finish(NA_STATUS_INVALID_ARGUMENT);
    if (values.byte_count > NA_CHANNEL_MAX_MESSAGE_BYTES || values.resource_count > NA_CHANNEL_MAX_RESOURCES)
        return finish(NA_STATUS_INVALID_MESSAGE);
    if (values.byte_count != 0 && values.bytes == 0)
        return finish(NA_STATUS_FAULT);
    if (values.resource_count != 0 && values.resources == 0)
        return finish(NA_STATUS_FAULT);
    if (!naos::usercopy::valid_range(values.bytes, values.byte_count) ||
        !naos::usercopy::valid_range(values.resources, values.resource_count * sizeof(na_resource_disposition_t)))
        return finish(NA_STATUS_FAULT);
    if (naos::usercopy::ranges_overlap(reinterpret_cast<u64>(frame), sizeof(*frame), values.bytes, values.byte_count) ||
        naos::usercopy::ranges_overlap(reinterpret_cast<u64>(frame), sizeof(*frame), values.resources,
                                       values.resource_count * sizeof(na_resource_disposition_t)) ||
        naos::usercopy::ranges_overlap(values.bytes, values.byte_count, values.resources,
                                       values.resource_count * sizeof(na_resource_disposition_t)))
        return finish(NA_STATUS_INVALID_ARGUMENT);

    auto *message =
        memory::New<channel_message>(memory::KernelCommonAllocatorV, values.byte_count, values.resource_count);
    if (message == nullptr || !message->valid())
    {
        if (message != nullptr)
            memory::Delete<>(memory::KernelCommonAllocatorV, message);
        return finish(NA_STATUS_RESOURCE_EXHAUSTED);
    }
    status = copy_from_user(message->bytes(), values.bytes, values.byte_count);
    if (status != NA_STATUS_OK)
    {
        memory::Delete<>(memory::KernelCommonAllocatorV, message);
        return finish(status);
    }

    freelibcxx::vector<na_resource_disposition_t> dispositions(memory::KernelCommonAllocatorV);
    dispositions.ensure(values.resource_count);
    if (values.resource_count != 0 && dispositions.data() == nullptr)
    {
        memory::Delete<>(memory::KernelCommonAllocatorV, message);
        return finish(NA_STATUS_RESOURCE_EXHAUSTED);
    }
    for (u64 i = 0; i < values.resource_count; i++)
    {
        na_resource_disposition_t disposition{};
        status = copy_from_user(&disposition, values.resources + i * sizeof(na_resource_disposition_t),
                                sizeof(na_resource_disposition_t));
        if (status != NA_STATUS_OK)
        {
            memory::Delete<>(memory::KernelCommonAllocatorV, message);
            return finish(status);
        }
        dispositions.push_back(disposition);
    }

    capability::transfer_record_list records(memory::KernelCommonAllocatorV);
    status = resources.take_native_batch(dispositions.data(), dispositions.size(), endpoint, records);
    if (status != NA_STATUS_OK)
    {
        memory::Delete<>(memory::KernelCommonAllocatorV, message);
        return finish(status);
    }
    status = channel->state()->enqueue(channel->side(), message, records, resources);
    if (status != NA_STATUS_OK)
        memory::Delete<>(memory::KernelCommonAllocatorV, message);
    collect_orphaned_channels();
    return finish(status);
}

na_status_t receive_raw_channel(task::resource_table_t &resources, na_handle_t endpoint,
                                na_channel_receive_frame_t *frame)
{
    capability::entry target;
    if (!resources.lookup_native(endpoint, target) || !target.object)
        return NA_STATUS_INVALID_HANDLE;
    if (target.meta.binding != NA_BINDING_RAW_CHANNEL_END)
        return NA_STATUS_WRONG_BINDING;
    auto *channel = target.object->get<raw_channel_endpoint>();
    if (channel == nullptr)
        return NA_STATUS_WRONG_BINDING;
    channel->begin_operation();
    auto finish = [&](na_status_t status) {
        channel->end_operation();
        return status;
    };

    na_channel_receive_frame_t values{};
    auto status = naos::usercopy::copy_versioned(values, frame);
    if (status != NA_STATUS_OK)
        return finish(status);
    if (!valid_struct_size(values.struct_size, sizeof(values)) || values.flags != 0 || values.method_id != 0 ||
        values.responder != NA_HANDLE_INVALID || values.reserved0 != 0)
        return finish(NA_STATUS_INVALID_ARGUMENT);
    if (values.byte_capacity > NA_CHANNEL_MAX_MESSAGE_BYTES || values.resource_capacity > NA_CHANNEL_MAX_RESOURCES)
        return finish(NA_STATUS_INVALID_ARGUMENT);

    channel_message *message = nullptr;
    status = channel->state()->claim_receive(channel->side(), message);
    if (status != NA_STATUS_OK)
        return finish(status);
    auto cancel = [&] { channel->state()->cancel_receive(channel->side(), message); };

    values.method_id = 0;
    values.responder = NA_HANDLE_INVALID;
    values.actual_bytes = 0;
    values.actual_resources = 0;
    values.required_bytes = message->byte_count();
    values.required_resources = message->resource_count();
    if (values.byte_capacity < message->byte_count() || values.resource_capacity < message->resource_count())
    {
        status = copy_to_user(reinterpret_cast<u64>(frame), &values, sizeof(values));
        cancel();
        return finish(status == NA_STATUS_OK ? NA_STATUS_BUFFER_TOO_SMALL : status);
    }
    if (!naos::usercopy::valid_output_range(values.bytes, values.byte_capacity))
    {
        cancel();
        return finish(NA_STATUS_FAULT);
    }
    if (!naos::usercopy::valid_output_range(values.resources, values.resource_capacity * sizeof(na_handle_t)))
    {
        cancel();
        return finish(NA_STATUS_FAULT);
    }
    if (naos::usercopy::ranges_overlap(reinterpret_cast<u64>(frame), sizeof(*frame), values.bytes,
                                       message->byte_count()) ||
        naos::usercopy::ranges_overlap(reinterpret_cast<u64>(frame), sizeof(*frame), values.resources,
                                       message->resource_count() * sizeof(na_handle_t)) ||
        naos::usercopy::ranges_overlap(values.bytes, message->byte_count(), values.resources,
                                       message->resource_count() * sizeof(na_handle_t)))
    {
        cancel();
        return finish(NA_STATUS_INVALID_ARGUMENT);
    }

    freelibcxx::vector<na_handle_t> reserved(memory::KernelCommonAllocatorV);
    status = resources.reserve_native(reserved, message->resource_count());
    if (status != NA_STATUS_OK)
    {
        cancel();
        return finish(status);
    }
    status = copy_to_user(values.bytes, message->bytes(), message->byte_count());
    if (status == NA_STATUS_OK && message->resource_count() != 0)
        status = copy_to_user(values.resources, reserved.data(), message->resource_count() * sizeof(na_handle_t));
    if (status != NA_STATUS_OK)
    {
        resources.rollback_native(reserved);
        cancel();
        return finish(status);
    }

    values.actual_bytes = message->byte_count();
    values.actual_resources = message->resource_count();
    values.required_bytes = 0;
    values.required_resources = 0;
    status = copy_to_user(reinterpret_cast<u64>(frame), &values, sizeof(values));
    if (status != NA_STATUS_OK)
    {
        resources.rollback_native(reserved);
        cancel();
        return finish(status);
    }

    for (u64 i = 0; i < message->resource_count(); i++)
    {
        status = resources.activate_native(reserved[i], std::move(message->resource(i)));
        if (status != NA_STATUS_OK)
        {
            for (u64 j = 0; j < i; j++)
                resources.close_native(reserved[j]);
            resources.rollback_native(reserved);
            cancel();
            return finish(status);
        }
    }
    if (!channel->state()->commit_receive(channel->side(), message))
    {
        for (auto handle : reserved)
            resources.close_native(handle);
        cancel();
        return finish(NA_STATUS_RESOURCE_EXHAUSTED);
    }
    memory::Delete<>(memory::KernelCommonAllocatorV, message);
    collect_orphaned_channels();
    return finish(NA_STATUS_OK);
}

na_status_t receive_raw_channel_kernel(task::resource_table_t &resources, na_handle_t endpoint, byte *bytes,
                                       u64 byte_capacity, u64 &actual_bytes, freelibcxx::vector<na_handle_t> &handles)
{
    actual_bytes = 0;
    handles.clear();
    capability::entry target;
    if (!resources.lookup_native(endpoint, target) || !target.object)
        return NA_STATUS_INVALID_HANDLE;
    if (target.meta.binding != NA_BINDING_RAW_CHANNEL_END)
        return NA_STATUS_WRONG_BINDING;
    auto *channel = target.object->get<raw_channel_endpoint>();
    if (channel == nullptr)
        return NA_STATUS_WRONG_BINDING;
    channel->begin_operation();
    auto finish = [&](na_status_t status) {
        channel->end_operation();
        return status;
    };

    channel_message *message = nullptr;
    auto status = channel->state()->claim_receive(channel->side(), message);
    if (status != NA_STATUS_OK)
        return finish(status);
    auto cancel = [&] { channel->state()->cancel_receive(channel->side(), message); };

    actual_bytes = message->byte_count();
    if ((actual_bytes != 0 && bytes == nullptr) || actual_bytes > byte_capacity ||
        message->resource_count() > NA_CAPABILITY_MAX_PER_PROCESS)
    {
        actual_bytes = 0;
        cancel();
        return finish(NA_STATUS_BUFFER_TOO_SMALL);
    }

    status = resources.reserve_native(handles, message->resource_count());
    if (status != NA_STATUS_OK)
    {
        actual_bytes = 0;
        cancel();
        return finish(status);
    }
    if (message->byte_count() != 0)
        memcpy(bytes, message->bytes(), message->byte_count());

    for (u64 i = 0; i < message->resource_count(); i++)
    {
        status = resources.activate_native(handles[i], std::move(message->resource(i)));
        if (status != NA_STATUS_OK)
        {
            for (u64 j = 0; j < i; j++)
                resources.close_native(handles[j]);
            resources.rollback_native(handles);
            actual_bytes = 0;
            cancel();
            return finish(status);
        }
    }
    if (!channel->state()->commit_receive(channel->side(), message))
    {
        for (auto handle : handles)
            resources.close_native(handle);
        resources.rollback_native(handles);
        actual_bytes = 0;
        cancel();
        return finish(NA_STATUS_RESOURCE_EXHAUSTED);
    }
    memory::Delete<>(memory::KernelCommonAllocatorV, message);
    collect_orphaned_channels();
    return finish(NA_STATUS_OK);
}

na_status_t discard_raw_channel(task::resource_table_t &resources, na_handle_t endpoint)
{
    capability::entry target;
    if (!resources.lookup_native(endpoint, target) || !target.object)
        return NA_STATUS_INVALID_HANDLE;
    if (target.meta.binding != NA_BINDING_RAW_CHANNEL_END)
        return NA_STATUS_WRONG_BINDING;
    auto *channel = target.object->get<raw_channel_endpoint>();
    if (channel == nullptr)
        return NA_STATUS_WRONG_BINDING;
    channel->begin_operation();
    channel_message *message = nullptr;
    const bool discarded = channel->state()->discard(channel->side(), message);
    channel->end_operation();
    if (!discarded)
    {
        collect_orphaned_channels();
        return NA_STATUS_WOULD_BLOCK;
    }
    memory::Delete<>(memory::KernelCommonAllocatorV, message);
    collect_orphaned_channels();
    return NA_STATUS_OK;
}

na_status_t wait_many(task::resource_table_t &resources, na_wait_item_t *items, u64 count, u64 deadline)
{
    if (count == 0 || count > NA_CAPABILITY_MAX_PER_PROCESS || items == nullptr ||
        !is_user_space_range(items, count * sizeof(na_wait_item_t)))
        return NA_STATUS_INVALID_ARGUMENT;

    freelibcxx::vector<na_wait_item_t> snapshot(memory::KernelCommonAllocatorV);
    snapshot.resize(count, na_wait_item_t{});
    if (snapshot.data() == nullptr)
        return NA_STATUS_RESOURCE_EXHAUSTED;
    const auto copy_status =
        naos::usercopy::copy_from(snapshot.data(), reinterpret_cast<u64>(items), count * sizeof(na_wait_item_t));
    if (copy_status != NA_STATUS_OK)
        return copy_status;
    for (auto &item : snapshot)
    {
        capability::entry entry;
        if (item.signals == 0 || !resources.lookup_native(item.handle, entry))
            return NA_STATUS_INVALID_HANDLE;
        if ((entry.meta.meta_rights & NA_RIGHT_WAIT) == 0)
            return NA_STATUS_ACCESS_DENIED;
    }
    wait_request request{&resources, &snapshot};
    if (!wait_condition(&request))
    {
        timer::watcher_id deadline_watcher = timer::invalid_watcher_id;
        if (deadline == 0)
            return NA_STATUS_WOULD_BLOCK;
        if (deadline != std::numeric_limits<u64>::max())
        {
            if (timer::get_high_resolution_time() >= deadline)
                return NA_STATUS_WAIT_TIMED_OUT;
            ensure_waiters();
            deadline_watcher = timer::schedule_at(deadline, timer::timer_handler::bind<&wait_deadline_wakeup>());
            waiters->do_wait([&request] { return wait_condition(&request); });
            (void)timer::cancel(deadline_watcher);
            if (!wait_condition(&request) && timer::get_high_resolution_time() >= deadline)
                return NA_STATUS_WAIT_TIMED_OUT;
        }
        else
        {
            ensure_waiters();
            waiters->do_wait([&request] { return wait_condition(&request); });
        }
    }
    if (copy_to_user(reinterpret_cast<u64>(items), snapshot.data(), count * sizeof(na_wait_item_t)) != NA_STATUS_OK)
        return NA_STATUS_FAULT;
    return NA_STATUS_OK;
}

na_status_t wait_for_signal(task::resource_table_t &resources, na_handle_t handle, na_signal_t signals, u64 deadline)
{
    if (handle == NA_HANDLE_INVALID || signals == 0)
        return NA_STATUS_INVALID_ARGUMENT;
    capability::entry entry;
    if (!resources.lookup_native(handle, entry) || !entry.object)
        return NA_STATUS_INVALID_HANDLE;
    if ((entry.meta.meta_rights & NA_RIGHT_WAIT) == 0)
        return NA_STATUS_ACCESS_DENIED;

    na_wait_item_t item{handle, signals, 0};
    freelibcxx::vector<na_wait_item_t> snapshot(memory::KernelCommonAllocatorV);
    snapshot.push_back(item);
    if (snapshot.data() == nullptr)
        return NA_STATUS_RESOURCE_EXHAUSTED;
    wait_request request{&resources, &snapshot};
    if (!wait_condition(&request))
    {
        timer::watcher_id deadline_watcher = timer::invalid_watcher_id;
        if (deadline == 0)
            return NA_STATUS_WOULD_BLOCK;
        if (deadline != std::numeric_limits<u64>::max())
        {
            if (timer::get_high_resolution_time() >= deadline)
                return NA_STATUS_WAIT_TIMED_OUT;
            ensure_waiters();
            if (waiters == nullptr)
                return NA_STATUS_RESOURCE_EXHAUSTED;
            deadline_watcher = timer::schedule_at(deadline, timer::timer_handler::bind<&wait_deadline_wakeup>());
        }
        else
        {
            ensure_waiters();
            if (waiters == nullptr)
                return NA_STATUS_RESOURCE_EXHAUSTED;
        }
        waiters->do_wait([&request] { return wait_condition(&request); });
        if (deadline_watcher != timer::invalid_watcher_id)
            (void)timer::cancel(deadline_watcher);
        if (!wait_condition(&request) && deadline != std::numeric_limits<u64>::max() &&
            timer::get_high_resolution_time() >= deadline)
            return NA_STATUS_WAIT_TIMED_OUT;
    }
    return NA_STATUS_OK;
}

void notify_channel_waiters()
{
    if (waiters != nullptr)
        waiters->do_wake_up();
}

void collect_orphaned_channels()
{
    uctx::RawSpinLockUninterruptibleContext registry_icu(registry_lock);
    if (registry == nullptr || registry->empty())
        return;

    freelibcxx::vector<channel_state *> states(memory::KernelCommonAllocatorV);
    for (auto state : *registry)
        states.push_back(state);
    freelibcxx::vector<u8> marked(memory::KernelCommonAllocatorV);
    marked.ensure(states.size());
    if (states.size() != 0 && marked.data() == nullptr)
        return;
    for (u64 i = 0; i < states.size(); i++)
        marked.push_back(0);

    freelibcxx::vector<channel_state *> work(memory::KernelCommonAllocatorV);
    for (u64 i = 0; i < states.size(); i++)
    {
        if (states[i]->has_root() || !states[i]->can_reap())
        {
            marked[i] = 1;
            work.push_back(states[i]);
        }
    }
    for (u64 index = 0; index < work.size(); index++)
    {
        freelibcxx::vector<channel_state *> targets(memory::KernelCommonAllocatorV);
        work[index]->collect_reachable_states(targets);
        for (auto target : targets)
        {
            u64 target_index = 0;
            if (contains_state(states, target, &target_index) && marked[target_index] == 0)
            {
                marked[target_index] = 1;
                work.push_back(target);
            }
        }
    }

    // Drop messages for the whole unreachable set before deleting any state.
    // An in-transit endpoint can point back to another orphaned channel; doing
    // this one state at a time can destroy the target state while its endpoint
    // object still holds the target's raw state pointer.
    for (u64 i = 0; i < states.size(); i++)
    {
        if (marked[i] == 0 && states[i]->can_reap())
            states[i]->discard_orphan_messages();
    }
    for (u64 i = 0; i < states.size(); i++)
    {
        auto *state = states[i];
        if (marked[i] != 0 || !state->can_reap() || state->endpoint_object_count() != 0)
            continue;
        auto iterator = registry->begin();
        while (iterator != registry->end() && *iterator != state)
            ++iterator;
        if (iterator != registry->end())
            registry->remove(iterator);
        memory::Delete<>(memory::KernelCommonAllocatorV, state);
    }
}

} // namespace naos::ipc
