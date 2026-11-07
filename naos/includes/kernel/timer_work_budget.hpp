#pragma once

#include "kernel/types.hpp"

namespace timer::detail
{

enum class due_work_result : u8
{
    drained,
    continuation_required,
};

/// Process due timer work in bounded batches so a callback that continually
/// re-arms an already-due watcher cannot monopolize the interrupt path. If the
/// batch fills, schedule a soft-IRQ continuation instead of arming a hardware
/// timer for an already-expired deadline.
template <typename DrainOne, typename Continue>
due_work_result drain_bounded_due_work(u32 budget, DrainOne &&drain_one, Continue &&continue_work)
{
    for (u32 processed = 0; processed < budget; ++processed)
    {
        if (!drain_one())
            return due_work_result::drained;
    }
    continue_work();
    return due_work_result::continuation_required;
}

} // namespace timer::detail
