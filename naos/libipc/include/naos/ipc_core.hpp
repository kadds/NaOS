#pragma once

#include <cstddef>
#include <cstdint>

#include <naos/abi.h>

namespace naos::ipc_core
{

class allocator
{
  public:
    virtual void *allocate(std::size_t size, std::size_t alignment) noexcept = 0;
    virtual void deallocate(void *pointer, std::size_t size, std::size_t alignment) noexcept = 0;

  protected:
    ~allocator() = default;
};

class lock
{
  public:
    virtual void acquire() noexcept = 0;
    virtual void release() noexcept = 0;

  protected:
    ~lock() = default;
};

class wait_notifier
{
  public:
    virtual void notify() noexcept = 0;

  protected:
    ~wait_notifier() = default;
};

class clock
{
  public:
    virtual std::uint64_t now() noexcept = 0;

  protected:
    ~clock() = default;
};

class resource
{
  public:
    using release_fn = void (*)(void *context, void *value);

    resource() = default;
    resource(void *context, void *value, release_fn release) noexcept
        : context_(context)
        , value_(value)
        , release_(release)
    {
    }

    resource(const resource &) = delete;
    resource &operator=(const resource &) = delete;

    resource(resource &&other) noexcept
        : context_(other.context_)
        , value_(other.value_)
        , release_(other.release_)
    {
        other.clear();
    }

    resource &operator=(resource &&other) noexcept
    {
        if (this == &other)
            return *this;
        reset();
        context_ = other.context_;
        value_ = other.value_;
        release_ = other.release_;
        other.clear();
        return *this;
    }

    ~resource() { reset(); }

    bool valid() const { return value_ != nullptr; }
    void *context() const { return context_; }
    void *value() const { return value_; }
    release_fn release_function() const { return release_; }

    void reset() noexcept
    {
        if (value_ != nullptr && release_ != nullptr)
            release_(context_, value_);
        clear();
    }

    void *take_value() noexcept
    {
        void *value = value_;
        clear();
        return value;
    }

  private:
    void clear() noexcept
    {
        context_ = nullptr;
        value_ = nullptr;
        release_ = nullptr;
    }

    void *context_ = nullptr;
    void *value_ = nullptr;
    release_fn release_ = nullptr;
};

class handle_table
{
  public:
    virtual na_status_t validate_waitable(std::uint64_t handle) noexcept = 0;
    virtual na_status_t begin_transfer(const std::uint64_t *handles, std::size_t count, void *transaction) noexcept = 0;
    virtual na_status_t restore_transfer(void *transaction) noexcept = 0;
    virtual void commit_transfer(void *transaction) noexcept = 0;

  protected:
    ~handle_table() = default;
};

class domain;
class channel;
class message;
class invocation;

enum class invocation_phase : std::uint8_t
{
    queued,
    receiving,
    dispatched,
    ready,
    consumed,
};

struct domain_config
{
    allocator *memory = nullptr;
    lock *synchronization = nullptr;
    std::uint64_t max_messages = 0;
    std::uint64_t max_bytes = 0;
    std::uint64_t max_resources = 0;
};

struct channel_config
{
    domain *owner_domain = nullptr;
    allocator *control_memory = nullptr;
    allocator *payload_memory = nullptr;
    lock *synchronization = nullptr;
    wait_notifier *notifier = nullptr;
    clock *timer = nullptr;
    handle_table *handles = nullptr;
    std::uint64_t max_messages = 0;
    std::uint64_t max_bytes = 0;
    std::uint64_t max_resources = 0;
};

class domain
{
  public:
    static domain *create(const domain_config &config) noexcept;
    void destroy() noexcept;
    bool valid() const noexcept { return valid_; }

  private:
    explicit domain(const domain_config &config) noexcept;
    ~domain() = default;

    allocator &allocator_;
    lock &lock_;
    std::uint64_t max_messages_;
    std::uint64_t max_bytes_;
    std::uint64_t max_resources_;
    std::uint64_t messages_ = 0;
    std::uint64_t bytes_ = 0;
    std::uint64_t resources_ = 0;
    bool valid_ = false;

