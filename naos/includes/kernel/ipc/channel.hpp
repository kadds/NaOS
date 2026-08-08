#pragma once

#include "freelibcxx/vector.hpp"
#include "kernel/capability.hpp"
#include "kernel/ipc/bounded_queue.hpp"
#include "kernel/lock.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/resource.hpp"
#include "kernel/time.hpp"
#include "kernel/wait.hpp"
#include <atomic>

namespace naos::ipc
{

class channel_state;

class channel_message
{
  public:
    channel_message(u64 byte_count, u64 resource_count);
    ~channel_message();

    channel_message(const channel_message &) = delete;
    channel_message &operator=(const channel_message &) = delete;

    bool valid() const;
    bool append(capability::transferred_resource &&resource);

    byte *bytes() { return bytes_; }
    const byte *bytes() const { return bytes_; }
    u64 byte_count() const { return byte_count_; }
    u64 resource_count() const { return resources_.size(); }
    u64 resource_capacity() const { return resource_capacity_; }
    capability::transferred_resource &resource(u64 index) { return resources_[index]; }

  private:
    byte *bytes_;
    u64 byte_count_;
    u64 resource_capacity_;
    freelibcxx::vector<capability::transferred_resource> resources_;
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
    na_status_t claim_receive(u8 side, channel_message *&message);
    bool cancel_receive(u8 side, channel_message *message);
    bool commit_receive(u8 side, channel_message *message);
    bool discard(u8 side, channel_message *&message);

    void endpoint_object_created();
    void endpoint_object_destroyed();
    void capability_acquired(u8 side, capability::location where);
    void capability_released(u8 side, capability::location where);
    void begin_operation();
    void end_operation();

    bool has_root() const;
    bool can_reap() const;
    void collect_reachable_states(freelibcxx::vector<channel_state *> &targets) const;
    void discard_orphan_messages();
    u64 endpoint_object_count() const { return endpoint_objects_.load(); }

    u8 side_for(const raw_channel_endpoint *endpoint) const;

  private:
    struct queue
    {
        queue(channel_message **storage, u64 capacity)
            : storage(storage)
            , fifo(storage, capacity)
            , bytes(0)
            , resources(0)
        {
        }

        ~queue();
        queue(const queue &) = delete;
        queue &operator=(const queue &) = delete;

        channel_message **storage;
        naos::ipc::bounded_queue<channel_message *> fifo;
        u64 bytes;
        u64 resources;
    };

    queue *queues_[2];
    u64 max_messages_;
    u64 max_bytes_;
    u64 max_resources_;
    mutable lock::spinlock_t lock_;
    std::atomic_uint64_t owners_[2];
    std::atomic_uint64_t roots_[2];
    std::atomic_uint64_t active_operations_;
    std::atomic_uint64_t active_claims_;
    std::atomic_uint64_t endpoint_objects_;
    bool valid_;

  public:
    bool valid() const { return valid_; }
};

na_status_t create_raw_channel(khandle &left, khandle &right, const na_channel_options_t *options);

na_status_t send_raw_channel(task::resource_table_t &resources, na_handle_t endpoint,
                             const na_channel_send_frame_t *frame);
na_status_t receive_raw_channel(task::resource_table_t &resources, na_handle_t endpoint,
                                na_channel_receive_frame_t *frame);
na_status_t receive_raw_channel_kernel(task::resource_table_t &resources, na_handle_t endpoint, byte *bytes,
                                        u64 byte_capacity, u64 &actual_bytes,
                                        freelibcxx::vector<na_handle_t> &handles);
na_status_t discard_raw_channel(task::resource_table_t &resources, na_handle_t endpoint);
na_status_t wait_many(task::resource_table_t &resources, na_wait_item_t *items, u64 count,
                      timeclock::microsecond_t deadline);
na_status_t wait_for_signal(task::resource_table_t &resources, na_handle_t handle, na_signal_t signals,
                            timeclock::microsecond_t deadline);

void collect_orphaned_channels();
void notify_channel_waiters();

} // namespace naos::ipc
