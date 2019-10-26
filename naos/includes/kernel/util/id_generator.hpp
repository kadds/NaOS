#pragma once
#include "bit_set.hpp"
#include "common.hpp"
#include "kernel/lock.hpp"
namespace util
{
class id_generator
{
    std::atomic_uint64_t counter;
    lock::spinlock_t spinlock;

  public:
    u64 next(){};
    void save(u64 i){};
};
} // namespace util
