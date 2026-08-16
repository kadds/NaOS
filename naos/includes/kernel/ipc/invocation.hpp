#pragma once

#include "freelibcxx/vector.hpp"
#include "kernel/capability.hpp"
#include "kernel/ipc/bounded_queue.hpp"
#include "kernel/ipc/invocation_deadline.hpp"
#include "kernel/lock.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/resource.hpp"
#include "kernel/wait.hpp"
#include "naos/abi.h"
#include <atomic>

namespace naos::ipc
{

class protocol_state;
class invocation_state;
class responder_object;

class protocol_descriptor final : public kobject
{
  public:
    explicit protocol_descriptor(const na_protocol_descriptor_t &descriptor);

    static type_e type_of() { return type_e::protocol_descriptor; }

    const na_protocol_descriptor_t &descriptor() const { return descriptor_; }
    bool matches(const na_uuid_t &uuid) const;

  private:
    na_protocol_descriptor_t descriptor_{};
};

enum class endpoint_role : u8
{
    client,
    server,
};

class protocol_endpoint final : public kobject
{
  public:
    protocol_endpoint(handle_t<protocol_state> state, endpoint_role role);
    ~protocol_endpoint() override;

    protocol_endpoint(const protocol_endpoint &) = delete;
    protocol_endpoint &operator=(const protocol_endpoint &) = delete;

    static type_e type_of(endpoint_role role)
    {
        return role == endpoint_role::client ? type_e::protocol_client_end : type_e::protocol_server_end;
    }

    bool capability_is_unique() const override { return true; }
    void on_capability_acquire(capability::location where) override;
    void on_capability_release(capability::location where) override;
    void on_capability_handoff(capability::location from, capability::location to) override;
    na_signal_t capability_signals() const override;
    u64 capability_state() const override;

    protocol_state *state() { return state_.operator&(); }
    const protocol_state *state() const { return state_.operator&(); }
    const handle_t<protocol_state> &state_ref() const { return state_; }
    endpoint_role role() const { return role_; }
    void begin_operation();
    void end_operation();

  private:
    handle_t<protocol_state> state_;
    endpoint_role role_;
};

enum class invocation_phase : u8
{
    queued,
    receiving,
    dispatched,
    ready,
    consumed,
};

class invocation_state
{
  public:
    invocation_state(u64 method_id, u64 operation_budget, u64 max_response_bytes = NA_CHANNEL_MAX_MESSAGE_BYTES,
                     u64 max_response_resources = NA_CHANNEL_MAX_RESOURCES);
    ~invocation_state();

    invocation_state(const invocation_state &) = delete;
    invocation_state &operator=(const invocation_state &) = delete;

    na_signal_t signals() const;
    u64 method_id() const { return method_id_; }
    u64 operation_deadline() const { return operation_deadline_; }

    bool begin_receive();
    void rollback_receive();
    bool finish_dispatch();
    void mark_dispatched();
    void set_queue_owner(const handle_t<protocol_state> &owner);
    void clear_queue_owner();
    bool cancellation_requested() const;
    bool execution_interrupted() const;
    void set_execution_wait_queue(task::wait_queue_t *queue);
    void clear_execution_wait_queue(task::wait_queue_t *queue);
    bool cancel(protocol_state *queue_owner);
    na_status_t arm_deadline(const handle_t<invocation_state> &self);
    void expire_deadline();
    void close_client();
    void abandon_responder();
    bool consume_responder();
    bool reserve_result_budget();

    bool complete_reply(freelibcxx::vector<byte> &&bytes, capability::transfer_record_list &&resources,
                        i64 protocol_error = 0, task::resource_table_t *source_resources = nullptr);
    bool complete_reply(freelibcxx::vector<byte> &bytes, capability::transfer_record_list &resources,
                        i64 protocol_error = 0, task::resource_table_t *source_resources = nullptr);
    bool complete_failure(na_execution_outcome_t outcome, na_outcome_reason_t reason, i64 protocol_error = 0);
    bool complete_not_delivered(na_outcome_reason_t reason);
    bool deadline_expired(bool dispatched_unknown);
    bool response_within_limits(u64 bytes, u64 resources) const;

    na_status_t claim_result(na_result_frame_t &frame, freelibcxx::vector<byte> &bytes,
                             capability::transfer_record_list &resources);
    void restore_result(freelibcxx::vector<byte> &&bytes, capability::transfer_record_list &&resources);
    na_status_t commit_result();

  private:
    void release_result_budget_locked();
    bool publish_locked(na_execution_outcome_t outcome, na_outcome_reason_t reason, freelibcxx::vector<byte> &&bytes,
                        capability::transfer_record_list &&resources, i64 protocol_error);
    bool publish_locked_no_wake(na_execution_outcome_t outcome, na_outcome_reason_t reason,
                                freelibcxx::vector<byte> &&bytes, capability::transfer_record_list &&resources,
                                i64 protocol_error);

    mutable lock::spinlock_t lock_;
    task::wait_queue_t wait_queue_;
    invocation_phase phase_;
    u64 method_id_;
    u64 operation_deadline_;
    u64 max_response_bytes_;
    u64 max_response_resources_;
    bool result_claimed_;
    bool responder_alive_;
    bool client_closed_;
    bool cancellation_requested_;
    bool result_budget_reserved_;
    task::wait_queue_t *execution_wait_queue_ = nullptr;
    na_execution_outcome_t execution_outcome_;
    na_outcome_reason_t outcome_reason_;
    i64 protocol_error_;
    handle_t<protocol_state> queue_owner_;
    freelibcxx::vector<byte> response_bytes_;
    capability::transfer_record_list response_resources_;
};

class invocation_object final : public kobject
{
  public:
    explicit invocation_object(handle_t<invocation_state> state);
    ~invocation_object() override;

