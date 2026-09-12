#ifndef NAOS_IPC_CORE_H
#define NAOS_IPC_CORE_H

/*
 * Transport- and capability-neutral channel state machine.
 *
 * This header is deliberately C-only.  The implementation may be C++, but
 * neither the kernel adapter nor the hosted Rust adapter has to understand
 * C++ object layout, exceptions, or capability objects.  Ownership of a
 * resource remains with the adapter that supplied its release callback.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t naos_ipc_status_t;
typedef uint64_t naos_ipc_signal_t;

enum
{
    NAOS_IPC_STATUS_OK = 0,
    NAOS_IPC_STATUS_INVALID_HANDLE = 1,
    NAOS_IPC_STATUS_WRONG_BINDING = 2,
    NAOS_IPC_STATUS_WRONG_SCOPE = 3,
    NAOS_IPC_STATUS_ACCESS_DENIED = 4,
    NAOS_IPC_STATUS_INVALID_ARGUMENT = 5,
    NAOS_IPC_STATUS_INVALID_MESSAGE = 6,
    NAOS_IPC_STATUS_BUFFER_TOO_SMALL = 7,
    NAOS_IPC_STATUS_WOULD_BLOCK = 8,
    NAOS_IPC_STATUS_WAIT_TIMED_OUT = 9,
    NAOS_IPC_STATUS_RESOURCE_EXHAUSTED = 10,
    NAOS_IPC_STATUS_FAULT = 11,
    NAOS_IPC_STATUS_OBJECT_REVOKED = 12,
    NAOS_IPC_STATUS_PEER_CLOSED = 13,
    NAOS_IPC_STATUS_ALREADY_CONSUMED = 14,
    NAOS_IPC_STATUS_NOT_SUPPORTED = 15,
    NAOS_IPC_STATUS_IO_ERROR = 16,
    NAOS_IPC_STATUS_WAIT_SET_INVALIDATED = 17,
};

enum
{
    NAOS_IPC_SIGNAL_READABLE = UINT64_C(1) << 0,
    NAOS_IPC_SIGNAL_WRITABLE = UINT64_C(1) << 1,
    NAOS_IPC_SIGNAL_PEER_CLOSED = UINT64_C(1) << 2,
    NAOS_IPC_SIGNAL_COMPLETED = UINT64_C(1) << 4,
    NAOS_IPC_SIGNAL_CANCEL_REQUESTED = UINT64_C(1) << 5,
};

typedef void *(*naos_ipc_allocate_fn)(void *context, size_t size, size_t alignment);
typedef void (*naos_ipc_deallocate_fn)(void *context, void *pointer, size_t size, size_t alignment);

typedef struct naos_ipc_allocator
{
    void *context;
    naos_ipc_allocate_fn allocate;
    naos_ipc_deallocate_fn deallocate;
} naos_ipc_allocator_t;

typedef void (*naos_ipc_lock_fn)(void *context);

typedef struct naos_ipc_lock
{
    void *context;
    naos_ipc_lock_fn acquire;
    naos_ipc_lock_fn release;
} naos_ipc_lock_t;

typedef void (*naos_ipc_notify_fn)(void *context);

typedef struct naos_ipc_wait_notifier
{
    void *context;
    naos_ipc_notify_fn notify;
} naos_ipc_wait_notifier_t;

typedef uint64_t (*naos_ipc_clock_now_fn)(void *context);

typedef struct naos_ipc_clock
{
    void *context;
    naos_ipc_clock_now_fn now;
} naos_ipc_clock_t;

/*
 * HandleTable intentionally knows only opaque numeric handles.  It does not
 * define capability rights or fd semantics.  Adapters may use these hooks to
 * validate a wait set and to stage an already-authorized resource transfer.
 */
typedef naos_ipc_status_t (*naos_ipc_validate_waitable_fn)(void *context, uint64_t handle);
typedef naos_ipc_status_t (*naos_ipc_begin_transfer_fn)(void *context, const uint64_t *handles, size_t count,
                                                        void **transaction);
typedef naos_ipc_status_t (*naos_ipc_restore_transfer_fn)(void *context, void *transaction);
typedef void (*naos_ipc_commit_transfer_fn)(void *context, void *transaction);

