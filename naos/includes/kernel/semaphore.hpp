#pragma once
#include "arch/klib.hpp"
#include "common.hpp"
#include "wait.hpp"
#include <atomic>

namespace lock
{
bool sp_condition(u64 data);

struct semaphore_t
{
    std::atomic_long lock_res = ATOMIC_FLAG_INIT;
    task::wait_queue wait_queue;
    semaphore_t(i64 count)
        : lock_res(count)
        , wait_queue(memory::KernelCommonAllocatorV)
    {
    }

    void down()
    {
        i64 exp;
        do
        {
            while (lock_res <= 0)
            {
                task::do_wait(&wait_queue, sp_condition, (u64)this, task::wait_context_type::uninterruptible);
            }
            exp = lock_res;
        } while (!lock_res.compare_exchange_strong(exp, exp - 1, std::memory_order_acquire));
    }

    void up()
    {
        lock_res++;
        if (lock_res > 0)
        {
            task::do_wake_up(&wait_queue, lock_res);
        }
    }
};

bool sp_condition(u64 data)
{
    semaphore_t *sep = (semaphore_t *)data;
    return sep->lock_res > 0;
}

} // namespace lock
