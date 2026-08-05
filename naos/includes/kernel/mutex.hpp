#pragma once
#include "kernel/common.hpp"
#include "wait.hpp"
#include <atomic>
namespace lock
{
struct mutex_t
{
  private:
    std::atomic_flag lock_m = ATOMIC_FLAG_INIT;
    task::wait_queue_t wait_queue;

  public:
    mutex_t()
        : wait_queue() {};

    void lock()
    {
        while (!lock_m.test_and_set())
        {
            wait_queue.do_wait([this] { return !lock_m.test(std::memory_order_acquire); });
        };
    }

    void unlock()
    {
        lock_m.clear();
        wait_queue.do_wake_up(1);
    }
};

} // namespace lock
