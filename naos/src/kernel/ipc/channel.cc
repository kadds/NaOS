#include "kernel/ipc/channel.hpp"
#include "kernel/ipc/epoll.hpp"

#include "kernel/arch/klib.hpp"
#include "kernel/log.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/mutex.hpp"
#include "kernel/task.hpp"
#include "kernel/timer.hpp"
#include "kernel/ucontext.hpp"
#include "kernel/usercopy.hpp"
#include <limits>

namespace naos::ipc
{
KLOG_MODULE(ipc);
namespace
{
lock::spinlock_t registry_lock;
lock::mutex_t registry_collection_lock;
channel_state *registry = nullptr;
std::atomic_uint64_t registry_count{0};

std::atomic<naos_ipc_domain_t *> core_domain{nullptr};
core_lock core_domain_lock;

void *core_control_allocate(void *, std::size_t size, std::size_t alignment)
{
    return memory::KernelCommonAllocatorV->allocate(size, alignment);
}

void core_control_deallocate(void *, void *pointer, std::size_t, std::size_t)
{
    if (pointer != nullptr)
        memory::KernelCommonAllocatorV->deallocate(pointer);
}

void *core_payload_allocate(void *, std::size_t size, std::size_t alignment)
{
    return memory::MemoryAllocatorV->allocate(size, alignment);
}

void core_payload_deallocate(void *, void *pointer, std::size_t, std::size_t)
{
    if (pointer != nullptr)
        memory::MemoryAllocatorV->deallocate(pointer);
}

void core_notify(void *context)
{
    auto *state = static_cast<channel_state *>(context);
    if (state != nullptr)
    {
        state->notify_readiness();
        state->notify_waiters();
    }
}

naos_ipc_allocator_t core_control_allocator{nullptr, core_control_allocate, core_control_deallocate};
naos_ipc_allocator_t core_payload_allocator{nullptr, core_payload_allocate, core_payload_deallocate};
naos_ipc_lock_t core_domain_lock_api{&core_domain_lock, core_lock_acquire, core_lock_release};

naos_ipc_domain_t *get_core_domain()
{
    auto *domain = core_domain.load(std::memory_order_acquire);
    if (domain != nullptr)
        return domain;

    naos_ipc_domain_config_t config{};
    config.memory = &core_control_allocator;
    config.synchronization = &core_domain_lock_api;
    config.max_messages = NA_CHANNEL_GLOBAL_MAX_MESSAGES;
    config.max_bytes = NA_CHANNEL_GLOBAL_MAX_BYTES;
    config.max_resources = NA_CHANNEL_GLOBAL_MAX_RESOURCES;
    auto *candidate = naos_ipc_domain_create(&config);
    if (candidate == nullptr)
        return nullptr;
    domain = nullptr;
    if (core_domain.compare_exchange_strong(domain, candidate, std::memory_order_release, std::memory_order_acquire))
        return candidate;
    naos_ipc_domain_destroy(candidate);
    return domain;
}

struct kernel_resource_holder
{
    capability::transferred_resource resource;
};

void release_kernel_resource(void *, void *value) noexcept
{
    auto *holder = static_cast<kernel_resource_holder *>(value);
    memory::Delete<>(memory::KernelCommonAllocatorV, holder);
}

void clear_core_resource(naos_ipc_resource_t &resource)
{
    resource.context = nullptr;
    resource.value = nullptr;
    resource.release = nullptr;
}

bool make_core_resource_batch(capability::transfer_record_list &records,
                              freelibcxx::vector<naos_ipc_resource_t> &resources)
{
    resources.ensure(records.size());
    if (records.size() != 0 && resources.data() == nullptr)
        return false;
    for (u64 i = 0; i < records.size(); i++)
    {
        auto *holder = memory::New<kernel_resource_holder>(memory::KernelCommonAllocatorV);
        if (holder == nullptr)
        {
            for (u64 j = 0; j < resources.size(); j++)
            {
                auto *previous = static_cast<kernel_resource_holder *>(resources[j].value);
                if (previous == nullptr)
                    continue;
                clear_core_resource(resources[j]);
                records[j].resource = std::move(previous->resource);
                memory::Delete<>(memory::KernelCommonAllocatorV, previous);
            }
            return false;
        }
        holder->resource = std::move(records[i].resource);
        resources.push_back(naos_ipc_resource_t{nullptr, holder, release_kernel_resource});
    }
    return true;
}

void restore_core_resource_batch(capability::transfer_record_list &records,
                                 freelibcxx::vector<naos_ipc_resource_t> &resources)
{
    for (u64 i = 0; i < resources.size(); i++)
    {
        auto *holder = static_cast<kernel_resource_holder *>(resources[i].value);
        if (holder == nullptr)
            continue;
        clear_core_resource(resources[i]);
        records[i].resource = std::move(holder->resource);
        memory::Delete<>(memory::KernelCommonAllocatorV, holder);
    }
}

bool register_state(channel_state *state)
{
    uctx::RawSpinLockUninterruptibleContext icu(registry_lock);
    state->set_registry_next(registry);
    state->set_registry_linked(true);
    registry = state;
    registry_count.fetch_add(1, std::memory_order_release);
    return true;
}

void unregister_state_locked(channel_state *state)
{
    if (state == nullptr || !state->registry_linked())
        return;
    channel_state *previous = nullptr;
    auto *current = registry;
    while (current != nullptr && current != state)
    {
        previous = current;
        current = current->registry_next();
    }
    if (current != state)
        return;
    if (previous != nullptr)
        previous->set_registry_next(state->registry_next());
    else
        registry = state->registry_next();
    state->set_registry_next(nullptr);
    state->set_registry_linked(false);
    registry_count.fetch_sub(1, std::memory_order_release);
}

na_status_t copy_from_user(void *destination, u64 source, u64 size)
{
    return naos::usercopy::copy_from(destination, source, size);
}

na_status_t copy_to_user(u64 destination, const void *source, u64 size)
{
    return naos::usercopy::copy_to(destination, source, size);
}

bool valid_struct_size(u32 actual, u64 expected) { return actual == expected; }

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

void collect_resource_target(void *context, u64, void *, void *value) noexcept
{
    auto *targets = static_cast<freelibcxx::vector<channel_state *> *>(context);
    auto *holder = static_cast<kernel_resource_holder *>(value);
    if (holder == nullptr || !holder->resource.valid() || !holder->resource.object()->is<raw_channel_endpoint>())
        return;
    auto *endpoint = holder->resource.object()->get<raw_channel_endpoint>();
    if (endpoint != nullptr)
        targets->push_back(endpoint->state());
}

void collect_message_targets(void *context, naos_ipc_message_t *message) noexcept
{
    if (message == nullptr)
        return;
    naos_ipc_message_visit_resources(message, collect_resource_target, context);
}

bool take_kernel_resource(channel_message *message, u64 index, capability::transferred_resource &resource)
{
    naos_ipc_resource_t core_resource{};
    const auto status = naos_ipc_message_take_resource(message->core_message(), index, &core_resource);
    if (status != NA_STATUS_OK || core_resource.value == nullptr)
    {
        naos_ipc_resource_reset(&core_resource);
        return false;
    }
    auto *holder = static_cast<kernel_resource_holder *>(core_resource.value);
    clear_core_resource(core_resource);
    if (holder == nullptr)
        return false;
    resource = std::move(holder->resource);
    memory::Delete<>(memory::KernelCommonAllocatorV, holder);
    return resource.valid();
}
} // namespace

channel_message::channel_message(naos_ipc_channel_t *channel, u64 byte_count, u64 resource_count)
    : message_(channel == nullptr ? nullptr : naos_ipc_message_create(channel, byte_count, resource_count))
{
    if (message_ != nullptr)
        naos_ipc_message_set_user_context(message_, this);
}

channel_message::~channel_message()
{
    if (message_ != nullptr)
        naos_ipc_message_destroy(message_);
}

bool channel_message::valid() const { return message_ != nullptr && naos_ipc_message_valid(message_); }

byte *channel_message::bytes()
{
    return reinterpret_cast<byte *>(message_ == nullptr ? nullptr : naos_ipc_message_bytes(message_));
}

const byte *channel_message::bytes() const
{
    return reinterpret_cast<const byte *>(message_ == nullptr ? nullptr : naos_ipc_message_const_bytes(message_));
}

u64 channel_message::byte_count() const { return message_ == nullptr ? 0 : naos_ipc_message_byte_count(message_); }

u64 channel_message::resource_count() const
{
    return message_ == nullptr ? 0 : naos_ipc_message_resource_count(message_);
}

u64 channel_message::resource_capacity() const
{
    return message_ == nullptr ? 0 : naos_ipc_message_resource_capacity(message_);
}

channel_state::channel_state(u64 max_messages, u64 max_bytes, u64 max_resources)
    : core_lock_()
    , core_lock_api_{&core_lock_, core_lock_acquire, core_lock_release}
    , core_notifier_api_{this, core_notify}
    , channel_(nullptr)
{
    const auto domain = get_core_domain();
    if (domain == nullptr)
        return;
    naos_ipc_channel_config_t config{};
    config.owner_domain = domain;
    config.control_memory = &core_control_allocator;
    config.payload_memory = &core_payload_allocator;
    config.synchronization = &core_lock_api_;
    config.notifier = &core_notifier_api_;
    config.max_messages = max_messages;
    config.max_bytes = max_bytes;
    config.max_resources = max_resources;
    config.clock = nullptr;
    config.handles = nullptr;
    channel_ = naos_ipc_channel_create(&config);
}

channel_state::~channel_state()
{
    if (channel_ == nullptr)
        return;
    discard_orphan_messages();
    naos_ipc_channel_destroy(channel_);
    channel_ = nullptr;
}

bool channel_state::valid() const { return channel_ != nullptr && naos_ipc_channel_valid(channel_); }

na_signal_t channel_state::signals(u8 side) const
{
    return channel_ == nullptr ? 0 : naos_ipc_channel_signals(channel_, side);
}

u64 channel_state::queued_messages(u8 side) const
{
    return channel_ == nullptr ? 0 : naos_ipc_channel_queued_messages(channel_, side);
}

u64 channel_state::max_messages() const { return channel_ == nullptr ? 0 : naos_ipc_channel_max_messages(channel_); }

u64 channel_state::endpoint_object_count() const
{
    if (channel_ == nullptr)
        return 0;
    uctx::RawSpinLockUninterruptibleContext guard(const_cast<lock::spinlock_t &>(lifecycle_lock_));
    return endpoint_objects_;
}

na_status_t channel_state::enqueue(u8 sender, channel_message *message, capability::transfer_record_list &records,
                                   task::resource_table_t &resources)
{
    if (!valid() || message == nullptr || records.size() > message->resource_capacity())
        return NA_STATUS_INVALID_ARGUMENT;
    freelibcxx::vector<naos_ipc_resource_t> core_resources(memory::KernelCommonAllocatorV);
    if (!make_core_resource_batch(records, core_resources))
        return NA_STATUS_RESOURCE_EXHAUSTED;
    const auto result = static_cast<na_status_t>(naos_ipc_channel_enqueue(
        channel_, sender, message->core_message(), core_resources.data(), core_resources.size(), 0));
    if (result == NA_STATUS_OK)
        resources.commit_native_batch(records);
    else
    {
        restore_core_resource_batch(records, core_resources);
        const auto restore_status = resources.restore_native_batch(records);
        if (restore_status != NA_STATUS_OK)
            return restore_status;
    }
    return result;
}

na_status_t channel_state::enqueue_kernel(u8 sender, channel_message *message,
                                          capability::transfer_record_list &records, u64 max_queue_messages)
{
    if (!valid() || message == nullptr || records.size() > message->resource_capacity())
        return NA_STATUS_INVALID_ARGUMENT;
    freelibcxx::vector<naos_ipc_resource_t> core_resources(memory::KernelCommonAllocatorV);
    if (!make_core_resource_batch(records, core_resources))
        return NA_STATUS_RESOURCE_EXHAUSTED;
    const auto result = static_cast<na_status_t>(naos_ipc_channel_enqueue(
        channel_, sender, message->core_message(), core_resources.data(), core_resources.size(), max_queue_messages));
    if (result != NA_STATUS_OK)
        restore_core_resource_batch(records, core_resources);
    return result;
}

na_status_t channel_state::claim_receive(u8 side, channel_message *&message)
{
    message = nullptr;
    if (!valid() || side > 1)
        return NA_STATUS_INVALID_ARGUMENT;
    naos_ipc_message_t *core_message = nullptr;
    const auto status = static_cast<na_status_t>(naos_ipc_channel_claim_receive(channel_, side, &core_message));
    if (status == NA_STATUS_OK)
        message = static_cast<channel_message *>(naos_ipc_message_user_context(core_message));
    return status;
}

bool channel_state::cancel_receive(u8 side, channel_message *message)
{
    if (!valid() || side > 1 || message == nullptr)
        return false;
    return naos_ipc_channel_cancel_receive(channel_, side, message->core_message()) != 0;
}

bool channel_state::commit_receive(u8 side, channel_message *message)
{
    if (!valid() || side > 1 || message == nullptr)
        return false;
    return naos_ipc_channel_commit_receive(channel_, side, message->core_message()) != 0;
}

bool channel_state::discard(u8 side, channel_message *&message)
{
    message = nullptr;
    if (!valid() || side > 1)
        return false;
    naos_ipc_message_t *core_message = nullptr;
    if (naos_ipc_channel_discard(channel_, side, &core_message) == 0)
        return false;
    message = static_cast<channel_message *>(naos_ipc_message_user_context(core_message));
    return message != nullptr;
}

void channel_state::endpoint_object_created(raw_channel_endpoint *endpoint)
{
    if (channel_ == nullptr)
        return;
    uctx::RawSpinLockUninterruptibleContext guard(lifecycle_lock_);
    endpoint_objects_++;
    if (endpoint != nullptr && endpoint->side() <= 1)
        endpoints_[endpoint->side()] = endpoint;
}

void channel_state::endpoint_object_destroyed(raw_channel_endpoint *endpoint)
{
    if (channel_ == nullptr)
        return;
    uctx::RawSpinLockUninterruptibleContext guard(lifecycle_lock_);
    if (endpoint_objects_ != 0)
        endpoint_objects_--;
    if (endpoint != nullptr && endpoint->side() <= 1 && endpoints_[endpoint->side()] == endpoint)
        endpoints_[endpoint->side()] = nullptr;
}

void channel_state::notify_readiness()
{
    // The lifecycle lock is also the endpoint lifetime pin.  Do not copy raw
    // pointers out of it: endpoint destruction clears the slot and may free
    // the object on another CPU immediately after the lock is released.
    uctx::RawSpinLockUninterruptibleContext guard(lifecycle_lock_);
    for (auto *endpoint : endpoints_)
        if (endpoint != nullptr)
            endpoint->notify_readiness();
}

void channel_state::notify_waiters() { wait_queue_.do_wake_up(); }

void channel_state::kernel_owner_acquired(u8 side)
{
    if (side <= 1)
        naos_ipc_channel_side_reference_acquired(channel_, side);
}

void channel_state::kernel_owner_released(u8 side)
{
    if (side <= 1)
        naos_ipc_channel_side_reference_released(channel_, side);
}

void channel_state::capability_acquired(u8 side, capability::location where)
{
    if (side > 1)
        return;
    if (where == capability::location::table_root)
    {
        uctx::RawSpinLockUninterruptibleContext guard(lifecycle_lock_);
        roots_[side]++;
    }
    naos_ipc_channel_side_reference_acquired(channel_, side);
}

void channel_state::capability_released(u8 side, capability::location where)
{
    if (side > 1)
        return;
    if (where == capability::location::table_root)
    {
        uctx::RawSpinLockUninterruptibleContext guard(lifecycle_lock_);
        if (roots_[side] != 0)
            roots_[side]--;
    }
    naos_ipc_channel_side_reference_released(channel_, side);
}

void channel_state::begin_operation()
{
    if (channel_ == nullptr)
    {
        KLOG_WARN("channel operation on invalid state {}", log::hex(reinterpret_cast<u64>(this)));
        return;
    }
    naos_ipc_channel_begin_operation(channel_);
}

void channel_state::end_operation()
{
    if (channel_ != nullptr)
        naos_ipc_channel_end_operation(channel_);
}

bool channel_state::has_root() const
{
    if (channel_ == nullptr)
        return false;
    uctx::RawSpinLockUninterruptibleContext guard(const_cast<lock::spinlock_t &>(lifecycle_lock_));
    return roots_[0] != 0 || roots_[1] != 0;
}

bool channel_state::can_reap() const
{
    if (channel_ == nullptr)
        return false;
    {
        uctx::RawSpinLockUninterruptibleContext guard(const_cast<lock::spinlock_t &>(lifecycle_lock_));
        if (endpoint_objects_ != 0 || roots_[0] != 0 || roots_[1] != 0)
            return false;
    }
    return naos_ipc_channel_can_reap(channel_);
}

void channel_state::collect_reachable_states(freelibcxx::vector<channel_state *> &targets) const
{
    naos_ipc_channel_visit_queued_messages(channel_, collect_message_targets, &targets);
}

void channel_state::discard_orphan_messages()
{
    for (u8 side = 0; side < 2; side++)
    {
        for (;;)
        {
            naos_ipc_message_t *core_message = nullptr;
            if (naos_ipc_channel_discard(channel_, side, &core_message) == 0)
                break;
            auto *message = static_cast<channel_message *>(naos_ipc_message_user_context(core_message));
            if (message != nullptr)
                memory::Delete<>(memory::KernelCommonAllocatorV, message);
            else
                naos_ipc_message_destroy(core_message);
        }
    }
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
        state_->endpoint_object_created(this);
}

raw_channel_endpoint::~raw_channel_endpoint()
{
    if (state_ != nullptr)
        state_->endpoint_object_destroyed(this);
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
    if (state_ != nullptr && state_->valid())
        state_->begin_operation();
    else
        KLOG_WARN("channel endpoint operation on invalid state {}", log::hex(reinterpret_cast<u64>(state_)));
}

void raw_channel_endpoint::end_operation()
{
    if (state_ != nullptr && state_->valid())
        state_->end_operation();
}

namespace
{
na_status_t create_raw_channel_with_options(khandle &left, khandle &right, na_channel_options_t values)
{
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
        memory::Delete<>(memory::KernelCommonAllocatorV, state);
        return NA_STATUS_RESOURCE_EXHAUSTED;
    }
    if (!register_state(state))
    {
        left_endpoint.reset();
        right_endpoint.reset();
        memory::Delete<>(memory::KernelCommonAllocatorV, state);
        return NA_STATUS_RESOURCE_EXHAUSTED;
    }
    left = left_endpoint;
    right = right_endpoint;
    return NA_STATUS_OK;
}
} // namespace

