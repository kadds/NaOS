#pragma once
#include "common.hpp"
#include "kernel/util/linked_list.hpp"
#include "task.hpp"

namespace task::scheduler
{
using thread_list_t = util::linked_list<task::thread_t *>;

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
    /// \brief try if switch task
    ///
    /// \return return has switch thread
    ///
    virtual bool schedule() = 0;

    ///
    /// \brief timer tick schedule calc
    ///
    virtual void schedule_tick() = 0;

    virtual bool has_task_to_schedule() = 0;

    virtual void init_cpu() = 0;
    virtual void destroy_cpu() = 0;

    virtual u64 sctl(int operator_type, thread_t *target, u64 attr, u64 *value, u64 size) = 0;

    scheduler() = default;
    scheduler(const scheduler &) = delete;
    scheduler &operator=(const scheduler &) = delete;
    virtual ~scheduler(){};
};

void init();
void init_cpu();

void add(thread_t *thread, scheduler_class scher);
void remove(thread_t *thread);
void update_state(thread_t *thread, thread_state state);
void update_prop(thread_t *thread, u8 static_priority, u8 dyn_priority);

u64 sctl(int operator_type, thread_t *target, u64 attr, u64 *value, u64 size);

ExportC void schedule();

} // namespace task::scheduler
