#pragma once

#include "kernel/types.hpp"

namespace irq::detail
{

/// Drain one interrupt-context batch and report when the work budget was
/// consumed. A full batch may have emptied the queue exactly; scheduling one
/// empty continuation is harmless and keeps the producer contract simple.
template <typename Operation, typename TryDequeue, typename Execute>
bool drain_bounded_interrupt_work(u32 budget, Operation &operation, TryDequeue &&try_dequeue, Execute &&execute)
{
    for (u32 processed = 0; processed < budget; ++processed)
    {
        if (!try_dequeue(operation))
            return false;
        execute(operation);
    }
    return true;
}

} // namespace irq::detail