na_status_t create_raw_channel(khandle &left, khandle &right, const na_channel_options_t *options)
{
    na_channel_options_t values{};
    values.struct_size = sizeof(values);
    values.max_messages = NA_CHANNEL_DEFAULT_MAX_MESSAGES;
    values.max_bytes = NA_CHANNEL_DEFAULT_MAX_BYTES;
    values.max_resources = NA_CHANNEL_DEFAULT_MAX_RESOURCES;
    if (options != nullptr)
    {
        const auto status = naos::usercopy::copy_versioned(values, options);
        if (status != NA_STATUS_OK)
            return status;
        if (!valid_struct_size(values.struct_size, sizeof(values)) || values.flags != 0 || values.reserved0 != 0)
            return NA_STATUS_INVALID_ARGUMENT;
    }
    if (values.max_messages == 0)
        values.max_messages = NA_CHANNEL_DEFAULT_MAX_MESSAGES;
    if (values.max_bytes == 0)
        values.max_bytes = NA_CHANNEL_DEFAULT_MAX_BYTES;
    if (values.max_resources == 0)
        values.max_resources = NA_CHANNEL_DEFAULT_MAX_RESOURCES;
    return create_raw_channel_with_options(left, right, values);
}

na_status_t create_raw_channel_kernel(khandle &left, khandle &right, const na_channel_options_t *options)
{
    na_channel_options_t values{};
    values.struct_size = sizeof(values);
    values.max_messages = NA_CHANNEL_DEFAULT_MAX_MESSAGES;
    values.max_bytes = NA_CHANNEL_DEFAULT_MAX_BYTES;
    values.max_resources = NA_CHANNEL_DEFAULT_MAX_RESOURCES;
    if (options != nullptr)
    {
        values = *options;
        if (!valid_struct_size(values.struct_size, sizeof(values)) || values.flags != 0 || values.reserved0 != 0)
            return NA_STATUS_INVALID_ARGUMENT;
    }
    if (values.max_messages == 0)
        values.max_messages = NA_CHANNEL_DEFAULT_MAX_MESSAGES;
    if (values.max_bytes == 0)
        values.max_bytes = NA_CHANNEL_DEFAULT_MAX_BYTES;
    if (values.max_resources == 0)
        values.max_resources = NA_CHANNEL_DEFAULT_MAX_RESOURCES;
    return create_raw_channel_with_options(left, right, values);
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

    auto *message = memory::New<channel_message>(memory::KernelCommonAllocatorV, channel->state()->core_channel(),
                                                 values.byte_count, values.resource_count);
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

na_status_t send_raw_channel_kernel(task::resource_table_t &resources, na_handle_t endpoint, const byte *bytes,
                                    u64 byte_count)
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
    if (byte_count > NA_CHANNEL_MAX_MESSAGE_BYTES || (byte_count != 0 && bytes == nullptr))
        return finish(NA_STATUS_INVALID_MESSAGE);

    auto *message =
        memory::New<channel_message>(memory::KernelCommonAllocatorV, channel->state()->core_channel(), byte_count, 0);
    if (message == nullptr || !message->valid())
    {
        if (message != nullptr)
            memory::Delete<>(memory::KernelCommonAllocatorV, message);
        return finish(NA_STATUS_RESOURCE_EXHAUSTED);
    }
    if (byte_count != 0)
        memcpy(message->bytes(), bytes, byte_count);

    capability::transfer_record_list records(memory::KernelCommonAllocatorV);
    const auto status = channel->state()->enqueue(channel->side(), message, records, resources);
    if (status != NA_STATUS_OK)
        memory::Delete<>(memory::KernelCommonAllocatorV, message);
    collect_orphaned_channels();
    return finish(status);
}

