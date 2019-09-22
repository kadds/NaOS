#pragma once
#include "kernel/scheduler.hpp"
namespace task::scheduler
{
class fix_time_scheduler : public scheduler
{
  private:
    void *list;

  public:
    void add(thread_t *thread) override;
    void remove(thread_t *thread) override;

    void schedule() override;

    void init() override;
    void destroy() override;

    void set_attribute(const char *attr_name, thread_t *target, u64 value) override;
    u64 get_attribute(const char *attr_name, thread_t *target) override;
};
} // namespace task::scheduler
