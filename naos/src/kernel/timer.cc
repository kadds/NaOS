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
    u64 expires;
    watcher_func function;
    u64 data;
    bool enable;
    watcher_t(){};
    watcher_t(u64 expires, watcher_func func, u64 data)
        : expires(expires)
        , function(func)
        , data(data)
        , enable(true){};
};

using watcher_list_t = util::linked_list<watcher_t>;

watcher_list_t *watcher_list;
watcher_list_t *tick_list;
lock::spinlock_t watcher_list_lock, tick_list_lock;

void on_tick(u64 vector, u64 data)
{
    uctx::SpinLockUnInterruptableContext ctl2(tick_list_lock);

    u64 us = current_clock_source->current();
    auto it = tick_list->begin();
    for (; it != tick_list->end();)
    {
        if (it->expires <= us)
        {
            if (likely(it->enable))
            {
                it->function(it->expires, it->data);
            }
            it = tick_list->remove(it);
        }
        else
        {
            break;
        }
    }

    // add to tick list
    uctx::SpinLockUnInterruptableContext ctl(watcher_list_lock);

    for (auto &ws : *watcher_list)
    {
        bool has_insert = false;
        for (auto tk = tick_list->begin(); tk != tick_list->end(); ++tk)
        {
            if (ws.expires < tk->expires)
            {
                tick_list->insert(tk, ws);
                has_insert = true;
                break;
            }
        }
        if (!has_insert)
            tick_list->push_back(ws);
    }
    watcher_list->clean();
}

void init()
{
    using namespace clock;
    {
        uctx::UnInterruptableContext ctx;
        watcher_list = memory::New<watcher_list_t>(memory::KernelCommonAllocatorV, memory::KernelCommonAllocatorV);
        tick_list = memory::New<watcher_list_t>(memory::KernelCommonAllocatorV, memory::KernelCommonAllocatorV);
        clock_sources =
            memory::New<clock_source_array_t>(memory::KernelCommonAllocatorV, memory::KernelCommonAllocatorV);
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

    current_clock_event->resume();
    current_clock_event->wait_next_tick();
    current_clock_source->reinit();
    clock::start_tick();
    irq::insert_soft_request_func(irq::soft_vector::timer, on_tick, 0);
}

time::microsecond_t get_high_resolution_time() { return current_clock_source->current(); }

void add_watcher(u64 expires_delta_time, watcher_func func, u64 user_data)
{
    uctx::SpinLockUnInterruptableContext uic(watcher_list_lock);
    watcher_list->push_back(watcher_t(expires_delta_time + get_high_resolution_time(), func, user_data));
}

bool add_time_point_watcher(u64 expires_time_point, watcher_func func, u64 user_data)
{
    uctx::SpinLockUnInterruptableContext uic(watcher_list_lock);
    if (get_high_resolution_time() < expires_time_point)
    {
        watcher_list->push_back(watcher_t(expires_time_point, func, user_data));
        return true;
    }
    return false;
}

///< don't remove timer in soft irq context
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
    uctx::SpinLockUnInterruptableContext ctl2(tick_list_lock);

    for (auto it = tick_list->begin(); it != tick_list->end();)
    {
        if (it->function <= func)
        {
            it->enable = false;
            return;
        }
        else
        {
            ++it;
        }
    }
}

} // namespace timer
