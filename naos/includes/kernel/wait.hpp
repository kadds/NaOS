#pragma once
#include "freelibcxx/function_ref.hpp"
#include "freelibcxx/linked_list.hpp"
#include "kernel/lock.hpp"
#include "mm/vm.hpp"
namespace task
{
struct process_t;

struct thread_t;
struct wait_context_t
{
    thread_t *thd;
    freelibcxx::function_ref<bool()> condition;
    wait_context_t(thread_t *thd, freelibcxx::function_ref<bool()> condition)
        : thd(thd)
        , condition(condition)
    {
    }

    bool operator==(const wait_context_t &w) const { return thd == w.thd && condition == w.condition; }

    bool operator!=(const wait_context_t &w) const { return !operator==(w); }
};

struct wait_queue_t
{
    freelibcxx::linked_list<wait_context_t> list;
    lock::spinlock_t lock;
    wait_queue_t()
        : list(memory::KernelCommonAllocatorV)
    {
    }

    ///
    /// \brief wait current task for condition at the wait queue
    ///
    /// \param condition a borrowed predicate; it must remain alive until do_wait returns
    ///
    /// \return bool false: wait confition check failed, maybe interrupt by signal. true: ok
    bool do_wait(freelibcxx::function_ref<bool()> condition);

    ///
    /// \brief try wake up task at queue
    ///
    /// \param queue the queue to wake up
    /// \param count maximum number of tasks to wake up
    u64 do_wake_up(u64 count = 0xFFFFFFFFFFFFFFFFUL);

    void remove(thread_t *thread);

    void remove(process_t *process);
};

} // namespace task
