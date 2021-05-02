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
  private:
    std::atomic_long lock_res = ATOMIC_FLAG_INIT;
    task::wait_queue_t wait_queue;
    friend bool sp_condition(u64 data);

  public:
    semaphore_t(i64 count)
        : lock_res(count)
    {
    }

    void down()
    {
        i64 exp;
        do
        {
            while (lock_res <= 0)
            {
                wait_queue.do_wait(sp_condition, (u64)this);
            }
            exp = lock_res;
            if (exp <= 0)
                continue;
        } while (!lock_res.compare_exchange_strong(exp, exp - 1, std::memory_order_acquire));
    }

    void up()
    {
        lock_res++;
        if (lock_res > 0)
        {
            wait_queue.do_wake_up(lock_res);
        }
    }

    i64 res() { return lock_res; }
};

} // namespace lock