    static type_e type_of() { return type_e::invocation; }

    bool capability_is_unique() const override { return true; }
    void on_capability_release(capability::location where) override;
    na_signal_t capability_signals() const override;
    u64 capability_state() const override;

    invocation_state *state() { return state_.operator&(); }
    const invocation_state *state() const { return state_.operator&(); }
    handle_t<invocation_state> state_ref() const { return state_; }

  private:
    handle_t<invocation_state> state_;
};

class responder_object final : public kobject
{
  public:
    explicit responder_object(handle_t<invocation_state> state);
    ~responder_object() override;

    static type_e type_of() { return type_e::responder; }

    bool capability_is_unique() const override { return true; }
    void on_capability_release(capability::location where) override;
    na_signal_t capability_signals() const override;
    u64 capability_state() const override;

    invocation_state *state() { return state_.operator&(); }
    const invocation_state *state() const { return state_.operator&(); }
    bool consume()
    {
        if (!state_ || !state_->consume_responder())
            return false;
        consumed_ = true;
        return true;
    }

  private:
    handle_t<invocation_state> state_;
    bool consumed_;
};

struct invocation_request
{
    handle_t<invocation_state> state;
    u64 method_id = 0;
    u64 operation_deadline = 0;
    process_id caller_pid = 0;
    task::resource_table_t *source_resources = nullptr;
    // Queue accounting must survive delivery moving resources out of this
    // request into the receiver's table.
    u64 queued_resource_count = 0;
    freelibcxx::vector<byte> bytes;
    capability::transfer_record_list resources;
    capability::transferred_resource responder;

    invocation_request(freelibcxx::Allocator *allocator, handle_t<invocation_state> state, u64 method_id,
                       u64 operation_deadline, process_id caller_pid)
        : state(std::move(state))
        , method_id(method_id)
        , operation_deadline(operation_deadline)
        , caller_pid(caller_pid)
        , bytes(allocator)
        , resources(allocator)
    {
    }

    invocation_request(const invocation_request &) = delete;
    invocation_request &operator=(const invocation_request &) = delete;
};

class protocol_state
{
  public:
    protocol_state(const na_protocol_descriptor_t &descriptor, u64 max_messages, u64 max_bytes, u64 max_resources);
    ~protocol_state();

    protocol_state(const protocol_state &) = delete;
    protocol_state &operator=(const protocol_state &) = delete;

    const na_protocol_descriptor_t &descriptor() const { return descriptor_; }
    na_signal_t signals(endpoint_role role) const;
    na_status_t enqueue(invocation_request *request, bool *queued = nullptr);
    invocation_request *remove_queued(invocation_state *state);
    na_status_t claim(invocation_request *&request);
    bool cancel_claim(invocation_request *request);
    bool commit_claim(invocation_request *request);
    bool abort_claim(invocation_request *request);
    void endpoint_acquired(endpoint_role role, capability::location where);
    void endpoint_released(endpoint_role role, capability::location where);
    void begin_operation();
    void end_operation();
    void close_server_queue();
    void protocol_violation();
    bool valid() const { return valid_; }

  private:
    struct queue
    {
        queue(invocation_request **storage, u64 capacity)
            : storage(storage)
            , fifo(storage, capacity)
            , bytes(0)
            , resources(0)
        {
        }

        invocation_request **storage;
        bounded_queue<invocation_request *> fifo;
        u64 bytes;
        u64 resources;
    };

    na_protocol_descriptor_t descriptor_{};
    queue *queue_;
    u64 max_messages_;
    u64 max_bytes_;
    u64 max_resources_;
    mutable lock::spinlock_t lock_;
    std::atomic_uint64_t owners_[2];
    std::atomic_uint64_t roots_[2];
    std::atomic_uint64_t active_operations_;
    std::atomic_uint64_t active_claims_;
    bool valid_;
    bool server_closed_;
};

na_status_t create_protocol_descriptor(task::resource_table_t &resources, const na_protocol_descriptor_t *input,
                                       na_handle_t *output);
na_status_t create_protocol_endpoint(task::resource_table_t &resources, na_handle_t descriptor,
                                     const na_protocol_endpoint_options_t *options, na_handle_t *client,
                                     na_handle_t *server);
na_status_t create_protocol_endpoint_objects(const na_protocol_descriptor_t &descriptor,
                                             const na_protocol_endpoint_options_t *options, khandle &client,
                                             khandle &server, capability::metadata &client_metadata,
                                             capability::metadata &server_metadata);
na_status_t invoke_submit(task::resource_table_t &resources, na_handle_t target, const na_submit_frame_t *frame,
                          na_handle_t *invocation, bool oneway);
na_status_t receive_protocol(task::resource_table_t &resources, na_handle_t endpoint,
                             na_channel_receive_frame_t *frame);
na_status_t invocation_cancel(task::resource_table_t &resources, na_handle_t invocation);
na_status_t invocation_take_result(task::resource_table_t &resources, na_handle_t invocation, na_result_frame_t *frame);
na_status_t responder_reply(task::resource_table_t &resources, na_handle_t responder, const na_reply_frame_t *frame);
na_status_t responder_fail(task::resource_table_t &resources, na_handle_t responder, const na_fail_frame_t *frame);

void notify_invocation_waiters();
void init_kernel_dispatch_worker();

} // namespace naos::ipc
