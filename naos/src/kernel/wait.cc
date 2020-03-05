#include "kernel/wait.hpp"
#include "kernel/scheduler.hpp"
#include "kernel/task.hpp"
#include "kernel/ucontext.hpp"
namespace task
{

bool wait_queue_t::do_wait(condition_func condition, u64 user_data, wait_context_type wct)
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
        uctx::RawSpinLockContext ctx(lock);
        list.emplace_back(current(), condition, user_data);
    }

    for (;;)
    {
        auto thd = current();
        thd->attributes |= task::thread_attributes::need_schedule;
        scheduler::update_state(thd, state);
        scheduler::schedule();
        if (condition(user_data))
            break;
        // false wake up, try sleep.
    }
    {
        uctx::RawSpinLockContext ctx(lock);
        list.remove(list.find(wait_context_t(current(), condition, user_data)));
    }
    return condition(user_data);
}

u64 wait_queue_t::do_wake_up(u64 count)
{
    uctx::RawSpinLockUninterruptibleContext ctx(lock);
    u64 i = 0;
    for (auto it = list.begin(); it != list.end() && i < count;)
    {
        if (it->condition(it->user_data))
        {
            scheduler::update_state(it->thd, thread_state::ready);
            it = list.remove(it);
            i++;
        }
        else
            ++it;
    }
    return i;
}

} // namespace task
