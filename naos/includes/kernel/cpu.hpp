#pragma once
#include "common.hpp"
#include "lock.hpp"
#include "wait.hpp"

namespace task
{
struct thread_t;
} // namespace task
namespace timeclock
{
class clock_source;
class clock_event;
} // namespace timeclock

namespace cpu
{
typedef void (*call_cpu_func_t)(u64 user_data);
typedef void (*task_func)(u64);
struct next_schedule_microtask_data_t
{
    task_func func;
    u64 data;
};

struct load_data_t
{
    u64 last_sched_time = 0;
    u64 last_tick_time = 0;
    u64 running_task_time = 0;
    u64 schedule_times = 0;

    u64 load_calc_times = 0;
    u64 last_load_time = 0;
    u64 calcing_load_fac = 0;

    u64 recent_load_fac = 0;
};

/// per cpu data
class cpu_data_t
{
    task::thread_t *current_task = nullptr;
    task::thread_t *idle_task = nullptr;
    u32 smp_id;
    void *schedule_data[2];
    void *tasklet_queue = nullptr;
    load_data_t load_data;
    void *timer_queue;

    timeclock::clock_source *clock_source = nullptr;
    timeclock::clock_event *clock_ev = nullptr;
    void *clock_queue = nullptr;

    call_cpu_func_t cpu_func;
    u64 call_data;
    lock::spinlock_t call_lock;

    task::wait_queue_t *soft_irq_wait_queue;

    freelibcxx::linked_list<next_schedule_microtask_data_t> schedule_microtask_queue;
    lock::spinlock_t microtask_lock;

  public:
    friend void init();
    cpu_data_t()
        : schedule_microtask_queue(memory::KernelCommonAllocatorV){};
    cpu_data_t(const cpu_data_t &) = delete;
    cpu_data_t &operator=(const cpu_data_t &) = delete;

    bool is_bsp();
    u32 id();
    void set_task(task::thread_t *task);

    task::thread_t *get_task() { return current_task; }
    void set_idle_task(::task::thread_t *task) { idle_task = task; }
    task::thread_t *get_idle_task() { return idle_task; }

    bool has_task() { return current_task != nullptr; }

    void set_schedule_data(u64 index, void *data) { schedule_data[index] = data; }

    void *get_schedule_data(u64 index) { return schedule_data[index]; }

    void *get_tasklet_queue() { return tasklet_queue; }

    void set_tasklet_queue(void *queue) { tasklet_queue = queue; }

    load_data_t &edit_load_data() { return load_data; }

    void *get_timer_queue() { return timer_queue; }

    void set_timer_queue(void *timer_queue) { this->timer_queue = timer_queue; }

    void set_clock_source(timeclock::clock_source *cs) { clock_source = cs; }
    timeclock::clock_source *get_clock_source() { return clock_source; }

    void set_clock_event(timeclock::clock_event *ev) { clock_ev = ev; }
    timeclock::clock_event *get_clock_event() { return clock_ev; }

    void *get_clock_queue() { return clock_queue; }

    void set_clock_queue(void *q) { clock_queue = q; }

    void set_call_cpu_func(call_cpu_func_t func, u64 user_data)
    {
        this->cpu_func = func;
        this->call_data = user_data;
    }

    void call_cpu() { cpu_func(call_data); }

    lock::spinlock_t &cpu_call_lock() { return call_lock; }

    task::wait_queue_t *get_soft_irq_wait_queue() { return soft_irq_wait_queue; }

    freelibcxx::linked_list<next_schedule_microtask_data_t> &get_microtask_queue() { return schedule_microtask_queue; }
    lock::spinlock_t &get_microtask_lock() { return microtask_lock; }
};
cpu_data_t &current();
bool has_init();
void init();
/// get cpu count
u64 count();
/// get cpu[id] data
cpu_data_t &get(u32 id);
} // namespace cpu