    bool reserve(std::uint64_t bytes, std::uint64_t resources) noexcept;
    void release(std::uint64_t bytes, std::uint64_t resources) noexcept;
    friend class channel;
    friend class invocation;
};

class message
{
  public:
    static message *create(channel &channel, std::uint64_t byte_count, std::uint64_t resource_capacity) noexcept;
    void destroy() noexcept;

    bool valid() const noexcept { return valid_; }
    void set_user_context(void *context) noexcept { user_context_ = context; }
    void *user_context() const noexcept { return user_context_; }
    std::uint8_t *bytes() noexcept { return bytes_; }
    const std::uint8_t *bytes() const noexcept { return bytes_; }
    std::uint64_t byte_count() const noexcept { return byte_count_; }
    std::uint64_t resource_count() const noexcept { return resource_count_; }
    std::uint64_t resource_capacity() const noexcept { return resource_capacity_; }

    const resource *resource_at(std::uint64_t index) const noexcept;
    na_status_t take_resource(std::uint64_t index, resource &result) noexcept;
    na_status_t restore_resource(std::uint64_t index, resource &&value) noexcept;

  private:
    message(channel *owner, allocator &control_allocator, allocator &payload_allocator, std::uint64_t byte_count,
            std::uint64_t resource_capacity) noexcept;
    ~message();

    channel *owner_;
    allocator &control_allocator_;
    allocator &payload_allocator_;
    std::uint8_t *bytes_ = nullptr;
    std::uint64_t byte_count_ = 0;
    resource *resources_ = nullptr;
    std::uint64_t resource_count_ = 0;
    std::uint64_t resource_capacity_ = 0;
    void *user_context_ = nullptr;
    bool valid_ = false;

    friend class channel;
};

struct invocation_callbacks
{
    void *context = nullptr;
    bool (*remove_queued)(void *context) noexcept = nullptr;
    void (*wake_execution)(void *context) noexcept = nullptr;
};

struct invocation_config
{
    domain *owner_domain;
    allocator *control_memory;
    allocator *payload_memory;
    lock *synchronization;
    wait_notifier *notifier;
    clock *timer;
    invocation_callbacks callbacks;
    std::uint64_t method_id;
    std::uint64_t operation_deadline;
    std::uint64_t max_response_bytes;
    std::uint64_t max_response_resources;
};

class invocation
{
  public:
    static invocation *create(const invocation_config &config) noexcept;
    void destroy() noexcept;

    bool valid() const noexcept { return valid_; }
    std::uint64_t method_id() const noexcept { return method_id_; }
    std::uint64_t operation_deadline() const noexcept { return operation_deadline_; }
    std::uint64_t signals() const noexcept;

    bool begin_receive() noexcept;
    void rollback_receive() noexcept;
    bool finish_dispatch() noexcept;
    void mark_dispatched() noexcept;
    bool cancellation_requested() const noexcept;
    bool execution_interrupted() const noexcept;
    bool cancel() noexcept;
    bool expire_if_due() noexcept;
    void close_client() noexcept;
    void abandon_responder() noexcept;
    bool consume_responder() noexcept;
    bool reserve_result_budget() noexcept;
    bool response_within_limits(std::uint64_t bytes, std::uint64_t resources) const noexcept;

    bool complete_reply(const std::uint8_t *bytes, std::uint64_t byte_count, resource *resources,
                        std::size_t resource_count, std::int64_t protocol_error) noexcept;
    static bool valid_failure(na_execution_outcome_t outcome, na_outcome_reason_t reason,
                              std::int64_t protocol_error) noexcept;
    bool complete_failure(na_execution_outcome_t outcome, na_outcome_reason_t reason,
                          std::int64_t protocol_error) noexcept;
    bool complete_not_delivered(na_outcome_reason_t reason) noexcept;

    na_status_t claim_result(std::uint64_t byte_capacity, std::uint64_t resource_capacity, std::uint64_t &actual_bytes,
                             std::uint64_t &actual_resources, std::uint64_t &required_bytes,
                             std::uint64_t &required_resources, na_execution_outcome_t &outcome,
                             na_outcome_reason_t &reason, std::int64_t &protocol_error) noexcept;
    na_status_t take_result_bytes(std::uint8_t *&bytes, std::uint64_t &byte_count) noexcept;
    na_status_t take_result_resource(std::uint64_t index, resource &result) noexcept;
    na_status_t restore_result(std::uint8_t *bytes, std::uint64_t byte_count, resource *resources,
                               std::size_t resource_count) noexcept;
    na_status_t commit_result() noexcept;

