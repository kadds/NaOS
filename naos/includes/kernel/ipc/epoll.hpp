#pragma once

#include "freelibcxx/vector.hpp"
#include "kernel/kobject.hpp"
#include "kernel/lock.hpp"
#include "kernel/mutex.hpp"
#include "kernel/time.hpp"
#include "kernel/types.hpp"
#include "kernel/wait.hpp"
#include "naos/abi.h"

namespace task
{
class resource_table_t;
}

namespace capability
{
struct entry;
}

namespace naos::ipc
{

class epoll final : public kobject, private readiness_sink
{
  public:
    static constexpr u64 max_registrations = 65'535;
    static constexpr u64 registration_chunk_size = 256;
    static constexpr u64 max_registration_chunks =
        (max_registrations + registration_chunk_size - 1) / registration_chunk_size;

    epoll();
    ~epoll() override;

    epoll(const epoll &) = delete;
    epoll &operator=(const epoll &) = delete;

    static type_e type_of() { return type_e::epoll; }

    na_status_t control(task::resource_table_t &resources, na_handle_t target, u32 operation,
                        const na_epoll_event_t *event);
    na_status_t wait(freelibcxx::vector<na_epoll_event_t> &events, timeclock::microsecond_t deadline,
                     freelibcxx::function_ref<void()> exit_cleanup = nullptr);
    bool initialized() const { return initialized_; }
    na_signal_t capability_signals() const override;
    void notify();

  private:
    struct registration
    {
        na_handle_t handle;
        u64 watcher_token;
        u64 generation;
        na_epoll_event_t event;
        u64 last_ready;
        u64 last_readiness_epoch;
        u64 ready_previous;
        u64 ready_next;
        khandle target;
        bool in_use;
        bool queued;
        // A deleted slot that must not be reused until its readiness watcher
        // removal has actually completed, so a reused token can never inherit
        // the previous registration's watcher entry.
        bool retiring;
    };

    using registration_chunk = freelibcxx::vector<registration>;

    static constexpr u64 invalid_slot = static_cast<u64>(-1);

    // Sink callback invoked by a watched target while it holds its readiness
    // lock.  It only marks the slot and wakes this epoll's wait queue.
    void on_readiness_change(u64 token) override;

    u64 find_registration_locked(na_handle_t handle) const;
    registration &registration_at_locked(u64 index);
    const registration &registration_at_locked(u64 index) const;
    bool has_free_registration_locked() const;
    bool ensure_registration_capacity(na_handle_t target);
    registration_chunk *allocate_registration_chunk();
    void queue_ready_locked(u64 token);
    void unqueue_ready_locked(u64 token);
    bool take_ready_locked(u64 &token, registration &snapshot);
    bool process_ready(freelibcxx::vector<na_epoll_event_t> *events);

    mutable lock::spinlock_t lock_;
    lock::mutex_t registration_growth_lock_;
    task::wait_queue_t wait_queue_;
    // Stable slots are allocated in chunks on demand. Each watched target
    // receives a never-reused watcher token. A freed slot is reused rather
    // than compacted, so an in-flight notification cannot be matched onto
    // another registration.
    freelibcxx::vector<registration_chunk *> registration_chunks_;
    u64 registration_chunk_count_ = 0;
    u64 registration_count_ = 0;
    // Intrusive ready queue: queue links live in stable registration slots, so
    // readiness notification needs no allocation or separately sized ring.
    u64 ready_head_ = invalid_slot;
    u64 ready_tail_ = invalid_slot;
    u64 ready_count_ = 0;
    u64 next_watcher_token_ = 1;
    bool initialized_ = false;

    static u64 observed_events(const khandle &target);
    static u64 visible_events(const registration &registration, u64 observed);
    static bool target_events(const registration &registration, u64 &events);
    bool has_ready();
    void collect_ready(freelibcxx::vector<na_epoll_event_t> &events);
};

} // namespace naos::ipc
