#include "kernel/scheduler.hpp"
#include "kernel/irq.hpp"
#include "kernel/schedulers/completely_fair.hpp"
#include "kernel/schedulers/round_robin.hpp"
#include "kernel/smp.hpp"
#include "kernel/timer.hpp"
#include "kernel/util/hash_map.hpp"

namespace task::scheduler
{

scheduler *real_time_schedulers;
scheduler *normal_schedulers;

void timer_tick(u64 pass, u64 user_data);

std::atomic_bool is_init = false;

task::thread_t *thread_to_reschedule;

std::atomic_bool reschedule_ready;

irq::request_result reschedule_func(const void *regs, u64 data, u64 user_data)
{
    task::thread_t *thd = thread_to_reschedule;

    if (!reschedule_ready)
    {
        thd->scheduler->on_migrate(thd);
        thd->attributes &= ~(thread_attributes::on_migrate);
    }
    reschedule_ready = true;
    return irq::request_result::ok;
}

bool reschedule_task_push(thread_t *task, u32 cpuid)
{
    if (reschedule_ready)
    {
        if (!reschedule_ready.exchange(false))
            return false;
        // trace::debug("reschedule task cpu ", task->cpuid, " to cpu ", cpuid);

        thread_to_reschedule = task;
        task->cpuid = cpuid;
        task->attributes |= thread_attributes::on_migrate;

        SMP::reschedule_cpu(cpuid);
        return true;
    }
    return false;
}

void init()
{
    if (cpu::current().is_bsp())
    {
        real_time_schedulers = memory::New<round_robin_scheduler>(memory::KernelCommonAllocatorV);
        normal_schedulers = memory::New<completely_fair_scheduler>(memory::KernelCommonAllocatorV);

        is_init = true;
        reschedule_ready = true;
        irq::insert_request_func(irq::hard_vector::IPI_reschedule, reschedule_func, 0);
    }
    timer::add_watcher(5000, timer_tick, 0);
}

void init_cpu()
{
    while (!is_init)
    {
        cpu_pause();
    }
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

struct update_state_ipi_param
{
    thread_t *thread;
    thread_state state;
    update_state_ipi_param(thread_t *thread, thread_state state)
        : thread(thread)
        , state(state)
    {
    }
};

void update_state_ipi(u64 data)
{
    update_state_ipi_param *p = (update_state_ipi_param *)data;
    p->thread->scheduler->update_state(p->thread, p->state);

    memory::Delete<>(memory::KernelCommonAllocatorV, p);
}

void update_state(thread_t *thread, thread_state state)
{
    kassert(thread->state != state, "BUG check failed");

    if (thread->cpuid == cpu::current().id())
    {
        thread->scheduler->update_state(thread, state);
    }
    else
    {
        SMP::call_cpu(thread->cpuid, update_state_ipi,
                      (u64)memory::New<update_state_ipi_param>(memory::KernelCommonAllocatorV, thread, state));
        // send IPI
    }
}

struct remove_task__ipi_param
{
    thread_t *thread;
    remove_func func;
    u64 data;
    remove_task__ipi_param(thread_t *thread, remove_func func, u64 data)
        : thread(thread)
        , func(func)
        , data(data)
    {
    }
};

void remove_task_ipi(u64 data)
{
    remove_task__ipi_param *p = (remove_task__ipi_param *)data;
    p->thread->scheduler->remove(p->thread);
    auto d = p->data;
    auto func = p->func;
    memory::Delete<>(memory::KernelCommonAllocatorV, p);
    func(d);
}

void remove(thread_t *thread, remove_func func, u64 user_data)
{
    if (thread->attributes & thread_attributes::on_migrate)
        cpu_pause();

    if (thread->cpuid == cpu::current().id())
    {
        thread->scheduler->update_state(thread, thread_state::stop);
        auto &lock = cpu::current().get_microtask_lock();
        cpu::next_schedule_microtask_data_t data;
        data.data = user_data;
        data.func = func;
        uctx::RawSpinLockUninterruptibleContext icu(lock);
        cpu::current().get_microtask_queue().push_back(data);
    }
    else
    {
        SMP::call_cpu(
            thread->cpuid, remove_task_ipi,
            (u64)memory::New<remove_task__ipi_param>(memory::KernelCommonAllocatorV, thread, func, user_data));
    }
}

void schedule()
{
    if (unlikely(!is_init))
        return;
    if (current() && !current()->preempt_data.preemptible())
    {
        trace::panic("schedule no preemptible.");
    }
    task::disable_preempt();

    if (!real_time_schedulers->schedule())
    {
        normal_schedulers->schedule();
    }
    auto &lock = cpu::current().get_microtask_lock();
    uctx::RawSpinLockUninterruptibleController icu(lock);
    icu.begin();
    auto &cpu = cpu::current();
    auto &q = cpu.get_microtask_queue();
    while (!q.empty())
    {
        auto data = cpu::current().get_microtask_queue().pop_back();
        icu.end();
        data.func(data.data);
        icu.begin();
    }
    icu.end();

    task::enable_preempt();
}

u64 sctl(int operator_type, thread_t *target, u64 attr, u64 *value, u64 size)
{
    return target->scheduler->sctl(operator_type, target, attr, value, size);
}

void timer_tick(u64 pass, u64 user_data)
{
    timer::add_watcher(5000, timer_tick, user_data);
    thread_t *thd = current();

    uctx::UninterruptibleContext icu;

    if (thd->attributes & thread_attributes::real_time)
        real_time_schedulers->schedule_tick();
    else
    {
        if (real_time_schedulers->scheduleable_task_count() > 0)
        {
            thd->attributes |= thread_attributes::need_schedule;
        }
        normal_schedulers->schedule_tick();
    }
    if (!reload_load_fac())
        return; /// time too short. do it after

    u64 cpu_count = cpu::count();
    u64 min_fac = cpu::current().edit_load_data().recent_load_fac;
    u64 cur_id = cpu::current().id();
    cpu::cpu_data_t *targe_cpu = nullptr;
    for (u64 i = 0; i < cpu_count; i++)
    {
        if (cur_id == i)
            continue;
        auto &cpu = cpu::get(i);
        if (min_fac >= cpu.edit_load_data().recent_load_fac)
        {
            min_fac = cpu.edit_load_data().recent_load_fac;
            targe_cpu = &cpu;
        }
    }

    if (targe_cpu != nullptr && (cpu::current().edit_load_data().recent_load_fac - min_fac) > 100)
    {
        auto task = real_time_schedulers->get_migratable_task(targe_cpu->id());
        if (task != nullptr)
        {
            if (reschedule_task_push(task, targe_cpu->id()))
            {
                real_time_schedulers->commit_migrate(task);
                return;
            }
        }

        task = normal_schedulers->get_migratable_task(targe_cpu->id());
        if (task != nullptr)
        {
            if (reschedule_task_push(task, targe_cpu->id()))
            {
                normal_schedulers->commit_migrate(task);
            }
        }
    }

    return;
}
// 10ms allow to reschedule
constexpr u64 load_calc_time_span = 10000;
constexpr u64 load_calc_times = 5;

bool reload_load_fac()
{
    auto &cpu = cpu::current();
    auto &data = cpu.edit_load_data();
    auto time = timer::get_high_resolution_time();
    auto dt = time - data.last_load_time;
    if (dt < load_calc_time_span)
        return false;
    data.last_load_time = time;

    data.calcing_load_fac +=
        (real_time_schedulers->scheduleable_task_count() + normal_schedulers->scheduleable_task_count()) * 100;

    if (++data.load_calc_times >= load_calc_times)
    {
        data.recent_load_fac = data.calcing_load_fac / data.load_calc_times;
        data.load_calc_times = 0;
        return true;
    }
    return false;
}
} // namespace task::scheduler
