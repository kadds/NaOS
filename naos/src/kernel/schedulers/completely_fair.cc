#include "kernel/schedulers/completely_fair.hpp"
#include "freelibcxx/skip_list.hpp"
#include "kernel/clock.hpp"
#include "kernel/cpu.hpp"
#include "kernel/mm/list_node_cache.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/task.hpp"
#include "kernel/timer.hpp"
#include "kernel/ucontext.hpp"

namespace task::scheduler
{
struct thread_time_cf_t
{
    i64 vtime;
    u64 vtime_delta;
};

struct less_cmp
{
};

struct cfs_thread_t
{
    thread_t *thread;
    bool operator<(const cfs_thread_t &thd)
    {
        return ((thread_time_cf_t *)(thread->schedule_data))->vtime <
               ((thread_time_cf_t *)(thd.thread->schedule_data))->vtime;
    }
    bool operator==(const cfs_thread_t &thd) { return thd.thread == thread; }
    explicit cfs_thread_t(thread_t *thread)
        : thread(thread)
    {
    }
};

using thread_skip_list_t = freelibcxx::skip_list<cfs_thread_t>;
struct cpu_task_list_cf_t
{
    thread_skip_list_t runable_list;
    thread_list_t block_list;
    u64 min_vruntime;

    cpu_task_list_cf_t()
        : runable_list(memory::KernelCommonAllocatorV)
        , block_list(memory::KernelCommonAllocatorV)
    {
    }
};

cpu_task_list_cf_t *get_cpu_task_list()
{
    return (cpu_task_list_cf_t *)cpu::current().get_schedule_data((int)completely_fair_scheduler::clazz);
}

thread_time_cf_t *get_schedule_data(thread_t *thd) { return (thread_time_cf_t *)thd->schedule_data; }

void completely_fair_scheduler::init_cpu()
{
    auto task_list = memory::New<cpu_task_list_cf_t>(memory::KernelCommonAllocatorV);
    cpu::current().set_schedule_data((int)clazz, task_list);
    auto dt = memory::New<thread_time_cf_t>(memory::KernelCommonAllocatorV);
    dt->vtime_delta = 100;
    dt->vtime = 0;
    task_list->min_vruntime = 0;
    auto &data = cpu::current().edit_load_data();
    data.last_sched_time = timer::get_high_resolution_time();
    data.last_tick_time = timer::get_high_resolution_time();

    cpu::current().get_idle_task()->schedule_data = dt;
    cpu::current().get_idle_task()->scheduler = this;
}

void completely_fair_scheduler::destroy_cpu()
{
    memory::Delete<>(memory::KernelCommonAllocatorV,
                     (cpu_task_list_cf_t *)cpu::current().get_schedule_data((int)clazz));
}

void completely_fair_scheduler::add(thread_t *thread)
{
    auto task_list = get_cpu_task_list();
    auto dt = memory::New<thread_time_cf_t>(memory::KernelCommonAllocatorV);
    dt->vtime_delta = 100;
    dt->vtime = 0;
    thread->cpuid = cpu::current().id();
    thread->schedule_data = dt;
    uctx::UninterruptibleContext icu;
    task_list->runable_list.insert(cfs_thread_t(thread));
}

void completely_fair_scheduler::remove(thread_t *thread)
{
    auto task_list = get_cpu_task_list();

    uctx::UninterruptibleContext icu;
    auto node = task_list->block_list.find(thread);
    if (node != task_list->block_list.end())
    {
        task_list->block_list.remove(node);
    }
    else
    {
        auto it = task_list->runable_list.find(cfs_thread_t(thread));
        if (it != task_list->runable_list.end())
        {
            task_list->runable_list.remove(it);
        }
        else
        {
            trace::panic("Can't find task ", thread->tid, " pid: ", thread->process->pid);
        }
    }

    auto scher_data = get_schedule_data(thread);
    thread->schedule_data = nullptr;
    memory::Delete<>(memory::KernelCommonAllocatorV, scher_data);
} // namespace task::scheduler

void completely_fair_scheduler::update_state(thread_t *thread, thread_state state)
{
    auto task_list = get_cpu_task_list();
    uctx::UninterruptibleContext icu;

    if (state == thread_state::ready)
    {
        if (thread->process->pid == 0)
        {
            thread->state = thread_state::ready;
            return;
        }
        if (thread->state == thread_state::stop)
        {
            auto node = task_list->block_list.find(thread);
            if (node != task_list->block_list.end())
            {
                thread->state = state;
                task_list->block_list.remove(node);
                thread->attributes &= ~(thread_attributes::block_to_stop);
                task_list->runable_list.insert(cfs_thread_t(thread));
                return;
            }
        }
        else if (thread->state == thread_state::running)
        {
            return;
        }
    }
    else if (state == thread_state::stop)
    {
        if (thread->state == thread_state::running)
        {
            thread->attributes |= thread_attributes::block_to_stop | thread_attributes::need_schedule;

            return;
        }
        else if (thread->state == thread_state::ready)
        {
            auto it = task_list->runable_list.find(cfs_thread_t(thread));
            if (it != task_list->runable_list.end())
            {
                thread->state = state;
                task_list->runable_list.remove(it);
                task_list->block_list.push_back(thread);
                return;
            }
        }
        else if (thread->state == task::thread_state::stop)
        {
            return;
        }
    }
    else if (state == thread_state::sched_switch_to_ready)
    {
        if (thread->process->pid == 0)
        {
            thread->state = thread_state::ready;
            return;
        }

        if (thread->attributes & thread_attributes::block_to_stop)
        {
            thread->state = thread_state::stop;
            task_list->block_list.push_back(thread);
        }
        else
        {
            if (thread->state == thread_state::running)
            {
                thread->state = thread_state::ready;
                task_list->runable_list.insert(cfs_thread_t(thread));
            }
        }
        return;
    }
    trace::panic("Unreachable control flow.", " CFS thread state:", (int)thread->state, ", to state: ", (int)state);
}

void completely_fair_scheduler::update_prop(thread_t *thread, u8 static_priority, u8 dyn_priority) {}

void completely_fair_scheduler::on_migrate(thread_t *thread)
{
    auto task_list = get_cpu_task_list();
    uctx::UninterruptibleContext icu;
    auto dt = (thread_time_cf_t *)thread->schedule_data;
    dt->vtime_delta = 100;
    dt->vtime = 0;
    thread->cpuid = cpu::current().id();
    if (thread->state == thread_state::ready)
        task_list->runable_list.insert(cfs_thread_t(thread));
    else if (thread->state == thread_state::stop)
        task_list->block_list.push_back(thread);
    else
        trace::panic("Unknown thread state when migrate(CFS). state: ", (u64)thread->state);
}

thread_t *completely_fair_scheduler::pick_available_task()
{
    auto task_list = get_cpu_task_list();

    if (task_list->runable_list.empty())
    {
        return cpu::current().get_idle_task();
    }
    auto thd = task_list->runable_list.front();
    task_list->runable_list.remove(thd);
    if (!task_list->runable_list.empty())
    {
        kassert(get_schedule_data(thd.thread)->vtime <=
                    get_schedule_data(task_list->runable_list.front().thread)->vtime,
                "CFS running list check failed!");
    }
    return thd.thread;
}

bool completely_fair_scheduler::schedule()
{
    thread_t *cur = current();
    auto &cpu = cpu::current();
    bool has_task = false;
    while (cur->attributes & task::thread_attributes::need_schedule)
    {
        uctx::UninterruptibleContext icu;

        cur->scheduler->update_state(cur, thread_state::sched_switch_to_ready);

        cur->attributes &= ~task::thread_attributes::need_schedule; ///< clean flags

        thread_t *next = pick_available_task();

        if (cur != next)
        {
            cpu.edit_load_data().last_sched_time = timer::get_high_resolution_time();
            cpu.edit_load_data().schedule_times++;
            next->state = task::thread_state::running;
            has_task = true;
            task::switch_thread(cur, next);
        }
        else
        {
            cur->state = task::thread_state::running;
        }
    }

    return has_task;
}

void completely_fair_scheduler::schedule_tick()
{
    auto cur = current();
    auto task_list = get_cpu_task_list();
    uctx::UninterruptibleContext icu;

    auto scher_data = get_schedule_data(cur);
    auto &cpu = cpu::current();
    auto ctime = timer::get_high_resolution_time();
    cpu.edit_load_data().running_task_time += ctime - cpu.edit_load_data().last_tick_time;
    cpu.edit_load_data().last_tick_time = ctime;

    scher_data->vtime += scher_data->vtime_delta;

    u64 delta = timer::get_high_resolution_time() - cpu.edit_load_data().last_sched_time;
    if (delta >= sched_min_granularity_us)
    {
        if (!task_list->runable_list.empty())
        {
            uctx::UninterruptibleContext icu;
            auto next = task_list->runable_list.front();

            if (cur->process->pid == 0 || get_schedule_data(next.thread)->vtime <= scher_data->vtime)
                cur->attributes |= thread_attributes::need_schedule;
        }
    }
}

u64 completely_fair_scheduler::scheduleable_task_count() { return get_cpu_task_list()->runable_list.size(); }

thread_t *completely_fair_scheduler::get_migratable_task(u32 cpuid)
{
    auto list = get_cpu_task_list();
    if (list->runable_list.empty())
        return nullptr;
    uctx::UninterruptibleContext icu;
    for (auto thd : list->runable_list)
    {
        if (thd.thread->cpumask.mask & (1ul << cpuid))
            return thd.thread;
    }
    return nullptr;
}

void completely_fair_scheduler::commit_migrate(thread_t *thd)
{
    auto list = get_cpu_task_list();
    uctx::UninterruptibleContext icu;

    auto it = list->runable_list.find(cfs_thread_t(thd));
    kassert(it != list->runable_list.end(), "commit task failed!");

    list->runable_list.remove(it);
}

u64 completely_fair_scheduler::sctl(int operator_type, thread_t *target, u64 attr, u64 *value, u64 size) { return 0; }

completely_fair_scheduler::completely_fair_scheduler()
    : sched_min_granularity_us(2000)
    , sched_wakeup_granularity_us(1)
{
}

} // namespace task::scheduler