typedef struct naos_ipc_handle_table
{
    void *context;
    naos_ipc_validate_waitable_fn validate_waitable;
    naos_ipc_begin_transfer_fn begin_transfer;
    naos_ipc_restore_transfer_fn restore_transfer;
    naos_ipc_commit_transfer_fn commit_transfer;
} naos_ipc_handle_table_t;

typedef void (*naos_ipc_resource_release_fn)(void *context, void *value);

typedef struct naos_ipc_resource
{
    void *context;
    void *value;
    naos_ipc_resource_release_fn release;
} naos_ipc_resource_t;

typedef struct naos_ipc_domain naos_ipc_domain_t;
typedef struct naos_ipc_channel naos_ipc_channel_t;
typedef struct naos_ipc_message naos_ipc_message_t;
typedef struct naos_ipc_invocation naos_ipc_invocation_t;
typedef struct naos_ipc_ring naos_ipc_ring_t;

typedef void (*naos_ipc_resource_visitor_fn)(void *context, uint64_t index, void *resource_context,
                                             void *resource_value);
typedef void (*naos_ipc_message_visitor_fn)(void *context, naos_ipc_message_t *message);

enum
{
    NAOS_IPC_EXECUTION_NONE = 0,
    NAOS_IPC_EXECUTION_NOT_DELIVERED = 1,
    NAOS_IPC_EXECUTION_OUTCOME_UNKNOWN = 2,
};

enum
{
    NAOS_IPC_OUTCOME_REASON_NONE = 0,
    NAOS_IPC_OUTCOME_REASON_PEER_CLOSED = 1,
    NAOS_IPC_OUTCOME_REASON_OBJECT_REVOKED = 2,
    NAOS_IPC_OUTCOME_REASON_OPERATION_DEADLINE = 3,
    NAOS_IPC_OUTCOME_REASON_CANCEL_REQUESTED = 4,
    NAOS_IPC_OUTCOME_REASON_REQUEST_DISCARDED = 5,
    NAOS_IPC_OUTCOME_REASON_RESPONDER_ABANDONED = 6,
    NAOS_IPC_OUTCOME_REASON_BROKER_FAILURE = 7,
    NAOS_IPC_OUTCOME_REASON_PROTOCOL_VIOLATION = 8,
    NAOS_IPC_OUTCOME_REASON_UNSUPPORTED = 9,
};

typedef int (*naos_ipc_invocation_remove_queued_fn)(void *context);
typedef void (*naos_ipc_invocation_wake_execution_fn)(void *context);

typedef struct naos_ipc_invocation_callbacks
{
    void *context;
    naos_ipc_invocation_remove_queued_fn remove_queued;
    naos_ipc_invocation_wake_execution_fn wake_execution;
} naos_ipc_invocation_callbacks_t;

typedef struct naos_ipc_invocation_config
{
    naos_ipc_domain_t *owner_domain;
    const naos_ipc_allocator_t *control_memory;
    const naos_ipc_allocator_t *payload_memory;
    const naos_ipc_lock_t *synchronization;
    const naos_ipc_wait_notifier_t *notifier;
    const naos_ipc_clock_t *clock;
    const naos_ipc_invocation_callbacks_t *callbacks;
    uint64_t method_id;
    uint64_t operation_deadline;
    uint64_t max_response_bytes;
    uint64_t max_response_resources;
} naos_ipc_invocation_config_t;

typedef struct naos_ipc_invocation_result_info
{
    uint64_t method_id;
    uint64_t actual_bytes;
    uint64_t actual_resources;
    uint64_t required_bytes;
    uint64_t required_resources;
    uint32_t execution_outcome;
    uint32_t outcome_reason;
    int64_t protocol_error;
} naos_ipc_invocation_result_info_t;

typedef struct naos_ipc_domain_config
{
    const naos_ipc_allocator_t *memory;
    const naos_ipc_lock_t *synchronization;
    uint64_t max_messages;
    uint64_t max_bytes;
    uint64_t max_resources;
} naos_ipc_domain_config_t;

