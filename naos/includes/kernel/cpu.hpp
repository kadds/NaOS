#pragma once
#include "common.hpp"
namespace task
{
struct thread_t;
} // namespace task
namespace clock
{
class clock_source;
class clock_event;
}; // namespace clock

namespace cpu
{
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

class cpu_data_t
{
    task::thread_t *current_task = nullptr;
    task::thread_t *idle_task = nullptr;
    void *schedule_data[2];
    void *tasklet_queue = nullptr;
    load_data_t load_data;
    void *timer_queue;

    clock::clock_source *clock_source;
    clock::clock_event *clock_ev;
    void *clock_queue;

  public:
    cpu_data_t() = default;
    cpu_data_t(const cpu_data_t &) = delete;
    cpu_data_t &operator=(const cpu_data_t &) = delete;

    bool is_bsp();
    int id();
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

    void set_clock_source(clock::clock_source *cs) { clock_source = cs; }
    clock::clock_source *get_clock_source() { return clock_source; }

    void set_clock_event(clock::clock_event *ev) { clock_ev = ev; }
    clock::clock_event *get_clock_event() { return clock_ev; }

    void *get_clock_queue() { return clock_queue; }

    void set_clock_queue(void *q) { clock_queue = q; }
};
cpu_data_t &current();
void init();
u64 count();
cpu_data_t &get(u32 id);
} // namespace cpu
