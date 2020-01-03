#pragma once
#include "common.hpp"
namespace task
{
struct thread_t;
} // namespace task

namespace cpu
{

class cpu_data_t
{
    task::thread_t *current_task = nullptr;
    task::thread_t *idle_task = nullptr;
    void *schedule_data[2];
    void *tasklet_queue = nullptr;

  public:
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
};
cpu_data_t &current();
void init();
u64 count();
} // namespace cpu
