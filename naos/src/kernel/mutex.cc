#include "kernel/mutex.hpp"
namespace lock
{

bool mutex_func(u64 data)
{
    mutex_t *mtx = (mutex_t *)data;
    return !mtx->lock_m._M_i;
}
} // namespace lock
