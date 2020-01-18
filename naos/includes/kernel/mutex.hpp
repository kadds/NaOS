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
    task::wait_queue wait_queue;

  public:
    friend bool mutex_func(u64 data);
    mutex_t()
        : wait_queue(memory::KernelCommonAllocatorV){};

    void lock()
    {
        while (!lock_m.test_and_set())
        {
            task::do_wait(&wait_queue, mutex_func, (u64)this, task::wait_context_type::uninterruptible);
        };
    }

    void unlock()
    {
        lock_m.clear();
        task::do_wake_up(&wait_queue, 1);
    }
};

} // namespace lock