  private:
    explicit invocation(const invocation_config &config) noexcept;
    ~invocation();

    void notify_state(bool wake_execution = false) noexcept;
    void release_result_budget_locked() noexcept;
    void clear_response_locked() noexcept;
    bool publish_locked(na_execution_outcome_t outcome, na_outcome_reason_t reason, std::uint8_t *bytes,
                        std::uint64_t byte_count, resource *resources, std::size_t resource_count,
                        std::int64_t protocol_error) noexcept;
    domain *owner_domain_;
    allocator &control_allocator_;
    allocator &payload_allocator_;
    lock &lock_;
    wait_notifier *notifier_;
    clock *clock_;
    invocation_callbacks callbacks_;
    std::uint64_t method_id_;
    std::uint64_t operation_deadline_;
    std::uint64_t max_response_bytes_;
    std::uint64_t max_response_resources_;
    std::uint8_t *response_bytes_ = nullptr;
    std::uint64_t response_byte_count_ = 0;
    resource *response_resources_ = nullptr;
    std::uint64_t response_resource_count_ = 0;
    std::uint64_t response_resource_capacity_ = 0;
    std::uint8_t *claimed_bytes_ = nullptr;
    std::uint64_t claimed_byte_count_ = 0;
    invocation_phase phase_ = invocation_phase::queued;
    bool result_claimed_ = false;
    bool responder_alive_ = true;
    bool client_closed_ = false;
    bool cancellation_requested_ = false;
    bool result_budget_reserved_ = false;
    na_execution_outcome_t execution_outcome_ = NA_EXECUTION_NONE;
    na_outcome_reason_t outcome_reason_ = NA_OUTCOME_REASON_NONE;
    std::int64_t protocol_error_ = 0;
    bool valid_ = false;
};

class channel
{
  public:
    static channel *create(const channel_config &config) noexcept;
    void destroy() noexcept;

    bool valid() const noexcept { return valid_; }
    std::uint64_t max_messages() const noexcept { return max_messages_; }
    std::uint64_t queued_messages(std::uint8_t side) const noexcept;
    std::uint64_t signals(std::uint8_t side) const noexcept;
    bool can_reap() const noexcept;

    void side_reference_acquired(std::uint8_t side) noexcept;
    void side_reference_released(std::uint8_t side) noexcept;
    void begin_operation() noexcept;
    void end_operation() noexcept;

    na_status_t enqueue(std::uint8_t sender, message &message, resource *resources, std::size_t resource_count,
                        std::uint64_t queue_message_limit = 0) noexcept;
    na_status_t claim_receive(std::uint8_t side, message *&message) noexcept;
    bool cancel_receive(std::uint8_t side, message &message) noexcept;
    bool commit_receive(std::uint8_t side, message &message) noexcept;
    bool discard(std::uint8_t side, message *&message) noexcept;

    using message_visitor = void (*)(void *context, message &message) noexcept;
    void visit_queued_messages(message_visitor visitor, void *context) const noexcept;

  private:
    struct queue;
    explicit channel(const channel_config &config) noexcept;
    ~channel();

    channel_config config_;
    queue *queues_ = nullptr;
    std::uint64_t max_messages_ = 0;
    std::uint64_t max_bytes_ = 0;
    std::uint64_t max_resources_ = 0;
    std::uint64_t side_references_[2] = {};
    std::uint64_t active_claims_ = 0;
    std::uint64_t active_operations_ = 0;
    bool valid_ = false;

