#pragma once
#include "common.hpp"
#include "wait.hpp"
#include <atomic>
namespace lock
{
bool mutex_func(u64 data);

struct mutex_t
{
  private:
    std::atomic_flag lock_m = ATOMIC_FLAG_INIT;
    task::wait_queue_t wait_queue;

  public:
    friend bool mutex_func(u64 data);
    mutex_t()
        : wait_queue(memory::KernelCommonAllocatorV){};

    void lock()
    {
        while (!lock_m.test_and_set())
        {
            wait_queue.do_wait(mutex_func, (u64)this);
        };
    }

    void unlock()
    {
        lock_m.clear();
        wait_queue.do_wake_up(1);
    }
};

} // namespace lock
