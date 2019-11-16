#include "kernel/wait.hpp"
#include "kernel/scheduler.hpp"
#include "kernel/task.hpp"
#include "kernel/ucontext.hpp"
namespace task
{

void do_wait(wait_queue *queue, condition_func condition, u64 user_data, wait_context_type wct)
{
    if (condition(user_data))
        return;
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
        return;

    uctx::UnInterruptableContext icu;
    {
        uctx::SpinLockContext ctx(queue->lock);
        queue->list.emplace_back(current(), condition, user_data);
    }

    for (;;)
    {
        current()->attributes |= task::thread_attributes::need_schedule;
        current()->state = state;
        scheduler::update(current());
        scheduler::schedule();
        if (condition(user_data))
            break;
    }
    {
        uctx::SpinLockContext ctx(queue->lock);
        queue->list.remove(queue->list.find(wait_context_t(current(), condition, user_data)));
    }
}

void do_wake_up(wait_queue *queue)
{
    uctx::SpinLockUnInterruptableContext ctx(queue->lock);
    for (auto it = queue->list.begin(); it != queue->list.end();)
    {
        if (it->condition(it->user_data))
        {
            it->thd->state = thread_state::ready;
            scheduler::update(it->thd);
            it = queue->list.remove(it);
        }
        else
            ++it;
    }
}
} // namespace task
