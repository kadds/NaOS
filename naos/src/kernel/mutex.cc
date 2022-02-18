#include "kernel/mutex.hpp"
namespace lock
{

bool mutex_func(u64 data)
{
    mutex_t *mtx = (mutex_t *)data;
    return !mtx->lock_m.test(std::memory_order_release);
}
} // namespace lock
