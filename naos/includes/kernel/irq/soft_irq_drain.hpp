#pragma once

#include "kernel/types.hpp"

namespace irq::detail
{

template <typename DisableInterrupts, typename HasPending, typename DrainPending>
void drain_soft_interrupts_at_exit_barrier(DisableInterrupts &&disable_interrupts, HasPending &&has_pending,
                                           DrainPending &&drain_pending)
{
    disable_interrupts();
    if (has_pending())
        drain_pending();
}

template <typename DrainOne, typename HasPending>
void drain_requeued_soft_interrupts(u32 vector_count, u32 pass_limit, DrainOne &&drain_one, HasPending &&has_pending)
{
    for (u32 pass = 0; pass < pass_limit; ++pass)
    {
        bool processed = false;
        for (u32 vector = 0; vector < vector_count; ++vector)
            processed = drain_one(vector) || processed;
        if (!processed || !has_pending())
            return;
    }
}

} // namespace irq::detail
