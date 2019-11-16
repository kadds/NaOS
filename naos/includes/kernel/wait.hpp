#pragma once
#include "kernel/mm/allocator.hpp"
#include "kernel/util/linked_list.hpp"
#include "lock.hpp"
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

struct wait_queue
{
    util::linked_list<wait_context_t> list;
    lock::spinlock_t lock;
    wait_queue(memory::IAllocator *a)
        : list(a)
    {
    }
};

void do_wait(wait_queue *queue, condition_func condition, u64 user_data, wait_context_type wct);
void do_wake_up(wait_queue *queue);

} // namespace task
