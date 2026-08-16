#pragma once
#include "../scheduler.hpp"
#include "freelibcxx/linked_list.hpp"
namespace task::scheduler
{
class round_robin_scheduler : public scheduler
{
    static const scheduler_class clazz = scheduler_class::round_robin;

  public:
    void add(thread_t *thread) override;
    void remove(thread_t *thread) override;

    void update_state(thread_t *thread, thread_state state) override;

    bool schedule() override;

    void schedule_tick() override;

    u64 scheduleable_task_count() override;

    void on_migrate(thread_t *thread) override;

    thread_t *get_migratable_task(u32 cpuid) override;

    void commit_migrate(thread_t *thd) override;

    void init_cpu() override;
    void destroy_cpu() override;

    round_robin_scheduler();
};

} // namespace task::scheduler
