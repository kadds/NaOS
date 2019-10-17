#pragma once
#include "kernel/lock.hpp"
#include "kernel/mm/list_node_cache.hpp"
#include "kernel/scheduler.hpp"
#include "kernel/util/linked_list.hpp"

namespace task::scheduler
{
class time_span_scheduler : public scheduler
{
  private:
    using thread_list_t = util::linked_list<task::thread_t *>;
    using thread_list_cache_allocator_t = memory::list_node_cache_allocator<thread_list_t>;

  private:
    thread_list_cache_allocator_t list_node_allocator;
    thread_list_t runable_list;
    thread_list_t wait_list;
    lock::spinlock_t list_spinlock;

    u64 last_time_millisecond;
    u32 epoch_time;
    u32 current_epoch;

  public:
    void add(thread_t *thread) override;
    void remove(thread_t *thread) override;
    void update(thread_t *thread) override;

    void schedule() override;
    void schedule_tick() override;

    void init() override;
    void destroy() override;

    void epoch();

    void set_attribute(const char *attr_name, thread_t *target, u64 value) override;
    u64 get_attribute(const char *attr_name, thread_t *target) override;

    time_span_scheduler();
};
} // namespace task::scheduler
