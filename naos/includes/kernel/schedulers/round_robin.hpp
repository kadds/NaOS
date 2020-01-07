#pragma once
#include "../scheduler.hpp"
#include "../util/linked_list.hpp"
namespace task::scheduler
{
class round_robin_scheduler : public scheduler
{
    static const scheduler_class clazz = scheduler_class::round_robin;
    thread_list_t block_threads;
    std::atomic_ulong task_ready_count;

  public:
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

    round_robin_scheduler();
};

} // namespace task::scheduler
