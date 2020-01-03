#include "kernel/scheduler.hpp"
#include "kernel/schedulers/completely_fair.hpp"
#include "kernel/schedulers/round_robin.hpp"
#include "kernel/timer.hpp"
#include "kernel/util/hash_map.hpp"

namespace task::scheduler
{
struct hash_func
{
    u64 operator()(scheduler_class sc) { return (u64)sc; }
};

scheduler *real_time_schedulers;
scheduler *normal_schedulers;
bool is_init = false;

void timer_tick(u64 pass, u64 user_data);

void init()
{
    if (cpu::current().is_bsp())
    {
        real_time_schedulers = memory::New<round_robin_scheduler>(memory::KernelCommonAllocatorV);
        normal_schedulers = memory::New<completely_fair_scheduler>(memory::KernelCommonAllocatorV);

        is_init = true;

        timer::add_watcher(5000, timer_tick, 0);
    }
}

void init_cpu()
{
    real_time_schedulers->init_cpu();
    normal_schedulers->init_cpu();
}

void add(thread_t *thread, scheduler_class scher)
{
    if (scher == scheduler_class::round_robin)
    {
        thread->scheduler = real_time_schedulers;
        thread->attributes |= thread_attributes::real_time;
    }
    else
    {
        thread->scheduler = normal_schedulers;
    }

    thread->scheduler->add(thread);
}

void remove(thread_t *thread) { thread->attributes |= thread_attributes::remove; }

void update_state(thread_t *thread, thread_state state) { thread->scheduler->update_state(thread, state); }

void schedule()
{
    if (unlikely(!is_init))
        return;
    if (!real_time_schedulers->schedule())
    {
        normal_schedulers->schedule();
    }
}

u64 sctl(int operator_type, thread_t *target, u64 attr, u64 *value, u64 size)
{
    return target->scheduler->sctl(operator_type, target, attr, value, size);
}

void timer_tick(u64 pass, u64 user_data)
{
    timer::add_watcher(5000, timer_tick, user_data);
    thread_t *thd = current();

    if (thd->attributes & thread_attributes::real_time)
        real_time_schedulers->schedule_tick();
    else
    {
        if (real_time_schedulers->has_task_to_schedule())
        {
            thd->attributes |= thread_attributes::need_schedule;
        }
        normal_schedulers->schedule_tick();
    }
    return;
}
} // namespace task::scheduler
