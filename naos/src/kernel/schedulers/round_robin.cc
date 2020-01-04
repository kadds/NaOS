#include "kernel/schedulers/round_robin.hpp"
#include "kernel/cpu.hpp"
#include "kernel/mm/list_node_cache.hpp"

namespace task::scheduler
{
struct thread_data_rr_t
{
    i64 rest_span;
};

struct cpu_task_rr_t
{
    using thread_list_t = util::linked_list<task::thread_t *>;
    using thread_list_cache_allocator_t = memory::list_node_cache_allocator<thread_list_t>;

    thread_list_cache_allocator_t list_node_allocator;
    thread_list_t runable_list;
    lock::spinlock_t list_spinlock;

    cpu_task_rr_t()
        : runable_list(&list_node_allocator)
    {
    }
};

i64 calc_span(thread_t *);
void round_robin_scheduler::add(thread_t *thread)
{
    auto l = (cpu_task_rr_t *)cpu::current().get_schedule_data((int)clazz);

    auto rr = memory::New<thread_data_rr_t>(memory::KernelCommonAllocatorV);
    rr->rest_span = calc_span(thread);
    thread->schedule_data = rr;
    l->runable_list.push_back(thread);
}

void round_robin_scheduler::remove(thread_t *thread)
{
    auto l = (cpu_task_rr_t *)cpu::current().get_schedule_data((int)clazz);
    auto it = l->runable_list.find(thread);
    if (it != l->runable_list.end())
    {
        l->runable_list.remove(it);
    }
    memory::Delete<>(memory::KernelCommonAllocatorV, (thread_data_rr_t *)thread->schedule_data);
}

void round_robin_scheduler::update_state(thread_t *thread, thread_state state)
{
    auto l = (cpu_task_rr_t *)cpu::current().get_schedule_data((int)clazz);

    if (state == thread_state::interruptable || state == thread_state::uninterruptible)
    {
        if (thread->state == thread_state::ready)
        {
            thread->state = state;
            auto it = l->runable_list.find(thread);
            if (it != l->runable_list.end())
            {
                l->runable_list.remove(it);
                block_threads.push_back(thread);
                return;
            }
        }
        else if (thread->state == thread_state::running)
        {
            if (state == thread_state::interruptable)
                thread->attributes |= thread_attributes::block_intr | thread_attributes::need_schedule;
            else
                thread->attributes |= thread_attributes::block_unintr | thread_attributes::need_schedule;

            return;
        }
    }
    else if (state == thread_state::ready)
    {
        if (thread->state == thread_state::uninterruptible || thread->state == thread_state::interruptable)
        {
            thread->state = state;
            block_threads.remove(block_threads.find(thread));
            thread->attributes &= ~(thread_attributes::block_unintr | thread_attributes::block_intr);
            l->runable_list.push_back(thread);
            return;
        }
        else if (thread->state == thread_state::running)
        {
            return;
        }
    }
    else if (state == thread_state::sched_switch_to_ready)
    {
        if (thread->attributes & thread_attributes::block_intr)
        {
            thread->state = thread_state::interruptable;
            block_threads.push_back(thread);
        }
        else if (thread->attributes & thread_attributes::block_unintr)
        {
            thread->state = thread_state::uninterruptible;
            block_threads.push_back(thread);
        }
        else
        {
            if (thread->state == thread_state::running)
            {
                thread->state = thread_state::ready;
                l->runable_list.push_back(thread);
            }
        }
        return;
    }
    trace::panic("Unreachable control flow.", " RR thread state:", (int)thread->state, ", to state: ", (int)state);
}

i64 calc_span(thread_t *thread) { return ((u64)thread->static_priority + 1000) / 100; }

void round_robin_scheduler::update_prop(thread_t *thread, u8 static_priority, u8 dyn_priority) {}

bool round_robin_scheduler::schedule()
{
    auto cur = current();
    bool has_task = false;
    auto l = (cpu_task_rr_t *)cpu::current().get_schedule_data((int)clazz);
    while (cur->attributes & thread_attributes::need_schedule)
    {

        if (!l->runable_list.empty())
        {
            uctx::UnInterruptableContext icu;

            has_task = true;

            cur->scheduler->update_state(cur, thread_state::sched_switch_to_ready);
            if (cur->attributes & thread_attributes::remove)
            {
                cur->scheduler->remove(cur);
            }

            if (cur->scheduler == this)
            {
                auto rr = (thread_data_rr_t *)cur->schedule_data;
                if (rr->rest_span <= 0)
                {
                    rr->rest_span = calc_span(cur);
                    l->runable_list.push_back(cur);
                }
            }
            auto next = l->runable_list.pop_front();
            cur->attributes &= ~thread_attributes::need_schedule;
            next->state = thread_state::running;
            task::switch_thread(cur, next);
        }
        else
        {
            if (cur->scheduler == this)
            {
                auto rr = (thread_data_rr_t *)cur->schedule_data;
                if (rr->rest_span <= 0)
                {
                    rr->rest_span = calc_span(cur);
                }
                if (cur->attributes & thread_attributes::block_unintr ||
                    cur->attributes & thread_attributes::block_intr)
                    return false;
                cur->attributes &= ~thread_attributes::need_schedule;
                return true;
            }
            else
                return false;
        }
    }
    return has_task;
}

void round_robin_scheduler::schedule_tick()
{
    auto cur = current();
    auto rr = (thread_data_rr_t *)cur->schedule_data;
    if (--rr->rest_span <= 0)
    {
        cur->attributes |= thread_attributes::need_schedule;
    }
}

bool round_robin_scheduler::has_task_to_schedule()
{
    auto l = (cpu_task_rr_t *)cpu::current().get_schedule_data((int)clazz);
    return !l->runable_list.empty();
}

u64 round_robin_scheduler::sctl(int operator_type, thread_t *target, u64 attr, u64 *value, u64 size) { return 0; }

void round_robin_scheduler::init_cpu()
{
    cpu::current().set_schedule_data((int)clazz, memory::New<cpu_task_rr_t>(memory::KernelCommonAllocatorV));
}

void round_robin_scheduler::destroy_cpu()
{
    memory::Delete<>(memory::KernelCommonAllocatorV, (cpu_task_rr_t *)cpu::current().get_schedule_data((int)clazz));
}

round_robin_scheduler::round_robin_scheduler()
    : block_threads(memory::KernelCommonAllocatorV)
{
}

} // namespace task::scheduler
