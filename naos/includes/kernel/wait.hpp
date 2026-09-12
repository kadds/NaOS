#pragma once
#include "freelibcxx/function_ref.hpp"
#include "kernel/lock.hpp"
#include "mm/vm.hpp"
#include <atomic>
namespace task
{
struct process_t;

struct thread_t;
struct wait_context_t
{
    thread_t *thd;
    freelibcxx::function_ref<bool()> condition;
    // Optional identity used by multiplexed wait queues. A futex bucket is
    // shared by many user addresses, but FUTEX_WAKE must only make waiters
    // for the exact address in the exact address space runnable.
    const void *key_domain = nullptr;
    const void *key_address = nullptr;
    std::atomic_uint64_t *wake_sequence = nullptr;
    bool wake_requested = false;
    bool queued = false;
    bool pending_wake = false;
    wait_context_t *prev = nullptr;
    wait_context_t *next = nullptr;
    wait_context_t *pending_prev = nullptr;
    wait_context_t *pending_next = nullptr;
    wait_context_t(thread_t *thd, freelibcxx::function_ref<bool()> condition, const void *key_domain = nullptr,
                   const void *key_address = nullptr, std::atomic_uint64_t *wake_sequence = nullptr)
        : thd(thd)
        , condition(condition)
        , key_domain(key_domain)
        , key_address(key_address)
        , wake_sequence(wake_sequence)
    {
    }

    bool operator==(const wait_context_t &w) const { return thd == w.thd && condition == w.condition; }

    bool operator!=(const wait_context_t &w) const { return !operator==(w); }
};

struct wait_queue_t
{
    lock::spinlock_t lock;
    wait_context_t *head = nullptr;
    wait_context_t *tail = nullptr;
    wait_context_t *pending_head = nullptr;
    wait_context_t *pending_tail = nullptr;

    ///
    /// \brief wait current task for condition at the wait queue
    ///
    /// \param condition a borrowed predicate; it must remain alive until do_wait returns
    ///
    /// \return bool false: wait confition check failed, maybe interrupt by signal. true: ok
    bool do_wait(freelibcxx::function_ref<bool()> condition, const void *key_domain = nullptr,
                 const void *key_address = nullptr, std::atomic_uint64_t *wake_sequence = nullptr,
                 bool register_before_check = false);

    ///
    /// \brief try wake up task at queue
    ///
    /// \param queue the queue to wake up
    /// \param count maximum number of tasks to wake up
    u64 do_wake_up(u64 count = 0xFFFFFFFFFFFFFFFFUL);

    /// Wake at most `count` waiters registered for one exact key. This keeps
    /// a hashed futex bucket from letting an unrelated waiter consume a
    /// FUTEX_WAKE(1) notification.
    u64 do_wake_up_matching(const void *key_domain, const void *key_address, u64 count = 0xFFFFFFFFFFFFFFFFUL);

    void remove(thread_t *thread);

    void remove(process_t *process);

  private:
    void enqueue_locked(wait_context_t *context);
    void unlink_locked(wait_context_t *context);
    void queue_pending_locked(wait_context_t *context);
    thread_t *pop_pending_locked();
};

} // namespace task
