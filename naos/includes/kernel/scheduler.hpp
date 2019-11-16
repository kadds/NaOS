#pragma once
#include "common.hpp"

namespace task
{
struct thread_t;
struct process_t;
}; // namespace task

namespace task::scheduler
{
namespace schedule_flags
{
enum : flag_t
{
    current_remove = 1,
};
} // namespace schedule_flags

/// Interface of all scheduler
class scheduler
{
  public:
    virtual void add(thread_t *thread) = 0;
    virtual void remove(thread_t *thread) = 0;
    /// called when priority is updated
    virtual void update(thread_t *thread) = 0;

    virtual void schedule(flag_t flag) = 0;
    virtual void schedule_tick() = 0;

    virtual void init() = 0;
    virtual void destroy() = 0;

    virtual void set_attribute(const char *attr_name, thread_t *target, u64 value) = 0;
    virtual u64 get_attribute(const char *attr_name, thread_t *target) = 0;

    scheduler() = default;
    scheduler(const scheduler &) = delete;
    scheduler &operator=(const scheduler &) = delete;
};

void set_scheduler(scheduler *scher);
void add(thread_t *thread);
void remove(thread_t *thread);
void update(thread_t *thread);
ExportC void schedule();
void schedule_tick();
void set_attribute(const char *attr_name, thread_t *target, u64 value);
u64 get_attribute(const char *attr_name, thread_t *target);
void force_schedule();

} // namespace task::scheduler