typedef struct naos_ipc_channel_config
{
    naos_ipc_domain_t *owner_domain;
    const naos_ipc_allocator_t *control_memory;
    const naos_ipc_allocator_t *payload_memory;
    const naos_ipc_lock_t *synchronization;
    const naos_ipc_wait_notifier_t *notifier;
    const naos_ipc_clock_t *clock;
    const naos_ipc_handle_table_t *handles;
    uint64_t max_messages;
    uint64_t max_bytes;
    uint64_t max_resources;
} naos_ipc_channel_config_t;

naos_ipc_domain_t *naos_ipc_domain_create(const naos_ipc_domain_config_t *config);
void naos_ipc_domain_destroy(naos_ipc_domain_t *domain);

naos_ipc_channel_t *naos_ipc_channel_create(const naos_ipc_channel_config_t *config);
void naos_ipc_channel_destroy(naos_ipc_channel_t *channel);
int naos_ipc_channel_valid(const naos_ipc_channel_t *channel);
uint64_t naos_ipc_channel_max_messages(const naos_ipc_channel_t *channel);
uint64_t naos_ipc_channel_queued_messages(const naos_ipc_channel_t *channel, uint8_t side);
naos_ipc_signal_t naos_ipc_channel_signals(const naos_ipc_channel_t *channel, uint8_t side);
int naos_ipc_channel_can_reap(const naos_ipc_channel_t *channel);

void naos_ipc_channel_side_reference_acquired(naos_ipc_channel_t *channel, uint8_t side);
void naos_ipc_channel_side_reference_released(naos_ipc_channel_t *channel, uint8_t side);
void naos_ipc_channel_begin_operation(naos_ipc_channel_t *channel);
void naos_ipc_channel_end_operation(naos_ipc_channel_t *channel);

naos_ipc_message_t *naos_ipc_message_create(naos_ipc_channel_t *channel, uint64_t byte_count,
                                            uint64_t resource_capacity);
void naos_ipc_message_destroy(naos_ipc_message_t *message);
int naos_ipc_message_valid(const naos_ipc_message_t *message);
uint8_t *naos_ipc_message_bytes(naos_ipc_message_t *message);
const uint8_t *naos_ipc_message_const_bytes(const naos_ipc_message_t *message);
uint64_t naos_ipc_message_byte_count(const naos_ipc_message_t *message);
uint64_t naos_ipc_message_resource_count(const naos_ipc_message_t *message);
uint64_t naos_ipc_message_resource_capacity(const naos_ipc_message_t *message);
void naos_ipc_message_set_user_context(naos_ipc_message_t *message, void *context);
void *naos_ipc_message_user_context(const naos_ipc_message_t *message);
void naos_ipc_message_visit_resources(const naos_ipc_message_t *message, naos_ipc_resource_visitor_fn visitor,
                                      void *context);
naos_ipc_status_t naos_ipc_message_take_resource(naos_ipc_message_t *message, uint64_t index,
                                                 naos_ipc_resource_t *resource);
naos_ipc_status_t naos_ipc_message_restore_resource(naos_ipc_message_t *message, uint64_t index,
                                                    naos_ipc_resource_t *resource);

void naos_ipc_resource_reset(naos_ipc_resource_t *resource);
int naos_ipc_resource_valid(const naos_ipc_resource_t *resource);

naos_ipc_status_t naos_ipc_channel_enqueue(naos_ipc_channel_t *channel, uint8_t sender, naos_ipc_message_t *message,
                                           naos_ipc_resource_t *resources, size_t resource_count,
                                           uint64_t queue_message_limit);
naos_ipc_status_t naos_ipc_channel_claim_receive(naos_ipc_channel_t *channel, uint8_t side,
                                                 naos_ipc_message_t **message);
int naos_ipc_channel_cancel_receive(naos_ipc_channel_t *channel, uint8_t side, naos_ipc_message_t *message);
int naos_ipc_channel_commit_receive(naos_ipc_channel_t *channel, uint8_t side, naos_ipc_message_t *message);
int naos_ipc_channel_discard(naos_ipc_channel_t *channel, uint8_t side, naos_ipc_message_t **message);
void naos_ipc_channel_visit_queued_messages(const naos_ipc_channel_t *channel, naos_ipc_message_visitor_fn visitor,
                                            void *context);

