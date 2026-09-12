#pragma once

#include "freelibcxx/vector.hpp"
#include "freelibcxx/function_ref.hpp"
#include "kernel/capability.hpp"
#include "kernel/ipc/core_lock.hpp"
#include "kernel/lock.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/resource.hpp"
#include "kernel/time.hpp"
#include "kernel/wait.hpp"
#include "naos/ipc_core.h"

namespace naos::ipc
{

class channel_state;

class channel_message
{
  public:
    channel_message(naos_ipc_channel_t *channel, u64 byte_count, u64 resource_count);
    ~channel_message();

    channel_message(const channel_message &) = delete;
    channel_message &operator=(const channel_message &) = delete;

    bool valid() const;

    byte *bytes();
    const byte *bytes() const;
    u64 byte_count() const;
    u64 resource_count() const;
    u64 resource_capacity() const;
    naos_ipc_message_t *core_message() { return message_; }

  private:
    naos_ipc_message_t *message_;
};

class raw_channel_endpoint : public kobject
{
  public:
    raw_channel_endpoint(channel_state *state, u8 side);
    ~raw_channel_endpoint() override;

    raw_channel_endpoint(const raw_channel_endpoint &) = delete;
    raw_channel_endpoint &operator=(const raw_channel_endpoint &) = delete;

    static type_e type_of() { return type_e::raw_channel_end; }

    bool capability_is_unique() const override { return true; }
    void on_capability_acquire(capability::location where) override;
    void on_capability_release(capability::location where) override;
    void on_capability_handoff(capability::location from, capability::location to) override;
    na_signal_t capability_signals() const override;

    channel_state *state() const { return state_; }
    u8 side() const { return side_; }
    void begin_operation();
    void end_operation();

  private:
    channel_state *state_;
    u8 side_;
};

class channel_state
{
  public:
    channel_state(u64 max_messages, u64 max_bytes, u64 max_resources);
    ~channel_state();

    channel_state(const channel_state &) = delete;
    channel_state &operator=(const channel_state &) = delete;

    na_signal_t signals(u8 side) const;
    na_status_t enqueue(u8 sender, channel_message *message, capability::transfer_record_list &records,
                        task::resource_table_t &resources);
    na_status_t enqueue_kernel(u8 sender, channel_message *message, capability::transfer_record_list &records,
                               u64 max_queue_messages = 0);
    na_status_t claim_receive(u8 side, channel_message *&message);
    bool cancel_receive(u8 side, channel_message *message);
    bool commit_receive(u8 side, channel_message *message);
    bool discard(u8 side, channel_message *&message);

    void endpoint_object_created(raw_channel_endpoint *endpoint);
    void endpoint_object_destroyed(raw_channel_endpoint *endpoint);
    void kernel_owner_acquired(u8 side);
    void kernel_owner_released(u8 side);
    void capability_acquired(u8 side, capability::location where);
    void capability_released(u8 side, capability::location where);
    void begin_operation();
    void end_operation();

    bool has_root() const;
    bool can_reap() const;
    u64 queued_messages(u8 side) const;
    u64 max_messages() const;
    void collect_reachable_states(freelibcxx::vector<channel_state *> &targets) const;
    void discard_orphan_messages();
    u64 endpoint_object_count() const;
    void notify_readiness();
    void notify_waiters();
    task::wait_queue_t &wait_queue() { return wait_queue_; }

    // The orphan collector owns this intrusive registry linkage.  Keeping it
    // in the state object makes registry insertion/removal allocation-free
    // while its raw spinlock is held.
    channel_state *registry_next() const { return registry_next_; }
    void set_registry_next(channel_state *next) { registry_next_ = next; }
    bool registry_linked() const { return registry_linked_; }
    void set_registry_linked(bool linked) { registry_linked_ = linked; }

    u8 side_for(const raw_channel_endpoint *endpoint) const;

  private:
    core_lock core_lock_;
    naos_ipc_lock_t core_lock_api_;
    ::lock::spinlock_t lifecycle_lock_;
    task::wait_queue_t wait_queue_;
    u64 endpoint_objects_ = 0;
    raw_channel_endpoint *endpoints_[2] = {};
    u64 roots_[2] = {};
    naos_ipc_wait_notifier_t core_notifier_api_;
    naos_ipc_channel_t *channel_;
    channel_state *registry_next_ = nullptr;
    bool registry_linked_ = false;

  public:
    bool valid() const;
    naos_ipc_channel_t *core_channel() const { return channel_; }
};

na_status_t create_raw_channel(khandle &left, khandle &right, const na_channel_options_t *options);
// Kernel callers already own their options in trusted memory and must not pass
// them through the usercopy-based syscall helper above.
na_status_t create_raw_channel_kernel(khandle &left, khandle &right, const na_channel_options_t *options);

na_status_t send_raw_channel(task::resource_table_t &resources, na_handle_t endpoint,
                             const na_channel_send_frame_t *frame);
na_status_t receive_raw_channel(task::resource_table_t &resources, na_handle_t endpoint,
                                na_channel_receive_frame_t *frame);
na_status_t receive_raw_channel_kernel(task::resource_table_t &resources, na_handle_t endpoint, byte *bytes,
                                       u64 byte_capacity, u64 &actual_bytes, freelibcxx::vector<na_handle_t> &handles);
na_status_t send_raw_channel_kernel(task::resource_table_t &resources, na_handle_t endpoint, const byte *bytes,
                                    u64 byte_count);
na_status_t send_raw_channel_kernel(const khandle &endpoint, const byte *bytes, u64 byte_count);
na_status_t discard_raw_channel(task::resource_table_t &resources, na_handle_t endpoint);
na_status_t wait_for_condition(task::wait_queue_t &queue, freelibcxx::function_ref<bool()> condition,
                               timeclock::microsecond_t deadline);
na_status_t wait_for_raw_channel(task::resource_table_t &resources, na_handle_t handle, na_signal_t signals,
                                 timeclock::microsecond_t deadline);

void collect_orphaned_channels();

} // namespace naos::ipc
