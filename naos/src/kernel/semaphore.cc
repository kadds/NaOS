#include "kernel/semaphore.hpp"

namespace lock
{
bool sp_condition(u64 data)
{
    semaphore_t *sep = (semaphore_t *)data;
    return sep->lock_res > 0;
}
} // namespace lock
