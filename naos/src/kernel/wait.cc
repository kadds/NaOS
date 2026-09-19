#include "kernel/wait.hpp"
#include "kernel/scheduler.hpp"
#include "kernel/task.hpp"
#include "kernel/ucontext.hpp"
namespace task
{

bool wait_queue_t::do_wait(freelibcxx::function_ref<bool()> condition, const void *key_domain, const void *key_address,
                           std::atomic_uint64_t *wake_sequence, bool register_before_check)
{
    if (!register_before_check && condition())
        return true;
    auto *thd = current();
    wait_context_t context(thd, condition, key_domain, key_address, wake_sequence);

    {
        uctx::RawSpinLockUninterruptibleContext ctx(lock);
        enqueue_locked(&context);
        if (register_before_check)
        {
            // The futex predicate is a bounded usercopy and does not sleep.
            // Registering it before this first check closes the check-to-stop
            // lost-wakeup window without evaluating arbitrary wait predicates
            // while holding a queue lock (epoll predicates may take another
            // object lock).
            thd->do_wait_queue_now = this;
            if (condition())
            {
                unlink_locked(&context);
                thd->do_wait_queue_now = nullptr;
                return true;
            }
        }

        // Publish the blocked state while the waiter is visible to notifiers.
        // A notifier that runs immediately afterwards can clear block_to_stop
        // before schedule() switches away; a notifier that runs later finds
        // the thread in the scheduler's blocked set and makes it runnable.
        // Both cases preserve the wake without a second, competing state
        // transition for the same wait context.
        thd->attributes |= task::thread_attributes::voluntary_context_switch;
        thd->attributes |= task::thread_attributes::need_schedule;
        thd->do_wait_queue_now = this;
        scheduler::update_state(thd, thread_state::stop);
    }

    if (condition())
    {
        scheduler::update_state(thd, thread_state::ready);
        uctx::RawSpinLockUninterruptibleContext ctx(lock);
        unlink_locked(&context);
        thd->do_wait_queue_now = nullptr;
        return true;
    }

    for (;;)
    {
        scheduler::schedule();
        if (condition())
            break;

        bool registered = false;
        {
            uctx::RawSpinLockUninterruptibleContext ctx(lock);
            if (!context.queued)
            {
                thd->do_wait_queue_now = nullptr;
            }
            else
            {
                // The wake request only records that the scheduler made this
                // waiter runnable.  The predicate is the authority; consume
                // the request before arming the next sleep.
                context.wake_requested = false;
                thd->attributes |= task::thread_attributes::voluntary_context_switch;
                thd->attributes |= task::thread_attributes::need_schedule;
                scheduler::update_state(thd, thread_state::stop);
                registered = true;
            }
        }

        if (!registered)
            return condition();
    }

    {
        uctx::RawSpinLockUninterruptibleContext ctx(lock);
        unlink_locked(&context);
        thd->do_wait_queue_now = nullptr;
    }

    return condition();
}

bool wait_queue_t::block_current(freelibcxx::function_ref<bool()> condition)
{
    auto *thd = current();
    if (thd == nullptr || thd->do_wait_queue_now != nullptr)
        return false;

    auto &context = thd->async_wait_context;
    context.thd = thd;
    context.condition = nullptr;
    context.key_domain = nullptr;
    context.key_address = nullptr;
    context.wake_sequence = nullptr;
    context.wake_requested = false;
    context.queued = false;
    context.pending_wake = false;
    context.prev = nullptr;
    context.next = nullptr;
    context.pending_prev = nullptr;
    context.pending_next = nullptr;
    {
        uctx::RawSpinLockUninterruptibleContext ctx(lock);
        enqueue_locked(&context);
        thd->do_wait_queue_now = this;
        if (condition != nullptr && condition())
        {
            unlink_locked(&context);
            thd->do_wait_queue_now = nullptr;
            return false;
        }
        thd->attributes |= thread_attributes::voluntary_context_switch;
        thd->attributes |= thread_attributes::need_schedule;
        scheduler::update_state(thd, thread_state::stop);
    }
    return true;
}

u64 wait_queue_t::do_wake_up(u64 count)
{
    const u64 requested = count;
    {
        uctx::RawSpinLockUninterruptibleContext ctx(lock);
        for (auto *context = head; context != nullptr && count != 0; context = context->next)
        {
            // One deferred scheduler transition is enough: after it runs the
            // waiter rechecks its predicate before parking again. Repeating
            // ready transitions while it remains in the blocked set adds
            // competing run-queue operations without carrying new state.
            if (!context->wake_requested)
            {
                context->wake_requested = true;
                context->thd->wait_queue_wake_refs.fetch_add(1, std::memory_order_relaxed);
                queue_pending_locked(context);
                count--;
            }
        }
    }

    u64 woken = 0;
    while (woken < requested)
    {
        thread_t *thread = nullptr;
        {
            uctx::RawSpinLockUninterruptibleContext ctx(lock);
            thread = pop_pending_locked();
        }
        if (thread == nullptr)
            break;
        scheduler::update_state_async(thread, thread_state::ready, &thread->wait_queue_wake_refs);
        woken++;
    }
    return woken;
}

u64 wait_queue_t::do_wake_up_matching(const void *key_domain, const void *key_address, u64 count)
{
    const u64 requested = count;
    {
        uctx::RawSpinLockUninterruptibleContext ctx(lock);
        for (auto *context = head; context != nullptr && count != 0; context = context->next)
        {
            if (context->key_domain != key_domain || context->key_address != key_address || context->wake_requested)
                continue;
            context->wake_requested = true;
            if (context->wake_sequence != nullptr)
                context->wake_sequence->fetch_add(1, std::memory_order_release);
            context->thd->wait_queue_wake_refs.fetch_add(1, std::memory_order_relaxed);
            queue_pending_locked(context);
            count--;
        }
    }

    u64 woken = 0;
    while (woken < requested)
    {
        thread_t *thread = nullptr;
        {
            uctx::RawSpinLockUninterruptibleContext ctx(lock);
            thread = pop_pending_locked();
        }
        if (thread == nullptr)
            break;
        scheduler::update_state_async(thread, thread_state::ready, &thread->wait_queue_wake_refs);
        woken++;
    }
    return woken;
}

void wait_queue_t::remove(thread_t *thread)
{
    uctx::RawSpinLockUninterruptibleContext ctx(lock);
    for (auto *context = head; context != nullptr;)
    {
        auto *next = context->next;
        if (thread == context->thd)
            unlink_locked(context);
        context = next;
    }
    if (thread != nullptr && thread->do_wait_queue_now == this)
        thread->do_wait_queue_now = nullptr;
}

void wait_queue_t::remove(process_t *process)
{
    uctx::RawSpinLockUninterruptibleContext ctx(lock);
    for (auto *context = head; context != nullptr;)
    {
        auto *next = context->next;
        if (process == context->thd->process)
            unlink_locked(context);
        context = next;
    }
}

void wait_queue_t::enqueue_locked(wait_context_t *context)
{
    context->queued = true;
    context->prev = tail;
    context->next = nullptr;
    if (tail != nullptr)
        tail->next = context;
    else
        head = context;
    tail = context;
}

void wait_queue_t::unlink_locked(wait_context_t *context)
{
    if (!context->queued)
        return;
    if (context->prev != nullptr)
        context->prev->next = context->next;
    else
        head = context->next;
    if (context->next != nullptr)
        context->next->prev = context->prev;
    else
        tail = context->prev;
    context->queued = false;
    context->prev = nullptr;
    context->next = nullptr;
    if (context->pending_wake)
    {
        context->thd->wait_queue_wake_refs.fetch_sub(1, std::memory_order_relaxed);
        if (context->pending_prev != nullptr)
            context->pending_prev->pending_next = context->pending_next;
        else
            pending_head = context->pending_next;
        if (context->pending_next != nullptr)
            context->pending_next->pending_prev = context->pending_prev;
        else
            pending_tail = context->pending_prev;
        context->pending_wake = false;
        context->pending_prev = nullptr;
        context->pending_next = nullptr;
    }
}

void wait_queue_t::queue_pending_locked(wait_context_t *context)
{
    if (context->pending_wake)
        return;
    context->pending_wake = true;
    context->pending_prev = pending_tail;
    context->pending_next = nullptr;
    if (pending_tail != nullptr)
        pending_tail->pending_next = context;
    else
        pending_head = context;
    pending_tail = context;
}

thread_t *wait_queue_t::pop_pending_locked()
{
    auto *context = pending_head;
    if (context == nullptr)
        return nullptr;
    pending_head = context->pending_next;
    if (pending_head != nullptr)
        pending_head->pending_prev = nullptr;
    else
        pending_tail = nullptr;
    context->pending_wake = false;
    context->pending_prev = nullptr;
    context->pending_next = nullptr;
    return context->thd;
}

} // namespace task
