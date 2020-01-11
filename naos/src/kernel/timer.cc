#include "kernel/timer.hpp"
#include "kernel/arch/local_apic.hpp"
#include "kernel/arch/pit.hpp"
#include "kernel/arch/rtc.hpp"
#include "kernel/arch/tsc.hpp"
#include "kernel/clock.hpp"
#include "kernel/cpu.hpp"
#include "kernel/irq.hpp"
#include "kernel/lock.hpp"
#include "kernel/mm/list_node_cache.hpp"
#include "kernel/trace.hpp"
#include "kernel/ucontext.hpp"
#include "kernel/util/array.hpp"
#include "kernel/util/circular_buffer.hpp"
#include "kernel/util/linked_list.hpp"
#include "kernel/util/skip_list.hpp"

namespace timer
{

using clock_source_array_t = util::array<clock::clock_source *>;

struct watcher_t
{
    /// target time microsecond
    u64 expires;
    watcher_func function;
    u64 data;

    /// HACK: save bit 1 to function

    void set_enable() { function = (watcher_func)((u64)function | (1ul << 63)); }

    void clear_enable() { function = (watcher_func)((u64)function & ~(1ul << 63)); }

    bool is_enable() { return (u64)function >> 63; }

    watcher_t(u64 expires, watcher_func func, u64 data)
        : expires(expires)
        , function(func)
        , data(data){};

    bool operator==(const watcher_t &w) { return expires == w.expires && function == w.function && data == w.data; }
    bool operator<(const watcher_t &w) { return expires < w.expires; }
};

using watcher_list_t = util::array<watcher_t>;
using tick_list_t = util::skip_list<watcher_t>;
using recent_list_t = util::circular_buffer<watcher_t>;

struct cpu_timer_t
{
    watcher_list_t watcher_list;
    tick_list_t tick_list;

    cpu_timer_t()
        : watcher_list(memory::KernelCommonAllocatorV)
        , tick_list(memory::KernelCommonAllocatorV, 2, 16)
    {
    }
};

clock::clock_source *get_clock_source() { return cpu::current().get_clock_source(); }

clock::clock_event *get_clock_event() { return cpu::current().get_clock_event(); }

constexpr u64 tick_us = 1000;

void on_tick(u64 vector, u64 data)
{
    auto &cpu_timer = *(cpu_timer_t *)cpu::current().get_timer_queue();
    u64 us = get_clock_source()->current();

    {
        // add to tick list
        uctx::UninterruptibleContext icu;
        for (auto &ws : cpu_timer.watcher_list)
        {
            if (likely(ws.is_enable()))
            {
                cpu_timer.tick_list.insert(ws);
            }
        }
        cpu_timer.watcher_list.clean();
    }
    uctx::UninterruptibleController icc;
    icc.begin();
    auto it = cpu_timer.tick_list.begin();
    for (; it != cpu_timer.tick_list.end() && it->expires <= us + tick_us / 2;)
    {
        if (likely(it->is_enable()))
        {
            auto f = it->function;
            auto exp = it->expires;
            auto d = it->data;
            icc.end();
            f(exp, d);
            icc.begin();
        }
        it = cpu_timer.tick_list.remove(it);
    }
    icc.end();
}
lock::spinlock_t timer_spinlock;
void init()
{
    timer_spinlock.lock();
    clock::clock_source *pit_source;
    clock_source_array_t clock_sources(memory::KernelCommonAllocatorV);

    {
        uctx::UninterruptibleContext ctx;

        auto cpu_timer = memory::New<cpu_timer_t>(memory::KernelCommonAllocatorV);
        cpu::current().set_timer_queue(cpu_timer);

        pit_source = arch::device::PIT::make_clock();

        clock_sources.push_back(arch::APIC::make_clock());
        clock_sources.push_back(arch::TSC::make_clock());

        cpu::current().set_clock_event(clock_sources.front()->get_event());
        cpu::current().set_clock_source(clock_sources.back());
    }
    for (auto cs : clock_sources)
    {
        cs->calibrate(pit_source);
    }

    auto cev = cpu::current().get_clock_event();
    cev->resume();
    cev->wait_next_tick();
    get_clock_source()->reinit();

    if (cpu::current().is_bsp())
    {
        clock::init();
        clock::start_tick();
        irq::insert_soft_request_func(irq::soft_vector::timer, on_tick, 0);
    }

    timer_spinlock.unlock();
}

time::microsecond_t get_high_resolution_time() { return get_clock_source()->current(); }

void add_watcher(u64 expires_delta_time, watcher_func func, u64 user_data)
{
    auto &cpu_timer = *(cpu_timer_t *)cpu::current().get_timer_queue();

    cpu_timer.watcher_list.push_back(watcher_t(expires_delta_time + get_high_resolution_time(), func, user_data));
}

bool add_time_point_watcher(u64 expires_time_point, watcher_func func, u64 user_data)
{
    auto &cpu_timer = *(cpu_timer_t *)cpu::current().get_timer_queue();

    if (get_high_resolution_time() < expires_time_point)
    {
        cpu_timer.watcher_list.push_back(watcher_t(expires_time_point, func, user_data));
        return true;
    }
    return false;
}

///< don't remove timer in soft irq context
void remove_watcher(watcher_func func)
{
    auto &cpu_timer = *(cpu_timer_t *)cpu::current().get_timer_queue();

    for (auto it = cpu_timer.watcher_list.begin(); it != cpu_timer.watcher_list.end(); ++it)
    {
        if (it->function == func)
        {
            cpu_timer.watcher_list.remove(it);
            return;
        }
    }
    uctx::UninterruptibleContext icu;
    for (auto it = cpu_timer.tick_list.begin(); it != cpu_timer.tick_list.end();)
    {
        if (it->function <= func)
        {
            it->clear_enable();
            return;
        }
        else
        {
            ++it;
        }
    }
}

} // namespace timer