na_status_t send_raw_channel_kernel(const khandle &endpoint, const byte *bytes, u64 byte_count)
{
    if (!endpoint)
        return NA_STATUS_INVALID_HANDLE;
    auto *channel = endpoint.as<raw_channel_endpoint>().operator&();
    if (channel == nullptr)
        return NA_STATUS_WRONG_BINDING;
    channel->begin_operation();
    auto finish = [&](na_status_t status) {
        channel->end_operation();
        return status;
    };
    if (byte_count > NA_CHANNEL_MAX_MESSAGE_BYTES || (byte_count != 0 && bytes == nullptr))
        return finish(NA_STATUS_INVALID_MESSAGE);

    auto *message =
        memory::New<channel_message>(memory::KernelCommonAllocatorV, channel->state()->core_channel(), byte_count, 0);
    if (message == nullptr || !message->valid())
    {
        if (message != nullptr)
            memory::Delete<>(memory::KernelCommonAllocatorV, message);
        return finish(NA_STATUS_RESOURCE_EXHAUSTED);
    }
    if (byte_count != 0)
        memcpy(message->bytes(), bytes, byte_count);

    capability::transfer_record_list records(memory::KernelCommonAllocatorV);
    const auto status = channel->state()->enqueue_kernel(channel->side(), message, records);
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
        values.responder != NA_HANDLE_INVALID || values.caller_pid != 0)
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
        capability::transferred_resource resource;
        if (!take_kernel_resource(message, i, resource))
        {
            for (u64 j = 0; j < i; j++)
                resources.close_native(reserved[j]);
            resources.rollback_native(reserved);
            if (channel->state()->commit_receive(channel->side(), message))
                memory::Delete<>(memory::KernelCommonAllocatorV, message);
            else
                channel->state()->cancel_receive(channel->side(), message);
            return finish(NA_STATUS_RESOURCE_EXHAUSTED);
        }
        status = resources.activate_native(reserved[i], std::move(resource));
        if (status != NA_STATUS_OK)
        {
            for (u64 j = 0; j < i; j++)
                resources.close_native(reserved[j]);
            resources.rollback_native(reserved);
            if (channel->state()->commit_receive(channel->side(), message))
                memory::Delete<>(memory::KernelCommonAllocatorV, message);
            else
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
        capability::transferred_resource resource;
        if (!take_kernel_resource(message, i, resource))
        {
            for (u64 j = 0; j < i; j++)
                resources.close_native(handles[j]);
            resources.rollback_native(handles);
            actual_bytes = 0;
            if (channel->state()->commit_receive(channel->side(), message))
                memory::Delete<>(memory::KernelCommonAllocatorV, message);
            else
                channel->state()->cancel_receive(channel->side(), message);
            return finish(NA_STATUS_RESOURCE_EXHAUSTED);
        }
        status = resources.activate_native(handles[i], std::move(resource));
        if (status != NA_STATUS_OK)
        {
            for (u64 j = 0; j < i; j++)
                resources.close_native(handles[j]);
            resources.rollback_native(handles);
            actual_bytes = 0;
            if (channel->state()->commit_receive(channel->side(), message))
                memory::Delete<>(memory::KernelCommonAllocatorV, message);
            else
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

namespace
{
struct deadline_wakeup
{
    task::wait_queue_t *queue;

    void wake(timeclock::microsecond_t) noexcept
    {
        if (queue != nullptr)
            queue->do_wake_up();
    }
};
} // namespace

na_status_t wait_for_condition(task::wait_queue_t &queue, freelibcxx::function_ref<bool()> condition,
                               timeclock::microsecond_t deadline)
{
    if (condition())
        return NA_STATUS_OK;
    if (deadline == 0)
        return NA_STATUS_WOULD_BLOCK;

    timer::watcher_id deadline_watcher = timer::invalid_watcher_id;
    deadline_wakeup wake{&queue};
    auto *waiter = task::current();
    const auto disarm_deadline = [&]() {
        if (deadline_watcher == timer::invalid_watcher_id)
            return;
        const auto watcher = deadline_watcher;
        (void)timer::cancel(watcher);
        deadline_watcher = timer::invalid_watcher_id;
        if (waiter != nullptr && waiter->wait_timeout_watcher == watcher)
            waiter->wait_timeout_watcher = timer::invalid_watcher_id;
    };
    if (deadline != std::numeric_limits<u64>::max())
    {
        if (timer::get_high_resolution_time() >= deadline)
            return NA_STATUS_WAIT_TIMED_OUT;
        deadline_watcher = timer::schedule_at(deadline, timer::timer_handler::bind<&deadline_wakeup::wake>(wake));
        if (deadline_watcher != timer::invalid_watcher_id && waiter != nullptr)
            waiter->wait_timeout_watcher = deadline_watcher;
    }

    for (;;)
    {
        if (condition())
            break;
        if (deadline != std::numeric_limits<u64>::max() && timer::get_high_resolution_time() >= deadline)
        {
            disarm_deadline();
            return NA_STATUS_WAIT_TIMED_OUT;
        }
        queue.do_wait(condition);
    }

    disarm_deadline();
    return NA_STATUS_OK;
}

na_status_t wait_for_raw_channel(task::resource_table_t &resources, na_handle_t handle, na_signal_t signals,
                                 timeclock::microsecond_t deadline)
{
    if (handle == NA_HANDLE_INVALID || signals == 0)
        return NA_STATUS_INVALID_ARGUMENT;
    capability::entry entry;
    if (!resources.lookup_native(handle, entry) || !entry.object)
        return NA_STATUS_INVALID_HANDLE;
    if ((entry.meta.meta_rights & NA_RIGHT_WAIT) == 0)
        return NA_STATUS_ACCESS_DENIED;
    if (entry.meta.binding != NA_BINDING_RAW_CHANNEL_END)
        return NA_STATUS_WRONG_BINDING;
    auto *endpoint = entry.object->get<raw_channel_endpoint>();
    if (endpoint == nullptr || endpoint->state() == nullptr)
        return NA_STATUS_WRONG_BINDING;

    // `entry.object` keeps the endpoint alive while the borrowed state queue
    // and predicate are used. Closing the table slot concurrently therefore
    // becomes the normal peer-closed signal instead of invalidating a raw
    // pointer under the waiter.
    auto object = entry.object;
    return wait_for_condition(
        endpoint->state()->wait_queue(), [&] { return (object->capability_signals() & signals) != 0; }, deadline);
}

void collect_orphaned_channels()
{
    // Reachability walks and vector growth may allocate and may call into the
    // channel core.  Serialize collectors, but hold the raw registry lock only
    // for the bounded pointer snapshot and final unlink phase.
    registry_collection_lock.lock();
    freelibcxx::vector<channel_state *> states(memory::KernelCommonAllocatorV);
    for (;;)
    {
        states.clear();
        const u64 expected = registry_count.load(std::memory_order_acquire);
        states.ensure(expected);
        if (states.capacity() < expected)
        {
            registry_collection_lock.unlock();
            return;
        }
        uctx::RawSpinLockUninterruptibleContext registry_icu(registry_lock);
        if (registry_count.load(std::memory_order_acquire) > states.capacity())
            continue;
        for (auto *state = registry; state != nullptr; state = state->registry_next())
            states.push_back(state);
        break;
    }
    if (states.empty())
    {
        registry_collection_lock.unlock();
        return;
    }

    freelibcxx::vector<u8> marked(memory::KernelCommonAllocatorV);
    marked.ensure(states.size());
    if (states.size() != 0 && marked.data() == nullptr)
    {
        registry_collection_lock.unlock();
        return;
    }
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
    freelibcxx::vector<channel_state *> reap(memory::KernelCommonAllocatorV);
    reap.ensure(states.size());
    if (reap.capacity() < states.size())
    {
        registry_collection_lock.unlock();
        return;
    }
    for (u64 i = 0; i < states.size(); i++)
    {
        auto *state = states[i];
        if (marked[i] == 0 && state->can_reap() && state->endpoint_object_count() == 0)
            reap.push_back(state);
    }
    {
        uctx::RawSpinLockUninterruptibleContext registry_icu(registry_lock);
        for (auto *state : reap)
        {
            if (state->can_reap() && state->endpoint_object_count() == 0)
                unregister_state_locked(state);
        }
    }
    for (auto *state : reap)
    {
        if (!state->registry_linked())
            memory::Delete<>(memory::KernelCommonAllocatorV, state);
    }
    registry_collection_lock.unlock();
}

} // namespace naos::ipc