    void clear_queue(queue &queue) noexcept;
    bool reserve_global(std::uint64_t bytes, std::uint64_t resources) noexcept;
    void release_global(std::uint64_t bytes, std::uint64_t resources) noexcept;
    friend class message;
};

/*
 * A bounded single-producer/single-consumer byte ring that lives entirely in
 * caller-provided storage.  It is an optional data-plane transport alongside
 * channel/invocation, never a replacement for them.  The steady-state
 * send/claim/commit path allocates nothing: the producer writes into a slot
 * and the consumer copies the published bytes out before validating them.
 *
 * Memory ordering is carried by the per-slot sequence word alone, so the
 * transport is correct on weakly-ordered CPUs and does not depend on x86 TSO:
 *   - publish edge: the producer stores `position + 1` with memory_order_release
 *     after writing the payload and metadata; the consumer's
 *     memory_order_acquire load of the same word observes the complete entry.
 *   - reclaim edge: the consumer stores `position + capacity` with
 *     memory_order_release once the slot is no longer pinned; the producer's
 *     memory_order_acquire load of the same word before reusing the slot
 *     guarantees it does not overwrite bytes still being read or validated.
 * The produce/consume counters are owner-written hints for fullness and the
 * queued count only; they never authorize reading or reusing a slot.
 */
inline constexpr std::uint64_t ring_max_slots = 256;
inline constexpr std::uint64_t ring_max_slot_bytes = 65536;
inline constexpr std::uint64_t ring_max_resources_per_slot = 64;

/* Layout-compatible with the C ABI's naos_ipc_resource_t; the ring stores
 * records, not live `resource` objects, because the storage may be shared. */
struct resource_record
{
    void *context = nullptr;
    void *value = nullptr;
    resource::release_fn release = nullptr;

    bool valid() const noexcept { return value != nullptr; }
    void clear() noexcept
    {
        context = nullptr;
        value = nullptr;
        release = nullptr;
    }
};

struct ring_config
{
    allocator *control_memory = nullptr;
    lock *synchronization = nullptr;
    wait_notifier *notifier = nullptr;
    clock *timer = nullptr;
    handle_table *handles = nullptr;
    void *storage = nullptr;
    std::uint64_t storage_bytes = 0;
    std::uint64_t slot_bytes = 0;
    std::uint32_t slot_capacity = 0;
    std::uint32_t resource_capacity = 0;
};

class ring
{
  public:
    static std::uint64_t required_bytes(std::uint64_t slot_bytes, std::uint32_t slot_capacity,
                                        std::uint32_t resource_capacity) noexcept;
    static na_status_t format(void *storage, std::uint64_t storage_bytes, std::uint64_t slot_bytes,
                              std::uint32_t slot_capacity, std::uint32_t resource_capacity) noexcept;
    static ring *create(const ring_config &config) noexcept;
    void destroy() noexcept;

    bool valid() const noexcept { return valid_; }
    std::uint64_t slot_capacity() const noexcept { return slot_capacity_; }
    std::uint64_t slot_bytes() const noexcept { return slot_bytes_; }
    std::uint64_t resource_capacity() const noexcept { return resource_capacity_; }
    std::uint64_t queued() const noexcept;
    bool claim_pending() const noexcept { return claimed_; }
    std::uint64_t signals(std::uint8_t side) const noexcept;

    na_status_t send(const std::uint8_t *bytes, std::uint64_t byte_count) noexcept;
    na_status_t send(const std::uint8_t *bytes, std::uint64_t byte_count, resource_record *resources,
                     std::size_t resource_count) noexcept;
    na_status_t claim(std::uint8_t *destination, std::uint64_t byte_capacity, std::uint64_t &byte_count,
                      std::uint64_t &resource_count) noexcept;
    na_status_t take_resource(std::uint64_t index, resource_record &result) noexcept;
    na_status_t commit() noexcept;
    na_status_t cancel() noexcept;
    void close(std::uint8_t side) noexcept;

  private:
    ring(const ring_config &config) noexcept;
    ~ring() = default;

    std::uint8_t *slot_base(std::uint64_t index) const noexcept;
    void release_slot_resources(std::uint64_t index) noexcept;

    allocator &control_allocator_;
    lock &lock_;
    wait_notifier *notifier_;
    std::uint8_t *storage_ = nullptr;
    void *control_ = nullptr;
    std::uint64_t slots_offset_ = 0;
    std::uint64_t payload_offset_ = 0;
    std::uint64_t resource_offset_ = 0;
    std::uint64_t slot_stride_ = 0;
    std::uint64_t slot_bytes_ = 0;
    std::uint32_t slot_capacity_ = 0;
    std::uint32_t resource_capacity_ = 0;
    bool claimed_ = false;
    std::uint64_t claimed_position_ = 0;
    std::uint64_t claimed_resources_ = 0;
    bool valid_ = false;
};

} // namespace naos::ipc_core
