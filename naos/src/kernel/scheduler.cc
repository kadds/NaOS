#include "kernel/scheduler.hpp"
namespace task::scheduler
{
scheduler *global_scheduler = nullptr;

void set_scheduler(scheduler *scher)
{
    scher->init();
    if (global_scheduler != nullptr)
    {
        global_scheduler->destroy();
    }
    global_scheduler = scher;
}

void add(thread_t *thread) { global_scheduler->add(thread); }

void remove(thread_t *thread) { global_scheduler->remove(thread); }
void update(thread_t *thread) { global_scheduler->update(thread); }

void schedule()
{
    if (likely(global_scheduler != nullptr))
        global_scheduler->schedule();
}

void schedule_tick() { global_scheduler->schedule_tick(); }

void set_attribute(const char *attr_name, thread_t *target, u64 value)
{
    global_scheduler->set_attribute(attr_name, target, value);
}

u64 get_attribute(const char *attr_name, thread_t *target)
{
    return global_scheduler->get_attribute(attr_name, target);
}
} // namespace task::scheduler
