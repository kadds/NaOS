#pragma once

#include "kernel/task.hpp"

namespace task::scheduler::detail
{
// Run only on the owning CPU with interrupts disabled. Queue adapters keep
// policy-specific ordering in RR/CFS while these transitions own membership.
inline bool dispatchable(const thread_t *thread)
{
    if (thread == nullptr || thread->state != thread_state::ready || thread->schedule_data == nullptr ||
        thread->exit_lifecycle.started() ||
        (thread->attributes & (thread_attributes::job_control_stopped | thread_attributes::on_migrate)) ||
        thread->process == nullptr)
        return false;

    const auto flags = thread->process->attributes.load(std::memory_order_acquire);
    // An exiting process must still run its surviving threads to cancellation
    // points. Only completion/reaping makes its threads undispatchable.
    return (flags & (process_attributes::destroy | process_attributes::no_thread)) == 0 &&
           (!(flags & process_attributes::userspace) || thread->process->mm_info != nullptr);
}

template <typename Queues> bool update_state(thread_t *thread, thread_state requested, Queues &queues)
{
    if (requested != thread_state::ready && requested != thread_state::stop)
        return false;
    if (thread->state == thread_state::destroy)
        return true;

    const bool exiting = thread->exit_lifecycle.started();
    if (requested == thread_state::stop)
    {
        if (thread->state == thread_state::running)
            thread->attributes |= thread_attributes::block_to_stop | thread_attributes::need_schedule;
        else if (thread->state == thread_state::ready)
        {
            if (!queues.remove_ready(thread) && !exiting)
                return false;
            thread->state = thread_state::stop;
            if (!exiting)
                queues.add_blocked(thread);
        }
        return true;
    }

    // A late wake cannot revoke scheduler removal. The callback still owns
    // schedule_data until the victim has switched off its kernel stack.
    if (exiting)
    {
        if (thread->state == thread_state::running)
            thread->attributes |= thread_attributes::block_to_stop | thread_attributes::need_schedule;
        else if (thread->state == thread_state::ready)
        {
            queues.remove_ready(thread);
            thread->state = thread_state::stop;
        }
        return true;
    }
    if (thread->attributes & thread_attributes::job_control_stopped)
        return true;
    if (thread->schedule_data == nullptr || thread->process == nullptr)
        return true;
    const auto flags = thread->process->attributes.load(std::memory_order_acquire);
    if (flags & (process_attributes::destroy | process_attributes::no_thread))
        return true;
    if ((flags & process_attributes::userspace) && thread->process->mm_info == nullptr)
        return true;
    if (thread == cpu::current().get_idle_task())
    {
        thread->state = thread_state::ready;
        return true;
    }

    if (thread->state == thread_state::running)
        thread->attributes &= ~thread_attributes::block_to_stop;
    else if (thread->state == thread_state::stop)
    {
        queues.remove_blocked(thread);
        thread->state = thread_state::ready;
        thread->attributes &= ~thread_attributes::block_to_stop;
        queues.add_ready(thread);
    }
    return true;
}

template <typename Queues> void prepare_switch_out(thread_t *thread, Queues &queues)
{
    if (thread == cpu::current().get_idle_task())
    {
        thread->state = thread_state::ready;
        return;
    }
    if (thread->exit_lifecycle.started())
    {
        queues.remove_ready(thread);
        queues.remove_blocked(thread);
        thread->state = thread_state::stop;
        return;
    }
    if (thread->attributes & thread_attributes::block_to_stop)
    {
        queues.remove_ready(thread);
        thread->state = thread_state::stop;
        queues.add_blocked(thread);
    }
    else if (thread->state == thread_state::running)
    {
        thread->state = thread_state::ready;
        queues.add_ready(thread);
    }
}
} // namespace task::scheduler::detail
