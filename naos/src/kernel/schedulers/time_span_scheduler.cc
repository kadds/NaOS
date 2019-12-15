#include "kernel/schedulers/time_span_scheduler.hpp"
#include "kernel/clock.hpp"
#include "kernel/cpu.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/task.hpp"
#include "kernel/timer.hpp"
#include "kernel/ucontext.hpp"

namespace task::scheduler
{
struct thread_time
{
    i64 rest_time;
    u64 priority;
};

void timer_tick(u64 pass, u64 user_data)
{
    time_span_scheduler *scher = (time_span_scheduler *)user_data;
    if (unlikely(scher == nullptr))
        return;

    timer::add_watcher(5000, timer_tick, user_data);
    scher->schedule_tick();
    return;
}

thread_time *get_schedule_data(task::thread_t *t) { return (thread_time *)t->schedule_data; }

u64 priority(task::thread_t *t) { return get_schedule_data(t)->priority; }

void gen_priority(task::thread_t *t)
{
    get_schedule_data(t)->priority = (t->static_priority * 10ul + t->dynamic_priority) / 10;
}

cpu_task_list_t::cpu_task_list_t()
    : runable_list(&list_node_allocator)
    , expired_list(&list_node_allocator)
    , block_list(&list_node_allocator)

{
}

time_span_scheduler::time_span_scheduler() {}

void time_span_scheduler::init()
{
    uctx::UnInterruptableContext icu;
    timer::add_watcher(5000, timer_tick, (u64)this);
    last_time_millisecond = timer::get_high_resolution_time();
}

void time_span_scheduler::destroy() { timer::remove_watcher(timer_tick); }

void time_span_scheduler::init_cpu()
{
    cpu_task_list_t *task_list = memory::New<cpu_task_list_t>(memory::KernelCommonAllocatorV);
    cpu::current().set_schedule_data(task_list);
    task::get_idle_task()->schedule_data = memory::New<thread_time>(memory::KernelCommonAllocatorV);
    get_schedule_data(task::get_idle_task())->rest_time = 0;
}

void time_span_scheduler::destroy_cpu()
{
    memory::Delete<>(memory::KernelCommonAllocatorV, (cpu_task_list_t *)cpu::current().get_schedule_data());
}

void time_span_scheduler::add(thread_t *thread)
{
    auto &u = cpu::current();
    cpu_task_list_t *task_list = (cpu_task_list_t *)u.get_schedule_data();
    uctx::SpinLockUnInterruptableContext uic(task_list->list_spinlock);
    thread->schedule_data = memory::New<thread_time>(memory::KernelCommonAllocatorV);
    gen_priority(thread);
    get_schedule_data(thread)->rest_time = task_list->runable_list.size() * 1000 /
                                           (task_list->runable_list.size() + task_list->expired_list.size() + 1) / 1000;
    insert_to_runable_list(thread);
}

void time_span_scheduler::remove(thread_t *thread)
{
    auto &u = cpu::current();
    cpu_task_list_t *task_list = (cpu_task_list_t *)u.get_schedule_data();
    uctx::SpinLockUnInterruptableContext uic(task_list->list_spinlock);
    auto node = task_list->block_list.find(thread);
    if (node != task_list->block_list.end())
    {
        task_list->block_list.remove(node);
    }
    else
    {
        node = task_list->expired_list.find(thread);
        if (node != task_list->expired_list.end())
        {
            task_list->expired_list.remove(node);
        }
        else
        {
            node = task_list->runable_list.find(thread);
            if (node != task_list->runable_list.end())
            {
                task_list->runable_list.remove(node);
            }
            else
            {
                auto &cpu = cpu::current();
                if (cpu.get_task() != thread)
                {
                    trace::panic("Can not remove task");
                }
            }
        }
    }
    auto scher_time = get_schedule_data(thread);
    memory::Delete(memory::KernelCommonAllocatorV, scher_time);
}

void time_span_scheduler::update(thread_t *thread)
{
    auto &u = cpu::current();
    cpu_task_list_t *task_list = (cpu_task_list_t *)u.get_schedule_data();
    uctx::SpinLockUnInterruptableContext uic(task_list->list_spinlock);

    if (thread->state == task::thread_state::ready)
    {
        auto node = task_list->block_list.find(thread);
        if (node != task_list->block_list.end())
        {
            task_list->block_list.remove(node);
            thread->attributes &= ~thread_attributes::block;
            insert_to_runable_list(thread);
        }
    }
    else if (thread->state == task::thread_state::interruptable || thread->state == task::thread_state::uninterruptible)
    {
        thread->attributes |= thread_attributes::block;

        if (current() == thread)
        {
            return;
        }
        auto node = task_list->runable_list.find(thread);
        if (node != task_list->runable_list.end())
        {
            task_list->runable_list.remove(node);
            task_list->block_list.push_back(thread);
            return;
        }
        node = task_list->expired_list.find(thread);
        if (node != task_list->expired_list.end())
        {
            task_list->expired_list.remove(node);
            task_list->block_list.push_back(thread);
            return;
        }
    }
    else
        trace::panic("Unreachable control flow.\n");
}

void time_span_scheduler::insert_to_runable_list(thread_t *t)
{
    auto &u = cpu::current();
    cpu_task_list_t *task_list = (cpu_task_list_t *)u.get_schedule_data();
    auto p = priority(t);
    for (auto it = task_list->runable_list.begin(); it != task_list->runable_list.end(); ++it)
    {
        if (priority(*it) > p)
        {
            task_list->runable_list.insert(it, t);
            return;
        }
    }
    task_list->runable_list.push_back(t);
}

void time_span_scheduler::epoch()
{
    auto &u = cpu::current();
    cpu_task_list_t *task_list = (cpu_task_list_t *)u.get_schedule_data();
    uctx::SpinLockUnInterruptableContext uic(task_list->list_spinlock);

    u64 count = task_list->expired_list.size();
    if (count == 0)
        return;
    u64 full_priority = 1;
    // 1ms
    u64 epoch_time = 1000000;
    for (auto it : task_list->expired_list)
    {
        gen_priority(it);
        full_priority += get_schedule_data(it)->priority;
        insert_to_runable_list(it);
    }
    task_list->expired_list.clean();

    for (auto it : task_list->runable_list)
    {
        // 1000
        get_schedule_data(it)->rest_time +=
            1000 + get_schedule_data(it)->priority * 1000 / full_priority * epoch_time / 1000;
    }
}

thread_t *time_span_scheduler::pick_available_task()
{
    auto &u = cpu::current();
    cpu_task_list_t *task_list = (cpu_task_list_t *)u.get_schedule_data();
    if (task_list->runable_list.empty())
    {
        return task::get_idle_task();
    }
    thread_t *t = task_list->runable_list.front();
    task_list->runable_list.remove(task_list->runable_list.begin());
    return t;
}

void time_span_scheduler::schedule(flag_t flag)
{
    if (unlikely(current() == nullptr))
    {
        return;
    }
    thread_t *cur = current();

    if (flag & schedule_flags::current_remove)
    {
        uctx::UnInterruptableContext icu;
        auto &u = cpu::current();
        cpu_task_list_t *task_list = (cpu_task_list_t *)u.get_schedule_data();
        uctx::SpinLockContextController ctr(task_list->list_spinlock);
        task_list->list_spinlock.lock();
        thread_t *next = pick_available_task();
        task_list->list_spinlock.unlock();
        next->state = task::thread_state::running;
        task::switch_thread(cur, next);
    }
    else
    {
        while (cur->attributes & task::thread_attributes::need_schedule)
        {
            uctx::UnInterruptableContext icu;
            auto &u = cpu::current();
            cpu_task_list_t *task_list = (cpu_task_list_t *)u.get_schedule_data();
            if (cur->attributes & task::thread_attributes::block)
            {
                task_list->block_list.push_back(cur);
            }
            else if (get_schedule_data(cur)->rest_time <= 0 && cur != task::get_idle_task())
            {
                task_list->expired_list.push_back(cur);
            }
            cur->attributes &= ~task::thread_attributes::need_schedule; ///< clean flags

            if (task_list->runable_list.empty())
                epoch();

            uctx::SpinLockContextController ctr(task_list->list_spinlock);
            task_list->list_spinlock.lock();
            thread_t *next = pick_available_task();
            task_list->list_spinlock.unlock();

            if (cur != next)
            {
                next->state = task::thread_state::running;
                if (cur->state == task::thread_state::running)
                {
                    cur->state = task::thread_state::ready; ///< return state to ready
                }
                task::switch_thread(cur, next);
            }
            else
            {
                cur->state = task::thread_state::running;
            }
        }
    }

    return;
}

void time_span_scheduler::schedule_tick()
{
    if (unlikely(current() == nullptr))
        return;
    uctx::UnInterruptableContext icu;
    auto cur = current();
    u64 cur_time = timer::get_high_resolution_time();
    u64 delta = cur_time - last_time_millisecond;
    last_time_millisecond = cur_time;
    if (cur->state != task::thread_state::running)
        return;
    auto scher_data = get_schedule_data(cur);
    scher_data->rest_time -= delta;
    if (scher_data->rest_time <= 0)
    {
        cur->attributes |= task::thread_attributes::need_schedule;
    }
}

void time_span_scheduler::set_attribute(const char *attr_name, thread_t *target, u64 value) {}

u64 time_span_scheduler::get_attribute(const char *attr_name, thread_t *target) { return 0; }
} // namespace task::scheduler
