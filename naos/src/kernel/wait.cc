#include "kernel/wait.hpp"
#include "kernel/scheduler.hpp"
#include "kernel/task.hpp"
#include "kernel/ucontext.hpp"
namespace task
{

bool do_wait(wait_queue *queue, condition_func condition, u64 user_data, wait_context_type wct)
{
    if (condition(user_data))
        return true;
    thread_state state;
    if (wct == wait_context_type::interruptable)
    {
        state = thread_state::interruptable;
    }
    else if (wct == wait_context_type::uninterruptible)
    {
        state = thread_state::uninterruptible;
    }
    else
        trace::panic("wait context type is unknown.");

    uctx::UninterruptibleContext icu;
    {
        uctx::RawSpinLockContext ctx(queue->lock);
        queue->list.emplace_back(current(), condition, user_data);
    }

    for (;;)
    {
        auto thd = current();
        thd->attributes |= task::thread_attributes::need_schedule;
        scheduler::update_state(thd, state);
        scheduler::schedule();
        if (thd->signal_pack.is_set() || condition(user_data))
            break;
        // false wake up, try sleep.
    }
    {
        uctx::RawSpinLockContext ctx(queue->lock);
        queue->list.remove(queue->list.find(wait_context_t(current(), condition, user_data)));
    }
    return condition(user_data);
}

u64 do_wake_up(wait_queue *queue, u64 count)
{
    uctx::RawSpinLockUninterruptibleContext ctx(queue->lock);
    u64 i = 0;
    for (auto it = queue->list.begin(); it != queue->list.end() && i < count;)
    {
        if (it->condition(it->user_data))
        {
            scheduler::update_state(it->thd, thread_state::ready);
            it = queue->list.remove(it);
            i++;
        }
        else
            ++it;
    }
    return i;
}

u64 do_wake_up_signal(wait_queue *queue, thread_t *thread)
{
    uctx::RawSpinLockUninterruptibleContext ctx(queue->lock);

    for (auto it = queue->list.begin(); it != queue->list.end();)
    {
        if (it->thd == thread)
        {
            scheduler::update_state(it->thd, thread_state::ready);
            it = queue->list.remove(it);
            return 1;
        }
        else
            ++it;
    }
    return 0;
}
} // namespace task
