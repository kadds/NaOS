#pragma once
#include "arch/klib.hpp"
#include "common.hpp"
#include <atomic>

namespace lock
{
struct semaphore_t
{
    std::atomic_long lock_res;
    semaphore_t(u64 count)
        : lock_res(count)
    {
    }

    void down()
    {
        lock_res--;
        while (lock_res < 0)
        {
            cpu_pause();
        }
    }

    void up() { lock_res++; }
};
struct spinlock_t
{
  private:
    std::atomic_flag lock_m = ATOMIC_FLAG_INIT;
#ifdef _DEBUG
    u64 last_stack_pointer;
    std::atomic_uint64_t wait_times;
#endif
  public:
    spinlock_t() = default;
    spinlock_t(const spinlock_t &) = delete;
    spinlock_t &operator=(const spinlock_t &) = delete;
    void lock()
    {
        do
        {
            while (lock_m._M_i)
                cpu_pause();
#ifdef _DEBUG
            wait_times++;
#endif
        } while (lock_m.test_and_set(std::memory_order_acquire));
#ifdef _DEBUG
        last_stack_pointer = get_stack();
        wait_times = 0;
#endif
    }

    bool try_lock() { return !lock_m.test_and_set(std::memory_order_acquire); }

    void unlock() { lock_m.clear(std::memory_order_release); }
};
/// TODO: read write lock
struct rw_lock_t
{
  private:
    spinlock_t write;

  public:
    rw_lock_t() = default;
    rw_lock_t(const rw_lock_t &) = delete;
    rw_lock_t &operator=(const rw_lock_t &) = delete;

    void lock_read() {}
    void lock_write() {}

    void unlock_read() {}
    void unlock_write() {}
};

}; // namespace lock