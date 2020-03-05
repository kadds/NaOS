#pragma once
#include "lock.hpp"
#include "mm/allocator.hpp"
#include "util/linked_list.hpp"
namespace task
{
enum class wait_context_type : u32
{
    interruptable,
    uninterruptible,
};

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
    util::linked_list<wait_context_t> list;
    lock::spinlock_t lock;
    wait_queue_t(memory::IAllocator *a)
        : list(a)
    {
    }

    ///
    /// \brief wait current task for condition at the wait queue
    ///
    /// \param queue the task save to
    /// \param condition the condition
    /// \param user_data the condition user data pass to
    /// \param wct wait context type
    ///
    /// \return bool false: wait confition check failed, maybe interrupt by signal. true: ok
    bool do_wait(condition_func condition, u64 user_data, wait_context_type wct);

    ///
    /// \brief try wake up task at queue
    ///
    /// \param queue the queue to wake up
    /// \param count maximum number of tasks to wake up
    u64 do_wake_up(u64 count = 0xFFFFFFFFFFFFFFFFUL);
};

} // namespace task