/*
 * Optional single-producer/single-consumer byte ring transport.
 *
 * The ring is not a replacement for the channel state machine: it is a
 * bounded data-plane transport that lives entirely in caller-provided
 * storage, so two address spaces can map the same bytes.  It performs no
 * dynamic allocation on the steady-state send/claim/commit path and raises
 * no kernel object.  Control, handles, admission and the invocation
 * lifecycle stay on the channel/invocation path above.
 *
 * Side 0 is the producer, side 1 the consumer.  Only one thread may call
 * send and only one thread may call claim/take_resource/commit/cancel;
 * the two roles may run concurrently.  queued/signals are read-only and
 * may be called from either role.
 */
#define NAOS_IPC_RING_MAX_SLOTS ((uint32_t)256)
#define NAOS_IPC_RING_MAX_SLOT_BYTES ((uint64_t)65536)
#define NAOS_IPC_RING_MAX_RESOURCES_PER_SLOT ((uint32_t)64)

typedef struct naos_ipc_ring_config
{
    const naos_ipc_allocator_t *control_memory;
    const naos_ipc_lock_t *synchronization;
    const naos_ipc_wait_notifier_t *notifier;
    const naos_ipc_clock_t *clock;
    const naos_ipc_handle_table_t *handles;
    void *storage;
    uint64_t storage_bytes;
    uint64_t slot_bytes;
    uint32_t slot_capacity;
    uint32_t resource_capacity;
} naos_ipc_ring_config_t;

/* Bytes required for a ring of this geometry.  Format the same buffer with
 * naos_ipc_ring_init before attaching it. */
uint64_t naos_ipc_ring_required_bytes(uint64_t slot_bytes, uint32_t slot_capacity, uint32_t resource_capacity);
/* Formats caller storage.  Must run once, with exclusive access, before the
 * storage is shared; re-running with the same geometry is a no-op and a
 * different geometry on live storage is rejected. */
naos_ipc_status_t naos_ipc_ring_init(void *storage, uint64_t storage_bytes, uint64_t slot_bytes,
                                     uint32_t slot_capacity, uint32_t resource_capacity);

naos_ipc_ring_t *naos_ipc_ring_create(const naos_ipc_ring_config_t *config);
void naos_ipc_ring_destroy(naos_ipc_ring_t *ring);
int naos_ipc_ring_valid(const naos_ipc_ring_t *ring);
uint64_t naos_ipc_ring_slot_capacity(const naos_ipc_ring_t *ring);
uint64_t naos_ipc_ring_slot_bytes(const naos_ipc_ring_t *ring);
uint64_t naos_ipc_ring_resource_capacity(const naos_ipc_ring_t *ring);
uint64_t naos_ipc_ring_queued(const naos_ipc_ring_t *ring);
int naos_ipc_ring_claim_pending(const naos_ipc_ring_t *ring);
naos_ipc_signal_t naos_ipc_ring_signals(const naos_ipc_ring_t *ring, uint8_t side);

/* Producer path.  WOULD_BLOCK on a full ring, PEER_CLOSED once side 1 is
 * closed, INVALID_MESSAGE when the payload or resource count exceeds the
 * slot geometry.  On success every resource is moved out of the caller's
 * array. */
naos_ipc_status_t naos_ipc_ring_send(naos_ipc_ring_t *ring, const uint8_t *bytes, uint64_t byte_count,
                                     naos_ipc_resource_t *resources, size_t resource_count);

/* Two-phase consumer path.  claim copies the published payload out to the
 * caller's buffer and pins the slot; the consumer validates the private
 * copy and only then commits (or cancels) it.  Because the slot stays
 * pinned, the producer cannot overwrite the bytes being validated.
 * BUFFER_TOO_SMALL reports the required byte_count without claiming. */
naos_ipc_status_t naos_ipc_ring_claim(naos_ipc_ring_t *ring, uint8_t *destination, uint64_t byte_capacity,
                                      uint64_t *byte_count, uint64_t *resource_count);
