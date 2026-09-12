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
    epoll();
    ~epoll() override;

    epoll(const epoll &) = delete;
    epoll &operator=(const epoll &) = delete;

    static type_e type_of() { return type_e::epoll; }

    na_status_t control(task::resource_table_t &resources, na_handle_t target, u32 operation,
                        const na_epoll_event_t *event);
    na_status_t wait(freelibcxx::vector<na_epoll_event_t> &events, timeclock::microsecond_t deadline);
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
        khandle target;
        bool in_use;
        bool queued;
        // A deleted slot that must not be reused until its readiness watcher
        // removal has actually completed, so a reused token can never inherit
        // the previous registration's watcher entry.
        bool retiring;
    };

    static constexpr u64 invalid_slot = static_cast<u64>(-1);

    // Sink callback invoked by a watched target while it holds its readiness
    // lock.  It only marks the slot and wakes this epoll's wait queue.
    void on_readiness_change(u64 token) override;

    u64 find_registration_locked(na_handle_t handle) const;
    void queue_ready_locked(u64 token);
    void unqueue_ready_locked(u64 token);
    bool take_ready_locked(u64 &token, registration &snapshot);
    bool process_ready(freelibcxx::vector<na_epoll_event_t> *events);

    mutable lock::spinlock_t lock_;
    // Registration and ready-ring growth is prepared under this sleepable
    // mutex, then committed under lock_.  The latter is never held across an
    // allocator call.
    mutable lock::mutex_t allocation_lock_;
    task::wait_queue_t wait_queue_;
    // Stable slots: the ready ring stores slot indices, while each watched
    // target receives a never-reused watcher token. A freed slot is reused
    // rather than compacted, so an in-flight notification cannot be matched
    // onto another registration.
    freelibcxx::vector<registration> registrations_;
    // Ring of slot indices a notifier marked as possibly ready.  Its physical
    // size tracks the slot count and is grown only from the control path, so
    // on_readiness_change performs no dynamic allocation.
    freelibcxx::vector<u64> ready_ring_;
    u64 ready_head_ = 0;
    u64 ready_count_ = 0;
    u64 next_watcher_token_ = 1;

    static u64 observed_events(const khandle &target);
    static u64 visible_events(const registration &registration, u64 observed);
    static bool target_events(const registration &registration, u64 &events);
    bool has_ready();
    void collect_ready(freelibcxx::vector<na_epoll_event_t> &events);
};

} // namespace naos::ipc
