#pragma once
#include "common.hpp"
namespace arch::lock
{
struct spinlock_t
{
    volatile u64 _i;

    void lock()
    {
        while (!__sync_bool_compare_and_swap(&_i, 0, 1))
            while (_i)
                ;
    }

    bool try_lock() { return __sync_bool_compare_and_swap(&_i, 0, 1); }

    void unlock() { _i = 0; }
};
} // namespace arch::lock
