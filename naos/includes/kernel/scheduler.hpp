#pragma once
#include "task.hpp"
namespace task::scheduler
{
class scheduler
{
  public:
    virtual void add(thread_t *thread) = 0;
    virtual void remove(thread_t *thread) = 0;

    virtual void schedule() = 0;

    virtual void init() = 0;
    virtual void destroy() = 0;

    virtual void set_attribute(const char *attr_name, thread_t *target, u64 value) = 0;
    virtual u64 get_attribute(const char *attr_name, thread_t *target) = 0;
};

void set_scheduler(scheduler *scher);

void add(thread_t *thread);
void remove(thread_t *thread);

void schedule();

void set_attribute(const char *attr_name, thread_t *target, u64 value);
u64 get_attribute(const char *attr_name, thread_t *target);

void tick();

} // namespace task::scheduler
