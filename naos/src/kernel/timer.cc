#include "kernel/timer.hpp"
#include "kernel/arch/local_apic.hpp"
#include "kernel/arch/pit.hpp"
#include "kernel/arch/rtc.hpp"
#include "kernel/arch/tsc.hpp"
#include "kernel/clock.hpp"
#include "kernel/irq.hpp"
#include "kernel/lock.hpp"
#include "kernel/mm/list_node_cache.hpp"
#include "kernel/trace.hpp"
#include "kernel/ucontext.hpp"
#include "kernel/util/array.hpp"
#include "kernel/util/linked_list.hpp"
namespace timer
{

clock::clock_source *current_clock_source = nullptr;
clock::clock_event *current_clock_event = nullptr;

using clock_source_array_t = util::array<clock::clock_source *>;
clock_source_array_t *clock_sources;

struct watcher_t
{
    /// target time microsecond
    u64 time;
    u64 current_pass_time;
    watcher_func function;
    u64 data;
    watcher_t(){};
    watcher_t(u64 time, watcher_func func, u64 data)
        : time(time)
        , current_pass_time(0)
        , function(func)
        , data(data){};
};

using watcher_list_t = util::linked_list<watcher_t>;

watcher_list_t *watcher_list;
lock::spinlock_t watcher_list_lock;
/// save latest time time
u64 last_time;

void on_tick(u64 vector, u64 data)
{
    u64 us = current_clock_source->current();
    u64 delta = us - last_time;
    last_time = us;
    uctx::SpinLockUnInterruptableContextController ctl(watcher_list_lock);
    ctl.begin();
    for (auto it = watcher_list->begin(); it != watcher_list->end();)
    {
        auto &e = *it;

        e.current_pass_time += delta;
        if (e.current_pass_time > e.time)
        {
            e.current_pass_time -= e.time;
            ctl.end();
            bool keep = e.function(e.time, e.data);
            ctl.begin();
            if (!keep)
            {
                it = watcher_list->remove(it);
                continue;
            }
            ++it;
            continue;
        }
        ++it;
    }
    ctl.end();
}

void init()
{
    using namespace clock;
    watcher_list = memory::New<watcher_list_t>(memory::KernelCommonAllocatorV, memory::KernelCommonAllocatorV);
    clock_sources = memory::New<clock_source_array_t>(memory::KernelCommonAllocatorV, memory::KernelCommonAllocatorV);
    {
        uctx::UnInterruptableContext ctx;
        clock_sources->push_back(arch::device::PIT::make_clock());
        clock_sources->push_back(arch::TSC::make_clock());
        current_clock_source = clock_sources->back();
        clock_sources->push_back(arch::APIC::make_clock());
    }
    u64 level = 10;
    for (auto cs : *clock_sources)
    {
        cs->calibrate(clock_sources->front());
        if (cs->get_event()->get_level() > level && cs->get_event()->is_valid())
        {
            level = cs->get_event()->get_level();
            current_clock_event = cs->get_event();
        }
    }

    clock::init();
    if (current_clock_event == nullptr)
    {
        trace::panic("Can't find a availble clock event device.");
    }

    last_time = current_clock_source->current();
    current_clock_event->resume();
    current_clock_event->wait_next_tick();
    clock::start_tick();
    irq::insert_soft_request_func(irq::soft_vector::timer, on_tick, 0);
}

time::microsecond_t get_high_resolution_time() { return current_clock_source->current(); }

void add_watcher(u64 time, watcher_func func, u64 user_data)
{
    uctx::SpinLockUnInterruptableContext uic(watcher_list_lock);
    for (auto it = watcher_list->begin(); it != watcher_list->end(); ++it)
    {
        if (time < it->time)
        {
            watcher_list->insert(it, watcher_t(time, func, user_data));
            return;
        }
    }
    watcher_list->push_back(watcher_t(time, func, user_data));
}

void remove_watcher(watcher_func func)
{
    uctx::SpinLockUnInterruptableContext uic(watcher_list_lock);
    for (auto it = watcher_list->begin(); it != watcher_list->end(); ++it)
    {
        if (it->function == func)
        {
            watcher_list->remove(it);
            return;
        }
    }
}

} // namespace timer
