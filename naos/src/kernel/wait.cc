#include "kernel/wait.hpp"
#include "kernel/scheduler.hpp"
#include "kernel/task.hpp"
#include "kernel/ucontext.hpp"
namespace task
{

bool wait_queue_t::do_wait(freelibcxx::function_ref<bool()> condition)
{
    if (condition())
        return true;
    auto *thd = current();

    {
        uctx::RawSpinLockUninterruptibleContext ctx(lock);
        list.push_back(thd, condition);
        thd->attributes |= task::thread_attributes::need_schedule;
        thd->do_wait_queue_now = this;
        scheduler::update_state(thd, thread_state::stop);
    }

    if (condition())
    {
        scheduler::update_state(thd, thread_state::ready);
        uctx::RawSpinLockUninterruptibleContext ctx(lock);
        auto it = list.find(wait_context_t(thd, condition));
        list.remove(it);
        thd->do_wait_queue_now = nullptr;
        return true;
    }

    for (;;)
    {
        scheduler::schedule();
        if (condition())
            break;

        {
            bool registered = false;
            {
                uctx::RawSpinLockUninterruptibleContext ctx(lock);
                auto it = list.find(wait_context_t(thd, condition));
                if (it == list.end())
                    thd->do_wait_queue_now = nullptr;
                else
                {
                    it->wake_requested = false;
                    scheduler::update_state(thd, thread_state::stop);
                    registered = true;
                }
            }
            if (!registered)
                return condition();
        }
    }

    {
        uctx::RawSpinLockUninterruptibleContext ctx(lock);
        auto it = list.find(wait_context_t(thd, condition));
        list.remove(it);
        thd->do_wait_queue_now = nullptr;
    }

    return condition();
}

u64 wait_queue_t::do_wake_up(u64 count)
{
    freelibcxx::vector<thread_t *> wake_targets(memory::KernelCommonAllocatorV);
    {
        uctx::RawSpinLockUninterruptibleContext ctx(lock);
        for (auto it = list.begin(); it != list.end() && wake_targets.size() < count; ++it)
        {
            if (!it->wake_requested)
            {
                it->wake_requested = true;
                it->thd->wait_queue_wake_refs.fetch_add(1, std::memory_order_relaxed);
                wake_targets.push_back(it->thd);
            }
        }
    }

    for (auto *thread : wake_targets)
    {
        scheduler::update_state_sync(thread, thread_state::ready);
        thread->wait_queue_wake_refs.fetch_sub(1, std::memory_order_release);
    }
    return wake_targets.size();
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
