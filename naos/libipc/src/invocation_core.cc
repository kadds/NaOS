#include <naos/ipc_core.hpp>

#include <cstring>
#include <limits>
#include <new>
#include <utility>

namespace naos::ipc_core
{
namespace
{
class lock_guard
{
  public:
    explicit lock_guard(lock &value) noexcept
        : value_(value)
    {
        value_.acquire();
    }

    ~lock_guard() { value_.release(); }

    lock_guard(const lock_guard &) = delete;
    lock_guard &operator=(const lock_guard &) = delete;

  private:
    lock &value_;
};

bool valid_allocator(const allocator *value) noexcept { return value != nullptr; }
bool valid_lock(const lock *value) noexcept { return value != nullptr; }

void notify(wait_notifier *notifier) noexcept
{
    if (notifier != nullptr)
        notifier->notify();
}

bool deadline_reached(clock *timer, std::uint64_t deadline) noexcept
{
    return deadline != 0 && timer != nullptr && timer->now() >= deadline;
}
} // namespace

invocation::invocation(const invocation_config &config) noexcept
    : owner_domain_(config.owner_domain)
    , control_allocator_(*config.control_memory)
    , payload_allocator_(*config.payload_memory)
    , lock_(*config.synchronization)
    , notifier_(config.notifier)
    , clock_(config.timer)
    , callbacks_(config.callbacks)
    , method_id_(config.method_id)
    , operation_deadline_(config.operation_deadline)
    , max_response_bytes_(config.max_response_bytes == 0 ? NA_CHANNEL_MAX_MESSAGE_BYTES : config.max_response_bytes)
    , max_response_resources_(config.max_response_resources == 0 ? NA_CHANNEL_MAX_RESOURCES
                                                                 : config.max_response_resources)
{
}

invocation *invocation::create(const invocation_config &config) noexcept
{
    if (config.owner_domain == nullptr || !config.owner_domain->valid() || !valid_allocator(config.control_memory) ||
        !valid_allocator(config.payload_memory) || !valid_lock(config.synchronization) ||
        config.max_response_bytes > NA_CHANNEL_MAX_MESSAGE_BYTES ||
        config.max_response_resources > NA_CHANNEL_MAX_RESOURCES)
        return nullptr;
    void *storage = config.control_memory->allocate(sizeof(invocation), alignof(invocation));
    if (storage == nullptr)
        return nullptr;
    auto *result = ::new (storage) invocation(config);
    result->valid_ = true;
    return result;
}

void invocation::destroy() noexcept
{
    allocator &memory = control_allocator_;
    this->~invocation();
    memory.deallocate(this, sizeof(invocation), alignof(invocation));
}

invocation::~invocation()
{
    clear_response_locked();
    if (claimed_bytes_ != nullptr)
        payload_allocator_.deallocate(claimed_bytes_, claimed_byte_count_, alignof(std::uint8_t));
    claimed_bytes_ = nullptr;
    claimed_byte_count_ = 0;
    release_result_budget_locked();
}

void invocation::notify_state(bool wake_execution) noexcept
{
    notify(notifier_);
    if (wake_execution && callbacks_.wake_execution != nullptr)
        callbacks_.wake_execution(callbacks_.context);
}

void invocation::release_result_budget_locked() noexcept
{
    if (!result_budget_reserved_)
        return;
    owner_domain_->release(max_response_bytes_, max_response_resources_);
    result_budget_reserved_ = false;
}

void invocation::clear_response_locked() noexcept
{
    if (response_resources_ != nullptr)
    {
        for (std::uint64_t index = 0; index < response_resource_capacity_; index++)
            response_resources_[index].~resource();
        control_allocator_.deallocate(response_resources_, sizeof(resource) * response_resource_capacity_,
                                      alignof(resource));
    }
    response_resources_ = nullptr;
    response_resource_count_ = 0;
    response_resource_capacity_ = 0;
    if (response_bytes_ != nullptr)
        payload_allocator_.deallocate(response_bytes_, response_byte_count_, alignof(std::uint8_t));
    response_bytes_ = nullptr;
    response_byte_count_ = 0;
}

bool invocation::publish_locked(na_execution_outcome_t outcome, na_outcome_reason_t reason, std::uint8_t *bytes,
                                std::uint64_t byte_count, resource *resources, std::size_t resource_count,
                                std::int64_t protocol_error) noexcept
{
    if (phase_ == invocation_phase::ready || phase_ == invocation_phase::consumed || client_closed_ ||
        byte_count > max_response_bytes_ || resource_count > max_response_resources_ ||
        (byte_count != 0 && bytes == nullptr) || (resource_count != 0 && resources == nullptr))
        return false;
    phase_ = invocation_phase::ready;
    execution_outcome_ = outcome;
    outcome_reason_ = reason;
    protocol_error_ = protocol_error > 0 ? -protocol_error : protocol_error;
    response_bytes_ = bytes;
    response_byte_count_ = byte_count;
    response_resources_ = resources;
    response_resource_count_ = resource_count;
    response_resource_capacity_ = resource_count;
    return true;
}

bool invocation::valid_failure(na_execution_outcome_t outcome, na_outcome_reason_t reason,
                               std::int64_t protocol_error) noexcept
{
    if (protocol_error > 0 || protocol_error < -std::numeric_limits<std::int32_t>::max())
        return false;
    if (outcome == NA_EXECUTION_NONE && reason == NA_OUTCOME_REASON_NONE)
        return protocol_error < 0;
    if ((outcome != NA_EXECUTION_NOT_DELIVERED && outcome != NA_EXECUTION_OUTCOME_UNKNOWN) ||
        reason <= NA_OUTCOME_REASON_NONE || reason > NA_OUTCOME_REASON_UNSUPPORTED)
        return false;
    if (outcome == NA_EXECUTION_NOT_DELIVERED)
        return reason == NA_OUTCOME_REASON_PEER_CLOSED || reason == NA_OUTCOME_REASON_OBJECT_REVOKED ||
               reason == NA_OUTCOME_REASON_OPERATION_DEADLINE || reason == NA_OUTCOME_REASON_CANCEL_REQUESTED ||
               reason == NA_OUTCOME_REASON_REQUEST_DISCARDED || reason == NA_OUTCOME_REASON_RESPONDER_ABANDONED ||
               reason == NA_OUTCOME_REASON_PROTOCOL_VIOLATION || reason == NA_OUTCOME_REASON_UNSUPPORTED;
    return reason == NA_OUTCOME_REASON_PEER_CLOSED || reason == NA_OUTCOME_REASON_OBJECT_REVOKED ||
           reason == NA_OUTCOME_REASON_OPERATION_DEADLINE || reason == NA_OUTCOME_REASON_CANCEL_REQUESTED ||
           reason == NA_OUTCOME_REASON_RESPONDER_ABANDONED || reason == NA_OUTCOME_REASON_BROKER_FAILURE ||
           reason == NA_OUTCOME_REASON_PROTOCOL_VIOLATION || reason == NA_OUTCOME_REASON_UNSUPPORTED;
}

std::uint64_t invocation::signals() const noexcept
{
    lock_guard guard(const_cast<lock &>(lock_));
    std::uint64_t result = 0;
    if (phase_ == invocation_phase::ready || phase_ == invocation_phase::consumed)
        result |= NA_SIGNAL_COMPLETED;
    if (client_closed_ && phase_ != invocation_phase::consumed)
        result |= NA_SIGNAL_PEER_CLOSED;
    if (cancellation_requested_ && phase_ != invocation_phase::consumed)
        result |= NA_SIGNAL_CANCEL_REQUESTED;
    return result;
}

bool invocation::begin_receive() noexcept
{
    bool published = false;
    {
        lock_guard guard(lock_);
        if (phase_ != invocation_phase::queued)
            return false;
        if (cancellation_requested_ || client_closed_ || !responder_alive_)
            published = publish_locked(NA_EXECUTION_NOT_DELIVERED,
                                       cancellation_requested_ ? NA_OUTCOME_REASON_CANCEL_REQUESTED
                                                               : NA_OUTCOME_REASON_RESPONDER_ABANDONED,
                                       nullptr, 0, nullptr, 0, 0);
        else
        {
            phase_ = invocation_phase::receiving;
            return true;
        }
    }
    if (published)
        notify_state();
    return false;
}

void invocation::rollback_receive() noexcept
{
    bool published = false;
    {
        lock_guard guard(lock_);
        if (phase_ != invocation_phase::receiving)
            return;
        if (cancellation_requested_)
            published = publish_locked(NA_EXECUTION_NOT_DELIVERED, NA_OUTCOME_REASON_CANCEL_REQUESTED, nullptr, 0,
                                       nullptr, 0, 0);
        else
            phase_ = invocation_phase::queued;
    }
    if (published)
        notify_state();
}

bool invocation::finish_dispatch() noexcept
{
    bool published = false;
    {
        lock_guard guard(lock_);
        if (phase_ != invocation_phase::receiving)
            return false;
        // A responder can disappear after begin_receive() but before the
        // adapter commits delivery.  Do not leave the invocation in a
        // dispatched state whose result can never be produced.
        if (!responder_alive_)
        {
            published = publish_locked(NA_EXECUTION_OUTCOME_UNKNOWN, NA_OUTCOME_REASON_RESPONDER_ABANDONED, nullptr, 0,
                                       nullptr, 0, 0);
        }
        else
        {
            phase_ = invocation_phase::dispatched;
        }
    }
    if (published)
        notify_state();
    // The receive adapter treats false as "delivery could not be committed";
    // the already-published unknown outcome is terminal and must take that
    // path too.
    return !published;
}

void invocation::mark_dispatched() noexcept
{
    lock_guard guard(lock_);
    if (phase_ == invocation_phase::queued || phase_ == invocation_phase::receiving)
        phase_ = invocation_phase::dispatched;
}

bool invocation::cancellation_requested() const noexcept
{
    lock_guard guard(const_cast<lock &>(lock_));
    return cancellation_requested_;
}

bool invocation::execution_interrupted() const noexcept
{
    lock_guard guard(const_cast<lock &>(lock_));
    return cancellation_requested_ || client_closed_ || !responder_alive_ || phase_ == invocation_phase::ready ||
           phase_ == invocation_phase::consumed;
}

bool invocation::cancel() noexcept
{
    bool queued = false;
    bool wake = false;
    {
        lock_guard guard(lock_);
        if (phase_ == invocation_phase::ready || phase_ == invocation_phase::consumed)
            return false;
        cancellation_requested_ = true;
        queued = phase_ == invocation_phase::queued;
        wake = phase_ == invocation_phase::receiving || phase_ == invocation_phase::dispatched;
    }

    bool removed = false;
    if (queued && callbacks_.remove_queued != nullptr)
        removed = callbacks_.remove_queued(callbacks_.context);
    (void)removed;

    bool published = false;
    {
        lock_guard guard(lock_);
        if (phase_ == invocation_phase::queued)
            published = publish_locked(NA_EXECUTION_NOT_DELIVERED, NA_OUTCOME_REASON_CANCEL_REQUESTED, nullptr, 0,
                                       nullptr, 0, 0);
        wake = wake || phase_ == invocation_phase::receiving || phase_ == invocation_phase::dispatched;
    }
    if (published || wake)
        notify_state(wake);
    return true;
}

bool invocation::expire_if_due() noexcept
{
    if (!deadline_reached(clock_, operation_deadline_))
        return false;

    bool queued = false;
    bool wake = false;
    {
        lock_guard guard(lock_);
        queued = phase_ == invocation_phase::queued;
        wake = phase_ == invocation_phase::receiving || phase_ == invocation_phase::dispatched;
    }
    if (queued && callbacks_.remove_queued != nullptr)
        callbacks_.remove_queued(callbacks_.context);

    bool published = false;
    {
        lock_guard guard(lock_);
        if (phase_ == invocation_phase::queued)
            published = publish_locked(NA_EXECUTION_NOT_DELIVERED, NA_OUTCOME_REASON_OPERATION_DEADLINE, nullptr, 0,
                                       nullptr, 0, 0);
        else if (phase_ == invocation_phase::receiving || phase_ == invocation_phase::dispatched)
        {
            responder_alive_ = false;
            published = publish_locked(NA_EXECUTION_OUTCOME_UNKNOWN, NA_OUTCOME_REASON_OPERATION_DEADLINE, nullptr, 0,
                                       nullptr, 0, 0);
        }
    }
    if (published)
        notify_state(wake);
    return published;
}

void invocation::close_client() noexcept
{
    bool notify_needed = false;
    bool wake = false;
    {
        lock_guard guard(lock_);
        client_closed_ = true;
        wake = phase_ == invocation_phase::receiving || phase_ == invocation_phase::dispatched;
        if (!result_claimed_)
        {
            clear_response_locked();
            release_result_budget_locked();
        }
        notify_needed = true;
    }
    if (notify_needed)
        notify_state(wake);
}

void invocation::abandon_responder() noexcept
{
    bool published = false;
    bool wake = false;
    {
        lock_guard guard(lock_);
        if (!responder_alive_)
            return;
        responder_alive_ = false;
        wake = phase_ == invocation_phase::receiving || phase_ == invocation_phase::dispatched;
        if (phase_ == invocation_phase::queued)
            published = publish_locked(NA_EXECUTION_NOT_DELIVERED, NA_OUTCOME_REASON_RESPONDER_ABANDONED, nullptr, 0,
                                       nullptr, 0, 0);
        else if (phase_ == invocation_phase::dispatched)
            published = publish_locked(NA_EXECUTION_OUTCOME_UNKNOWN, NA_OUTCOME_REASON_RESPONDER_ABANDONED, nullptr, 0,
                                       nullptr, 0, 0);
    }
    if (published || wake)
        notify_state(wake);
}

bool invocation::consume_responder() noexcept
{
    lock_guard guard(lock_);
    if (!responder_alive_)
        return false;
    responder_alive_ = false;
    return true;
}

bool invocation::reserve_result_budget() noexcept
{
    lock_guard guard(lock_);
    if (result_budget_reserved_)
        return true;
    if (!owner_domain_->reserve(max_response_bytes_, max_response_resources_))
        return false;
    result_budget_reserved_ = true;
    return true;
}

bool invocation::response_within_limits(std::uint64_t bytes, std::uint64_t resources) const noexcept
{
    lock_guard guard(const_cast<lock &>(lock_));
    return bytes <= max_response_bytes_ && resources <= max_response_resources_;
}

bool invocation::complete_reply(const std::uint8_t *bytes, std::uint64_t byte_count, resource *resources,
                                std::size_t resource_count, std::int64_t protocol_error) noexcept
{
    if (byte_count > max_response_bytes_ || resource_count > max_response_resources_ ||
        (byte_count != 0 && bytes == nullptr) || (resource_count != 0 && resources == nullptr))
        return false;
    auto *copy = static_cast<std::uint8_t *>(
        byte_count == 0 ? nullptr : payload_allocator_.allocate(byte_count, alignof(std::uint8_t)));
    if (byte_count != 0 && copy == nullptr)
        return false;
    if (byte_count != 0)
        std::memcpy(copy, bytes, byte_count);

    resource *owned_resources = nullptr;
    if (resource_count != 0)
    {
        owned_resources = static_cast<resource *>(
            control_allocator_.allocate(sizeof(resource) * resource_count, alignof(resource)));
        if (owned_resources == nullptr)
        {
            if (copy != nullptr)
                payload_allocator_.deallocate(copy, byte_count, alignof(std::uint8_t));
            return false;
        }
        for (std::size_t index = 0; index < resource_count; index++)
            new (owned_resources + index) resource(std::move(resources[index]));
    }

    bool published = false;
    {
        lock_guard guard(lock_);
        published = publish_locked(NA_EXECUTION_NONE, NA_OUTCOME_REASON_NONE, copy, byte_count, owned_resources,
                                   resource_count, protocol_error);
    }
    if (!published)
    {
        for (std::size_t index = 0; index < resource_count; index++)
            resources[index] = std::move(owned_resources[index]);
        for (std::size_t index = 0; index < resource_count; index++)
            owned_resources[index].~resource();
        if (owned_resources != nullptr)
            control_allocator_.deallocate(owned_resources, sizeof(resource) * resource_count, alignof(resource));
        if (copy != nullptr)
            payload_allocator_.deallocate(copy, byte_count, alignof(std::uint8_t));
        return false;
    }
    notify_state();
    return true;
}

bool invocation::complete_failure(na_execution_outcome_t outcome, na_outcome_reason_t reason,
                                  std::int64_t protocol_error) noexcept
{
    if (!valid_failure(outcome, reason, protocol_error))
        return false;
    bool published = false;
    {
        lock_guard guard(lock_);
        published = publish_locked(outcome, reason, nullptr, 0, nullptr, 0, protocol_error);
    }
    if (published)
        notify_state();
    return published;
}

bool invocation::complete_not_delivered(na_outcome_reason_t reason) noexcept
{
    return complete_failure(NA_EXECUTION_NOT_DELIVERED, reason, 0);
}

na_status_t invocation::claim_result(std::uint64_t byte_capacity, std::uint64_t resource_capacity,
                                     std::uint64_t &actual_bytes, std::uint64_t &actual_resources,
                                     std::uint64_t &required_bytes, std::uint64_t &required_resources,
                                     na_execution_outcome_t &outcome, na_outcome_reason_t &reason,
                                     std::int64_t &protocol_error) noexcept
{
    expire_if_due();
    lock_guard guard(lock_);
    actual_bytes = response_byte_count_;
    actual_resources = response_resource_count_;
    required_bytes = response_byte_count_;
    required_resources = response_resource_count_;
    outcome = execution_outcome_;
    reason = outcome_reason_;
    protocol_error = protocol_error_;
    if (phase_ == invocation_phase::consumed)
        return NA_STATUS_ALREADY_CONSUMED;
    if (phase_ != invocation_phase::ready)
        return NA_STATUS_WOULD_BLOCK;
    if (result_claimed_)
        return NA_STATUS_WOULD_BLOCK;
    if (byte_capacity < response_byte_count_ || resource_capacity < response_resource_count_)
        return NA_STATUS_BUFFER_TOO_SMALL;
    result_claimed_ = true;
    actual_bytes = response_byte_count_;
    actual_resources = response_resource_count_;
    required_bytes = 0;
    required_resources = 0;
    return NA_STATUS_OK;
}

na_status_t invocation::take_result_bytes(std::uint8_t *&bytes, std::uint64_t &byte_count) noexcept
{
    bytes = nullptr;
    byte_count = 0;
    lock_guard guard(lock_);
    if (!result_claimed_ || phase_ != invocation_phase::ready)
        return NA_STATUS_WOULD_BLOCK;
    if (claimed_bytes_ != nullptr)
        return NA_STATUS_ALREADY_CONSUMED;
    claimed_bytes_ = response_bytes_;
    claimed_byte_count_ = response_byte_count_;
    response_bytes_ = nullptr;
    response_byte_count_ = 0;
    bytes = claimed_bytes_;
    byte_count = claimed_byte_count_;
    return NA_STATUS_OK;
}

na_status_t invocation::take_result_resource(std::uint64_t index, resource &result) noexcept
{
    lock_guard guard(lock_);
    if (!result_claimed_ || phase_ != invocation_phase::ready || index >= response_resource_count_)
        return NA_STATUS_INVALID_ARGUMENT;
    if (!response_resources_[index].valid())
        return NA_STATUS_INVALID_HANDLE;
    result = std::move(response_resources_[index]);
    return NA_STATUS_OK;
}

na_status_t invocation::restore_result(std::uint8_t *bytes, std::uint64_t byte_count, resource *resources,
                                       std::size_t resource_count) noexcept
{
    lock_guard guard(lock_);
    if (!result_claimed_ || phase_ != invocation_phase::ready || resource_count > response_resource_count_ ||
        (resource_count != 0 && resources == nullptr) || (bytes != nullptr && claimed_bytes_ != nullptr))
        return NA_STATUS_INVALID_ARGUMENT;
    if (bytes != nullptr)
    {
        response_bytes_ = bytes;
        response_byte_count_ = byte_count;
    }
    else if (claimed_bytes_ != nullptr)
    {
        response_bytes_ = claimed_bytes_;
        response_byte_count_ = claimed_byte_count_;
        claimed_bytes_ = nullptr;
        claimed_byte_count_ = 0;
    }
    for (std::size_t index = 0; index < resource_count; index++)
    {
        if (resources[index].valid() && response_resources_[index].valid())
            return NA_STATUS_INVALID_ARGUMENT;
    }
    for (std::size_t index = 0; index < resource_count; index++)
    {
        if (resources[index].valid())
            response_resources_[index] = std::move(resources[index]);
    }
    result_claimed_ = false;
    return NA_STATUS_OK;
}

na_status_t invocation::commit_result() noexcept
{
    {
        lock_guard guard(lock_);
        if (!result_claimed_ || phase_ != invocation_phase::ready)
            return phase_ == invocation_phase::consumed ? NA_STATUS_ALREADY_CONSUMED : NA_STATUS_WOULD_BLOCK;
        phase_ = invocation_phase::consumed;
        result_claimed_ = false;
        clear_response_locked();
        if (claimed_bytes_ != nullptr)
            payload_allocator_.deallocate(claimed_bytes_, claimed_byte_count_, alignof(std::uint8_t));
        claimed_bytes_ = nullptr;
        claimed_byte_count_ = 0;
        release_result_budget_locked();
    }
    notify_state();
    return NA_STATUS_OK;
}

} // namespace naos::ipc_core
