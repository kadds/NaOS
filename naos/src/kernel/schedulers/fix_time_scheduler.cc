#include "kernel/schedulers/fix_time_scheduler.hpp"
#include "kernel/mm/list_node_cache.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/task.hpp"
#include "kernel/ucontext.hpp"
#include "kernel/util/linked_list.hpp"

namespace task::scheduler
{
struct thread_timer
{
    u64 spent;
};
struct list_t
{
    thread_list_cache_t list_node_cache;
    thread_list_node_cache_allocator_t list_allocator;
    thread_list_t list;
    list_t()
        : list_node_cache(memory::KernelBuddyAllocatorV)
        , list_allocator(&list_node_cache)
        , list(&list_allocator)
    {
    }
};
thread_list_t &get_list(void *list) { return ((list_t *)list)->list; }

void fix_time_scheduler::init() { list = memory::New<list_t>(memory::KernelCommonAllocatorV); }

void fix_time_scheduler::destroy() { memory::Delete(memory::KernelCommonAllocatorV, (list_t *)list); }

void fix_time_scheduler::add(thread_t *thread)
{
    thread->schedule_data = memory::New<thread_timer>(memory::KernelCommonAllocatorV);
    auto &thl = get_list(list);
    thl.insert(thl.begin(), thread);
}

void fix_time_scheduler::remove(thread_t *thread)
{
    auto &thl = get_list(list);
    auto node = thl.find(thread);
    if (node != thl.end())
    {
        thl.remove(node);
    }

    auto thd_timer = (thread_timer *)thread->schedule_data;
    memory::Delete(memory::KernelCommonAllocatorV, thd_timer);
}

void fix_time_scheduler::schedule()
{
    auto &thl = get_list(list);
    if (current()->state != task::thread_state::running)
    {
        for (u64 i = 0; i < thl.size(); i++)
        {
            auto thd = thl.front();

            if (thd->state == task::thread_state::ready)
            {
                task::switch_thread(current(), thd);
                thl.move_back();
                return;
            }

            thl.move_back();
        }
        task::switch_thread(current(), task::get_idle_task());
    }
}

void fix_time_scheduler::set_attribute(const char *attr_name, thread_t *target, u64 value) {}

u64 fix_time_scheduler::get_attribute(const char *attr_name, thread_t *target) { return 0; }
} // namespace task::scheduler
