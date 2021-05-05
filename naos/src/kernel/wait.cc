#include "kernel/wait.hpp"
#include "kernel/scheduler.hpp"
#include "kernel/task.hpp"
#include "kernel/ucontext.hpp"
namespace task
{

bool wait_queue_t::do_wait(condition_func condition, u64 user_data)
{
    if (condition(user_data))
        return true;
    thread_state state = thread_state::stop;

    uctx::UninterruptibleContext icu;
    {
        uctx::RawSpinLockContext ctx(lock);
        list.push_back(current(), condition, user_data);
    }

    for (;;)
    {
        auto thd = current();
        thd->attributes |= task::thread_attributes::need_schedule;
        thd->do_wait_queue_now = this;
        scheduler::update_state(thd, state);
        scheduler::schedule();
        if (condition(user_data))
            break;
        // false wake up, try sleep.
    }
    {
        uctx::RawSpinLockContext ctx(lock);
        auto it = list.find(wait_context_t(current(), condition, user_data));
        list.remove(it);
        current()->do_wait_queue_now = nullptr;
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

void wait_queue_t::remove(thread_t *thread)
{
    uctx::RawSpinLockUninterruptibleContext ctx(lock);
    for (auto it = list.begin(); it != list.end();)
    {
        if (thread == it->thd)
        {
            it = list.remove(it);
        }
        else
            ++it;
    }
}

void wait_queue_t::remove(process_t *process)
{
    uctx::RawSpinLockUninterruptibleContext ctx(lock);
    for (auto it = list.begin(); it != list.end();)
    {
        if (process == it->thd->process)
        {
            it = list.remove(it);
        }
        else
            ++it;
    }
}

} // namespace task
