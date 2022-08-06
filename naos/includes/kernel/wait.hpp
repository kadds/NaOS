#pragma once
#include "freelibcxx/linked_list.hpp"
#include "kernel/lock.hpp"
#include "mm/vm.hpp"
namespace task
{
struct process_t;

typedef bool (*condition_func)(u64 user_data);
struct thread_t;
struct wait_context_t
{
    thread_t *thd;
    condition_func condition;
    u64 user_data;
    wait_context_t(thread_t *thd, condition_func condition, u64 user_data)
        : thd(thd)
        , condition(condition)
        , user_data(user_data)
    {
    }

    bool operator==(const wait_context_t &w) const
    {
        return thd == w.thd && user_data == w.user_data && condition == w.condition;
    }

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
    /// \param condition the condition
    /// \param user_data the condition user data pass to
    ///
    /// \return bool false: wait confition check failed, maybe interrupt by signal. true: ok
    bool do_wait(condition_func condition, u64 user_data);

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
