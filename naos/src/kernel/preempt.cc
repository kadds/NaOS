#include "kernel/preempt.hpp"
#include "kernel/scheduler.hpp"
#include "kernel/task.hpp"

namespace task
{

void yield_preempt()
{
    thread_t *thd = current();
    if (likely(thd != nullptr))
    {
        if (thd->preempt_data.preemptible() && (thd->attributes & thread_attributes::need_schedule))
        {
            scheduler::schedule();
        }
    }
}

ExportC void yield_preempt_schedule() { yield_preempt(); }

void disable_preempt()
{
    thread_t *thd = current();
    if (likely(thd != nullptr))
    {
        thd->preempt_data.disable_preempt();
    }
}
void enable_preempt()
{
    thread_t *thd = current();
    if (likely(thd != nullptr))
    {
        thd->preempt_data.enable_preempt();
    }
}
} // namespace task
