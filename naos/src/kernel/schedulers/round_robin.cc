#include "kernel/schedulers/round_robin.hpp"
#include "kernel/cpu.hpp"
#include "kernel/mm/list_node_cache.hpp"
#include "kernel/timer.hpp"

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
    thread_list_t block_threads;

    cpu_task_rr_t()
        : runable_list(&list_node_allocator)
        , block_threads(&list_node_allocator)
    {
    }
};

i64 calc_span(thread_t *);
void round_robin_scheduler::add(thread_t *thread)
{
    auto l = (cpu_task_rr_t *)cpu::current().get_schedule_data((int)clazz);

    auto rr = memory::New<thread_data_rr_t>(memory::KernelCommonAllocatorV);

    rr->rest_span = calc_span(thread);
    thread->cpuid = cpu::current().id();
    thread->schedule_data = rr;
    uctx::UninterruptibleContext icu;

    l->runable_list.push_back(thread);
}

void round_robin_scheduler::remove(thread_t *thread)
{
    auto l = (cpu_task_rr_t *)cpu::current().get_schedule_data((int)clazz);
    uctx::UninterruptibleContext icu;

    auto it = l->runable_list.find(thread);
    if (it != l->runable_list.end())
    {
        l->runable_list.remove(it);
    }
    else
    {
        auto it2 = l->block_threads.find(thread);
        if (it2 != l->block_threads.end())
        {
            l->block_threads.remove(it2);
        }
        else
        {
            trace::panic("Can't find task ", thread->tid, " pid: ", thread->process->pid);
        }
    }
    memory::Delete<>(memory::KernelCommonAllocatorV, (thread_data_rr_t *)thread->schedule_data);
}

void round_robin_scheduler::update_state(thread_t *thread, thread_state state)
{
    auto l = (cpu_task_rr_t *)cpu::current().get_schedule_data((int)clazz);
    uctx::UninterruptibleContext icu;

    if (state == thread_state::stop)
    {
        if (thread->state == thread_state::ready)
        {
            auto it = l->runable_list.find(thread);
            if (it != l->runable_list.end())
            {
                thread->state = state;
                l->runable_list.remove(it);
                l->block_threads.push_back(thread);
                return;
            }
        }
        else if (thread->state == thread_state::running)
        {
            thread->attributes |= thread_attributes::block_to_stop | thread_attributes::need_schedule;
            return;
        }
    }
    else if (state == thread_state::ready)
    {
        if (thread->state == thread_state::stop)
        {
            auto it = l->block_threads.find(thread);
            if (it != l->block_threads.end())
            {
                thread->state = state;
                l->block_threads.remove(it);
                thread->attributes &= ~(thread_attributes::block_to_stop);
                l->runable_list.push_back(thread);
                return;
            }
        }
        else if (thread->state == thread_state::running)
        {
            return;
        }
    }
    else if (state == thread_state::sched_switch_to_ready)
    {
        if (thread->attributes & thread_attributes::block_to_stop)
        {
            thread->state = thread_state::stop;
            l->block_threads.push_back(thread);
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

void round_robin_scheduler::on_migrate(thread_t *thread)
{
    auto l = (cpu_task_rr_t *)cpu::current().get_schedule_data((int)clazz);

    auto rr = memory::New<thread_data_rr_t>(memory::KernelCommonAllocatorV);
    rr->rest_span = calc_span(thread);
    thread->cpuid = cpu::current().id();
    thread->schedule_data = rr;

    uctx::UninterruptibleContext icu;
    l->runable_list.push_back(thread);

    if (thread->state == thread_state::ready)
        l->runable_list.push_back(thread);
    else if (thread->state == thread_state::stop)
        l->block_threads.push_back(thread);
    else
        trace::panic("Unknown thread state when migrate(RR). state: ", (u64)thread->state);
}

bool round_robin_scheduler::schedule()
{
    auto cur = current();
    bool has_task = false;
    auto &cpu = cpu::current();
    auto l = (cpu_task_rr_t *)cpu.get_schedule_data((int)clazz);
    while (cur->attributes & thread_attributes::need_schedule)
    {
        if (!l->runable_list.empty())
        {
            uctx::UninterruptibleContext icu;
            if (!l->runable_list.empty())
            {
                has_task = true;

                cur->scheduler->update_state(cur, thread_state::sched_switch_to_ready);

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
                cpu.edit_load_data().last_sched_time = timer::get_high_resolution_time();
                cpu.edit_load_data().schedule_times++;
                task::switch_thread(cur, next);
                continue;
            }
        }
        if (cur->scheduler == this)
        {
            auto rr = (thread_data_rr_t *)cur->schedule_data;
            if (rr->rest_span <= 0)
            {
                rr->rest_span = calc_span(cur);
            }
            if (cur->attributes & thread_attributes::block_to_stop)
                return false;
            cur->attributes &= ~thread_attributes::need_schedule;
            return true;
        }
        else
            return false;
    }

    return has_task;
}

void round_robin_scheduler::schedule_tick()
{
    auto cur = current();
    auto rr = (thread_data_rr_t *)cur->schedule_data;
    auto &cpu = cpu::current();
    auto ctime = timer::get_high_resolution_time();
    cpu.edit_load_data().running_task_time += ctime - cpu.edit_load_data().last_tick_time;
    cpu.edit_load_data().last_tick_time = ctime;

    if (--rr->rest_span <= 0)
    {
        cur->attributes |= thread_attributes::need_schedule;
    }
}

u64 round_robin_scheduler::scheduleable_task_count()
{
    auto l = (cpu_task_rr_t *)cpu::current().get_schedule_data((int)clazz);
    return l->runable_list.size();
}

thread_t *round_robin_scheduler::get_migratable_task(u32 cpuid)
{
    auto l = (cpu_task_rr_t *)cpu::current().get_schedule_data((int)clazz);
    if (l->runable_list.empty())
        return nullptr;
    uctx::UninterruptibleContext icu;
    for (auto thd : l->runable_list)
    {
        if (thd->cpumask.mask & (1ul << cpuid))
            return thd;
    }
    return nullptr;
}

void round_robin_scheduler::commit_migrate(thread_t *thd)
{
    auto l = (cpu_task_rr_t *)cpu::current().get_schedule_data((int)clazz);
    uctx::UninterruptibleContext icu;
    auto it = l->runable_list.find(thd);
    kassert(it != l->runable_list.end(), "commit task failed!");
    l->runable_list.remove(it);
}

u64 round_robin_scheduler::sctl(int operator_type, thread_t *target, u64 attr, u64 *value, u64 size) { return 0; }

void round_robin_scheduler::init_cpu()
{
    cpu::current().set_schedule_data((int)clazz, memory::New<cpu_task_rr_t>(memory::KernelCommonAllocatorV));
    auto &data = cpu::current().edit_load_data();
    data.last_sched_time = timer::get_high_resolution_time();
    data.last_tick_time = timer::get_high_resolution_time();
}

void round_robin_scheduler::destroy_cpu()
{
    memory::Delete<>(memory::KernelCommonAllocatorV, (cpu_task_rr_t *)cpu::current().get_schedule_data((int)clazz));
}

round_robin_scheduler::round_robin_scheduler() {}

} // namespace task::scheduler
