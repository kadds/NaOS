#pragma once
#include "arch/klib.hpp"
#include "common.hpp"
#include <atomic>

/// locks aren't disable interrupt, use it by 'ucontext::guard'
namespace lock
{
///\brief Not nestable, unfair spinlock
struct spinlock_t
{
  private:
    std::atomic_bool lock_m;
#ifdef _DEBUG
    u64 last_stack_pointer;
    std::atomic_uint64_t wait_times;
#endif
  public:
    spinlock_t()
        : lock_m(false)
    {
    }
    spinlock_t(const spinlock_t &) = delete;
    spinlock_t &operator=(const spinlock_t &) = delete;
    void lock()
    {
        bool target = false;
        do
        {
            cpu_pause();
            target = false;
#ifdef _DEBUG
            wait_times++;
#endif
        } while (!lock_m.compare_exchange_strong(target, true, std::memory_order_acquire));
#ifdef _DEBUG
        last_stack_pointer = get_stack();
        wait_times = 0;
#endif
    }

    bool try_lock()
    {
        bool target = false;
        return lock_m.compare_exchange_strong(target, true, std::memory_order_acquire);
    }

    void unlock() { lock_m.store(false, std::memory_order_release); }
};

/// read write lock
struct rw_lock_t
{
  private:
    std::atomic_uint64_t lock_m = ATOMIC_FLAG_INIT;
    static_assert(sizeof(lock_m) == 8);

  public:
    rw_lock_t() = default;
    rw_lock_t(const rw_lock_t &) = delete;
    rw_lock_t &operator=(const rw_lock_t &) = delete;

    void lock_read()
    {
        u64 exp;
        do
        {
            // while (lock_m == 0x8000000000000000UL)
            cpu_pause();

            exp = lock_m & 0x7FFFFFFFFFFFFFFFUL;
        } while (!lock_m.compare_exchange_strong(exp, exp + 1, std::memory_order_acquire));
    }

    void lock_write()
    {
        u64 exp;
        do
        {
            // while (lock_m != 0)
            cpu_pause();
            exp = 0;
        } while (!lock_m.compare_exchange_strong(exp, 0x8000000000000000UL, std::memory_order_acquire));
    }

    bool try_lock_read()
    {
        u64 val = lock_m.load(std::memory_order_release);
        if (val == 0x8000000000000000UL)
            return false;
        u64 exp = val & 0x7FFFFFFFFFFFFFFFUL;
        return lock_m.compare_exchange_strong(exp, exp + 1, std::memory_order_acquire);
    }

    bool try_lock_write()
    {
        u64 exp;
        if (lock_m.load(std::memory_order_release) != 0)
            return false;
        exp = 0;
        return lock_m.compare_exchange_strong(exp, 0x8000000000000000UL, std::memory_order_acquire);
    }

    void unlock_read()
    {
        u64 exp;
        do
        {
#ifdef _DEBUG
            if (lock_m & 0x8000000000000000UL)
            {
                /// TODO: state is invalid.
            }
#endif
            exp = lock_m.load(std::memory_order_release) & 0x7FFFFFFFFFFFFFFFUL;
        } while (!lock_m.compare_exchange_strong(exp, exp - 1, std::memory_order_acquire));
    }

    void unlock_write()
    {
        u64 exp;
        do
        {
#ifdef _DEBUG
            if (lock_m == 0)
            {
                /// TODO: state is invalid.
            }
#endif
            exp = 0x8000000000000000UL;
        } while (!lock_m.compare_exchange_strong(exp, 0, std::memory_order_acquire));
    }
};

} // namespace lock