naos_ipc_status_t naos_ipc_ring_take_resource(naos_ipc_ring_t *ring, uint64_t index, naos_ipc_resource_t *resource);
naos_ipc_status_t naos_ipc_ring_commit(naos_ipc_ring_t *ring);
naos_ipc_status_t naos_ipc_ring_cancel(naos_ipc_ring_t *ring);
void naos_ipc_ring_close(naos_ipc_ring_t *ring, uint8_t side);

naos_ipc_invocation_t *naos_ipc_invocation_create(const naos_ipc_invocation_config_t *config);
void naos_ipc_invocation_destroy(naos_ipc_invocation_t *invocation);
int naos_ipc_invocation_valid(const naos_ipc_invocation_t *invocation);
uint64_t naos_ipc_invocation_method_id(const naos_ipc_invocation_t *invocation);
uint64_t naos_ipc_invocation_operation_deadline(const naos_ipc_invocation_t *invocation);
naos_ipc_signal_t naos_ipc_invocation_signals(const naos_ipc_invocation_t *invocation);
int naos_ipc_invocation_begin_receive(naos_ipc_invocation_t *invocation);
void naos_ipc_invocation_rollback_receive(naos_ipc_invocation_t *invocation);
int naos_ipc_invocation_finish_dispatch(naos_ipc_invocation_t *invocation);
void naos_ipc_invocation_mark_dispatched(naos_ipc_invocation_t *invocation);
int naos_ipc_invocation_cancellation_requested(const naos_ipc_invocation_t *invocation);
int naos_ipc_invocation_execution_interrupted(const naos_ipc_invocation_t *invocation);
int naos_ipc_invocation_cancel(naos_ipc_invocation_t *invocation);
int naos_ipc_invocation_expire_if_due(naos_ipc_invocation_t *invocation);
void naos_ipc_invocation_close_client(naos_ipc_invocation_t *invocation);
void naos_ipc_invocation_abandon_responder(naos_ipc_invocation_t *invocation);
int naos_ipc_invocation_consume_responder(naos_ipc_invocation_t *invocation);
int naos_ipc_invocation_reserve_result_budget(naos_ipc_invocation_t *invocation);
int naos_ipc_invocation_response_within_limits(const naos_ipc_invocation_t *invocation, uint64_t bytes,
                                               uint64_t resources);
int naos_ipc_invocation_complete_reply(naos_ipc_invocation_t *invocation, const uint8_t *bytes, uint64_t byte_count,
                                       naos_ipc_resource_t *resources, size_t resource_count, int64_t protocol_error);
int naos_ipc_invocation_failure_valid(uint32_t execution_outcome, uint32_t outcome_reason, int64_t protocol_error);
int naos_ipc_invocation_complete_failure(naos_ipc_invocation_t *invocation, uint32_t execution_outcome,
                                         uint32_t outcome_reason, int64_t protocol_error);
int naos_ipc_invocation_complete_not_delivered(naos_ipc_invocation_t *invocation, uint32_t outcome_reason);
naos_ipc_status_t naos_ipc_invocation_claim_result(naos_ipc_invocation_t *invocation, uint64_t byte_capacity,
                                                   uint64_t resource_capacity, naos_ipc_invocation_result_info_t *info);
naos_ipc_status_t naos_ipc_invocation_take_result_bytes(naos_ipc_invocation_t *invocation, uint8_t **bytes,
                                                        uint64_t *byte_count);
naos_ipc_status_t naos_ipc_invocation_take_result_resource(naos_ipc_invocation_t *invocation, uint64_t index,
                                                           naos_ipc_resource_t *resource);
naos_ipc_status_t naos_ipc_invocation_restore_result(naos_ipc_invocation_t *invocation, uint8_t *bytes,
                                                     uint64_t byte_count, naos_ipc_resource_t *resources,
                                                     size_t resource_count);
naos_ipc_status_t naos_ipc_invocation_commit_result(naos_ipc_invocation_t *invocation);

/* Adapter-side wait-set validation; the core does not implement a wait queue. */
naos_ipc_status_t naos_ipc_validate_wait_set(const naos_ipc_handle_table_t *handles, const uint64_t *values,
                                             size_t count);
uint64_t naos_ipc_clock_now(const naos_ipc_clock_t *clock);

#ifdef __cplusplus
}
#endif

#endif /* NAOS_IPC_CORE_H */
