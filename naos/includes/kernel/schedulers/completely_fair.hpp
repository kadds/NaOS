#pragma once
#include "kernel/lock.hpp"
#include "kernel/mm/list_node_cache.hpp"
#include "kernel/scheduler.hpp"

namespace task::scheduler
{

class completely_fair_scheduler : public scheduler
{
  private:
    timeclock::microsecond_t sched_min_granularity_us;
    timeclock::microsecond_t sched_wakeup_granularity_us;

  public:
    static const scheduler_class clazz = scheduler_class::cfs;

    void add(thread_t *thread) override;
    void remove(thread_t *thread) override;

    void update_state(thread_t *thread, thread_state state) override;

    void update_prop(thread_t *thread, u8 static_priority, u8 dyn_priority) override;

    bool schedule() override;

    void schedule_tick() override;

    u64 sctl(int operator_type, thread_t *target, u64 attr, u64 *value, u64 size) override;

    u64 scheduleable_task_count() override;

    void on_migrate(thread_t *thread) override;

    thread_t *get_migratable_task(u32 cpuid) override;

    void commit_migrate(thread_t *thd) override;

    void init_cpu() override;
    void destroy_cpu() override;
    completely_fair_scheduler();

  private:
    task::thread_t *pick_available_task();
};
} // namespace task::scheduler
