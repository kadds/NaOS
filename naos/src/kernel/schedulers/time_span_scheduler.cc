#include "kernel/schedulers/time_span_scheduler.hpp"
#include "kernel/clock.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/task.hpp"
#include "kernel/timer.hpp"
#include "kernel/ucontext.hpp"

namespace task::scheduler
{
struct thread_time
{
    i64 rest_time;
};

bool timer_tick(u64 pass, u64 user_data)
{
    time_span_scheduler *scher = (time_span_scheduler *)user_data;
    if (unlikely(scher == nullptr))
        return true;

    scher->schedule_tick();
    return true;
}

thread_time *get_schedule_data(task::thread_t *t) { return (thread_time *)t->schedule_data; }

u64 goodness(task::thread_t *t)
{
    u64 weight = t->static_priority * 10ul + t->dynamic_priority;
    return weight;
}

time_span_scheduler::time_span_scheduler()
    : runable_list(&list_node_allocator)
    , wait_list(&list_node_allocator)
{
}

void time_span_scheduler::init()
{
    timer::add_watcher(5000, timer_tick, (u64)this);
    last_time_millisecond = timer::get_high_resolution_time();
    epoch_time = 50000;
    current_epoch = 0;

    task::get_idle_task()->schedule_data = memory::New<thread_time>(memory::KernelCommonAllocatorV);
    get_schedule_data(task::get_idle_task())->rest_time = -1;
}

void time_span_scheduler::destroy() { timer::remove_watcher(timer_tick); }

void time_span_scheduler::add(thread_t *thread)
{
    uctx::SpinLockUnInterruptableContext uic(list_spinlock);
    thread->schedule_data = memory::New<thread_time>(memory::KernelCommonAllocatorV);

    get_schedule_data(thread)->rest_time = 1;
    runable_list.insert(runable_list.begin(), thread);
    // min 50ms
    epoch_time = 50000 + (runable_list.size() + wait_list.size()) * 2000;
    // max 300ms
    epoch_time = epoch_time > 300000 ? 300000 : epoch_time;
}

void time_span_scheduler::remove(thread_t *thread)
{
    uctx::SpinLockUnInterruptableContext uic(list_spinlock);

    auto node = runable_list.find(thread);
    if (node != runable_list.end())
    {
        runable_list.remove(node);
        auto sche_time = get_schedule_data(thread);
        memory::Delete(memory::KernelCommonAllocatorV, sche_time);
    }
    else
    {
        auto node = wait_list.find(thread);
        if (node != wait_list.end())
        {
            wait_list.remove(node);
            auto sche_time = get_schedule_data(thread);
            memory::Delete(memory::KernelCommonAllocatorV, sche_time);
        }
    }
}

void time_span_scheduler::update(thread_t *thread) {}

void time_span_scheduler::epoch()
{
    uctx::SpinLockUnInterruptableContext uic(list_spinlock);

    for (auto thread : wait_list)
    {
        get_schedule_data(thread)->rest_time /= 2;
    }

    u64 count = runable_list.size() + wait_list.size();
    if (count == 0)
        return;
    u64 tc = epoch_time / count;
    current_epoch = 0;

    for (auto thread : wait_list)
    {
        get_schedule_data(thread)->rest_time += tc;
        runable_list.push_back(thread);
    }
    wait_list.clean();
}

void time_span_scheduler::schedule()
{
    if (unlikely(current() == nullptr))
    {
        return;
    }

    while (current()->attributes & task::thread_attributes::need_schedule)
    {
        uctx::SpinLockUnInterruptableContext uic(list_spinlock);
        thread_t *next = task::get_idle_task();
        u64 cur_time = timer::get_high_resolution_time();
        u64 delta = cur_time - last_time_millisecond;
        last_time_millisecond = cur_time;

        current_epoch += delta;

        if (epoch_time <= current_epoch)
        {
            // swap
            epoch();
        }
        if (current() != next)
        {
            runable_list.remove(runable_list.find(current()));
            wait_list.push_back(current());
        }

        if (!runable_list.empty())
        {
            current()->state = task::thread_state::ready;

            u64 max_weight = 0;
            for (auto thd : runable_list)
            {
                u64 weight = goodness(thd);
                if (max_weight <= weight)
                {
                    weight = max_weight;
                    next = thd;
                }
            }
        }
        current()->attributes &= ~task::thread_attributes::need_schedule;
        if (current() != next)
            task::switch_thread(current(), next);
    }
    return;
}

void time_span_scheduler::schedule_tick()
{
    if (unlikely(current() == nullptr))
        return;

    u64 cur_time = timer::get_high_resolution_time();
    u64 delta = cur_time - last_time_millisecond;
    last_time_millisecond = cur_time;
    if (current()->state != task::thread_state::running)
        return;
    auto sche_data = get_schedule_data(current());
    sche_data->rest_time -= delta;
    if (sche_data->rest_time <= 0)
    {
        current()->attributes |= task::thread_attributes::need_schedule;
    }
}

void time_span_scheduler::set_attribute(const char *attr_name, thread_t *target, u64 value) {}

u64 time_span_scheduler::get_attribute(const char *attr_name, thread_t *target) { return 0; }
} // namespace task::scheduler
