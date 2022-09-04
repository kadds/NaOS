#pragma once
#include "arch/klib.hpp"
#include "common.hpp"
#include "kernel/kobject.hpp"
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

    void down(i64 n = 1)
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
        } while (!lock_res.compare_exchange_strong(exp, exp - n, std::memory_order_acquire));
    }

    void up(i64 n = 1)
    {
        lock_res += n;
        if (lock_res > 0)
        {
            wait_queue.do_wake_up(lock_res);
        }
    }

    i64 res() { return lock_res; }
};

struct semaphore_obj_t : public kobject, semaphore_t
{
  public:
    using semaphore_t::down;
    using semaphore_t::res;
    using semaphore_t::up;
    semaphore_obj_t(i64 count)
        : kobject(kobject::type_e::semaphore)
        , semaphore_t(count)
    {
    }
    static type_e type_of() { return type_e::semaphore; }
};

} // namespace lock
