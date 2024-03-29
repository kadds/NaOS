#pragma once
#include "common.hpp"
#include "freelibcxx/linked_list.hpp"
#include "task.hpp"

namespace task::scheduler
{
using thread_list_t = freelibcxx::linked_list<task::thread_t *>;

enum class scheduler_class : flag_t
{
    round_robin,
    cfs,
};

///
/// \brief interface of all schedulers
///
///
class scheduler
{
  public:
    ///
    /// \brief add task to scheduler
    ///
    /// \param thread the task thread to add
    ///
    virtual void add(thread_t *thread) = 0;
    ///
    /// \brief remove task from scheduler
    ///
    /// \param thread the task to remove
    ///
    virtual void remove(thread_t *thread) = 0;
    ///
    /// \brief update task state
    ///
    /// \param thread
    /// \param state state to switch
    /// \note called when state is updated
    ///
    virtual void update_state(thread_t *thread, thread_state state) = 0;

    virtual void update_prop(thread_t *thread, u8 static_priority, u8 dyn_priority) = 0;

    ///
    /// \brief call when the task is migrating to other cpu
    ///
    /// \param thread
    ///
    virtual void on_migrate(thread_t *thread) = 0;

    ///
    /// \brief try if switch task
    ///
    /// \return return has switch thread
    ///
    virtual bool schedule() = 0;

    ///
    /// \brief timer tick schedule calc
    ///
    virtual void schedule_tick() = 0;

    ///
    /// \brief return task count which can scheduleable (unmasked, ready state)
    ///
    virtual u64 scheduleable_task_count() = 0;

    ///
    /// \brief select a task to migrate
    ///
    /// \param cpuid cpu to migrate
    /// \return task
    virtual thread_t *get_migratable_task(u32 cpuid) = 0;

    ///
    /// \brief commit task to migrate
    ///
    /// \param thd task
    ///
    virtual void commit_migrate(thread_t *thd) = 0;

    /// Initialize per cpu data
    virtual void init_cpu() = 0;
    virtual void destroy_cpu() = 0;

    ///
    /// \brief control scheduler
    ///
    virtual u64 sctl(int operator_type, thread_t *target, u64 attr, u64 *value, u64 size) = 0;

    scheduler() = default;
    scheduler(const scheduler &) = delete;
    scheduler &operator=(const scheduler &) = delete;
    virtual ~scheduler(){};
};

void init();
void init_cpu();

typedef void (*remove_func)(u64 data);

void add(thread_t *thread, scheduler_class scher);
void remove(thread_t *thread, remove_func, u64 user_data);
void update_state(thread_t *thread, thread_state state);
void update_prop(thread_t *thread, u8 static_priority, u8 dyn_priority);
bool reschedule_task_push(thread_t *task, u32 cpuid);
bool reschedule_task_pull(thread_t *task);

u64 sctl(int operator_type, thread_t *target, u64 attr, u64 *value, u64 size);

ExportC void schedule();

bool migrate_pre_check();

} // namespace task::scheduler
