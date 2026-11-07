#include "kernel/schedulers/round_robin.hpp"
#include "kernel/cpu.hpp"
#include "kernel/mm/list_node_cache.hpp"
#include "kernel/schedulers/thread_transition.hpp"
#include "kernel/timer.hpp"
#include "kernel/ucontext.hpp"

KLOG_MODULE(sched);
namespace task::scheduler
{
struct thread_data_rr_t
{
    i64 rest_span;
};

struct cpu_task_rr_t
{
    using thread_list_t = freelibcxx::linked_list<task::thread_t *>;

    thread_list_t runable_list;
    thread_list_t block_threads;

    cpu_task_rr_t()
        : runable_list(memory::KernelCommonAllocatorV)
        , block_threads(memory::KernelCommonAllocatorV)
    {
    }
};

i64 calc_span(thread_t *);
void round_robin_scheduler::add(thread_t *thread)
{
    auto l = reinterpret_cast<cpu_task_rr_t *>(cpu::current().get_schedule_data(static_cast<int>(clazz)));

    auto rr = memory::New<thread_data_rr_t>(memory::KernelCommonAllocatorV);

    rr->rest_span = calc_span(thread);
    thread->cpuid = cpu::current().id();
    thread->schedule_data = rr;
    uctx::UninterruptibleContext icu;

    l->runable_list.push_back(thread);
}

namespace
{
struct rr_queues
{
    cpu_task_rr_t &cpu;
    bool remove_ready(thread_t *thread)
    {
        bool removed = false;
        for (auto it = cpu.runable_list.begin(); it != cpu.runable_list.end();)
        {
            if (*it == thread)
            {
                it = cpu.runable_list.remove(it);
                removed = true;
            }
            else
                ++it;
        }
        return removed;
    }
    void remove_blocked(thread_t *thread)
    {
        for (auto it = cpu.block_threads.begin(); it != cpu.block_threads.end();)
            if (*it == thread)
                it = cpu.block_threads.remove(it);
            else
                ++it;
    }
    void add_ready(thread_t *thread) { cpu.runable_list.push_back(thread); }
    void add_blocked(thread_t *thread)
    {
        if (cpu.block_threads.find(thread) == cpu.block_threads.end())
            cpu.block_threads.push_back(thread);
    }
};
rr_queues get_queues()
{
    return {*reinterpret_cast<cpu_task_rr_t *>(
        cpu::current().get_schedule_data(static_cast<int>(scheduler_class::round_robin)))};
}
} // namespace

void round_robin_scheduler::remove(thread_t *thread)
{
    uctx::UninterruptibleContext guard;
    auto queues = get_queues();
    queues.remove_ready(thread);
    queues.remove_blocked(thread);
    auto *data = static_cast<thread_data_rr_t *>(thread->schedule_data);
    thread->schedule_data = nullptr;
    if (data != nullptr)
        memory::Delete<>(memory::KernelCommonAllocatorV, data);
}

void round_robin_scheduler::update_state(thread_t *thread, thread_state state)
{
    uctx::UninterruptibleContext guard;
    auto queues = get_queues();
    if (!detail::update_state(thread, state, queues))
        KLOG_PANIC("Invalid RR transition: {} -> {}", static_cast<u64>(thread->state), static_cast<u64>(state));
}

void round_robin_scheduler::prepare_switch_out(thread_t *thread)
{
    uctx::UninterruptibleContext guard;
    auto queues = get_queues();
    detail::prepare_switch_out(thread, queues);
}

i64 calc_span(thread_t *thread) { return ((u64)thread->static_priority + 1000) / 100; }

void round_robin_scheduler::on_migrate(thread_t *thread)
{
    auto l = reinterpret_cast<cpu_task_rr_t *>(cpu::current().get_schedule_data(static_cast<int>(clazz)));

    auto rr = memory::New<thread_data_rr_t>(memory::KernelCommonAllocatorV);
    rr->rest_span = calc_span(thread);
    thread->cpuid = cpu::current().id();
    thread->schedule_data = rr;

    uctx::UninterruptibleContext icu;
    if (thread->state == thread_state::ready)
        l->runable_list.push_back(thread);
    else if (thread->state == thread_state::stop)
        l->block_threads.push_back(thread);
    else
        KLOG_PANIC("Unknown thread state when migrate(RR). state: {}", (u64)thread->state);
}

bool round_robin_scheduler::schedule()
{
    auto cur = current();
    bool has_task = false;
    auto &cpu = cpu::current();
    auto l = reinterpret_cast<cpu_task_rr_t *>(cpu::current().get_schedule_data(static_cast<int>(clazz)));
    while (cur->attributes & thread_attributes::need_schedule)
    {
        uctx::UninterruptibleContext guard;
        while (!l->runable_list.empty())
        {
            auto *candidate = l->runable_list.front();
            const bool dispatchable = detail::dispatchable(candidate);
            if (dispatchable)
                break;

            KLOG_WARN("discarding stale RR runnable entry thread={} process={} tid={} state={}",
                      reinterpret_cast<u64>(candidate),
                      candidate == nullptr ? 0 : reinterpret_cast<u64>(candidate->process),
                      candidate == nullptr ? 0 : static_cast<u64>(candidate->tid),
                      candidate == nullptr ? 0 : static_cast<u64>(candidate->state));
            l->runable_list.pop_front();
        }
        if (!l->runable_list.empty())
        {
            has_task = true;
            cur->scheduler->prepare_switch_out(cur);

            if (cur->scheduler == this && cur->state == thread_state::ready && !cur->exit_lifecycle.started())
            {
                auto *rr = static_cast<thread_data_rr_t *>(cur->schedule_data);
                if (rr->rest_span <= 0)
                    rr->rest_span = calc_span(cur);
            }
            auto *next = l->runable_list.pop_front();
            cur->attributes &= ~thread_attributes::need_schedule;
            next->state = thread_state::running;
            cpu.edit_load_data().last_sched_time = timer::get_high_resolution_time();
            cpu.edit_load_data().schedule_times++;
            task::switch_thread(cur, next);
            continue;
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
    auto l = reinterpret_cast<cpu_task_rr_t *>(cpu::current().get_schedule_data(static_cast<int>(clazz)));
    return l->runable_list.size();
}

thread_t *round_robin_scheduler::get_migratable_task(u32 cpuid)
{
    auto l = reinterpret_cast<cpu_task_rr_t *>(cpu::current().get_schedule_data(static_cast<int>(clazz)));
    if (l->runable_list.empty())
        return nullptr;
    uctx::UninterruptibleContext icu;
    for (auto thd : l->runable_list)
    {
        if (thd != current() && thd->state == thread_state::ready && (thd->cpumask.mask & (1ul << cpuid)))
            return thd;
    }
    return nullptr;
}

bool round_robin_scheduler::commit_migrate(thread_t *thd)
{
    auto l = reinterpret_cast<cpu_task_rr_t *>(cpu::current().get_schedule_data(static_cast<int>(clazz)));
    uctx::UninterruptibleContext icu;
    auto it = l->runable_list.find(thd);
    if (it == l->runable_list.end())
        return false;

    auto scher_data = (thread_data_rr_t *)thd->schedule_data;
    thd->schedule_data = nullptr;
    memory::Delete<>(memory::KernelCommonAllocatorV, scher_data);

    l->runable_list.remove(it);
    return true;
}

void round_robin_scheduler::init_cpu()
{
    cpu::current().set_schedule_data(static_cast<int>(clazz),
                                     memory::New<cpu_task_rr_t>(memory::KernelCommonAllocatorV));
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
