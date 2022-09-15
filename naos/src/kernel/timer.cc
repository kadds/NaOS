#include "kernel/timer.hpp"
#include "common.hpp"
#include "freelibcxx/linked_list.hpp"
#include "freelibcxx/skip_list.hpp"
#include "freelibcxx/vector.hpp"
#include "kernel/arch/acpipm.hpp"
#include "kernel/arch/hpet.hpp"
#include "kernel/arch/io_apic.hpp"
#include "kernel/arch/local_apic.hpp"
#include "kernel/arch/pit.hpp"
#include "kernel/arch/rtc.hpp"
#include "kernel/arch/tsc.hpp"
#include "kernel/clock.hpp"
#include "kernel/clock/clock_source.hpp"
#include "kernel/cmdline.hpp"
#include "kernel/cpu.hpp"
#include "kernel/irq.hpp"
#include "kernel/lock.hpp"
#include "kernel/mm/list_node_cache.hpp"
#include "kernel/trace.hpp"
#include "kernel/ucontext.hpp"

namespace timer
{

using clock_source_array_t = freelibcxx::vector<timeclock::clock_source *>;

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

using watcher_list_t = freelibcxx::linked_list<watcher_t>;
using tick_list_t = freelibcxx::skip_list<watcher_t>;

struct cpu_timer_t
{
    watcher_list_t watcher_list;
    tick_list_t tick_list;

    cpu_timer_t()
        : watcher_list(memory::KernelCommonAllocatorV)
        , tick_list(memory::KernelCommonAllocatorV)
    {
    }
};

timeclock::clock_source *get_clock_source()
{
    if (likely(cpu::has_init()))
    {
        return cpu::current().get_clock_source();
    }
    return nullptr;
}

timeclock::clock_event *get_clock_event()
{
    if (likely(cpu::has_init()))
    {
        return cpu::current().get_clock_event();
    }
    return nullptr;
}

constexpr u64 tick_us = 1000;

void on_tick(u64 vector, u64 data)
{
    auto &cpu_timer = *reinterpret_cast<cpu_timer_t *>(cpu::current().get_timer_queue());
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
        cpu_timer.watcher_list.clear();
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

bool check_source(timeclock::clock_source *cs)
{
    if (cs->get_event())
    {
        cs->get_event()->resume();
    }
    u64 last = cs->current();
    for (int i = 0; i < 1'000'000; i++)
    {
        u64 v = cs->current();
        if (v < last || v > last + 10'000)
        {
            trace::warning("check clock source ", cs->name(), " current ", v, " last ", last, " jiff ", cs->jiff(),
                           " at ", i);
            if (cs->get_event())
            {
                cs->get_event()->suspend();
            }
            return false;
        }
        last = v;
    }
    if (cs->get_event())
    {
        cs->get_event()->suspend();
    }
    return true;
}

lock::spinlock_t timer_spinlock;
timeclock::clock_source *global_source = nullptr;
void init()
{
    timer_spinlock.lock();
    clock_source_array_t clock_sources(memory::KernelCommonAllocatorV);
    {
        bool enable_pit = cmdline::get_bool("pit", true);
        bool enable_hpet = cmdline::get_bool("hpet", false);
        bool enable_acpipm = cmdline::get_bool("acpipm", true);

        auto cpu_timer = memory::New<cpu_timer_t>(memory::KernelCommonAllocatorV);
        cpu::current().set_timer_queue(cpu_timer);
        arch::device::PIT::disable_all();

        if (enable_hpet && global_source == nullptr)
        {
            {
                uctx::UninterruptibleContext ctx;
                global_source = arch::device::HPET::make_clock();
            }
            if (global_source && !check_source(global_source))
            {
                trace::warning("hpet timer is not stable");
                global_source = nullptr;
            }
        }

        if (global_source == nullptr && enable_acpipm)
        {
            {
                uctx::UninterruptibleContext ctx;
                global_source = arch::device::ACPI::make_clock();
            }
            if (global_source && !check_source(global_source))
            {
                trace::warning("acpi pm timer is not stable");
                global_source = nullptr;
            }
        }

        if (global_source == nullptr && enable_pit && arch::APIC::exist(arch::APIC::gsi_vector::pit))
        {
            {
                uctx::UninterruptibleContext ctx;
                global_source = arch::device::PIT::make_clock();
            }
            if (global_source && !check_source(global_source))
            {
                trace::warning("pit timer is not stable");
                global_source = nullptr;
            }
        }

        if (global_source == nullptr)
        {
            trace::panic("timer is not available");
        }
        if (cpu::current().is_bsp())
        {
            trace::info("Use ", global_source->name(), " as timer");
        }
        timeclock::clock_source *tsc, *local_apic;
        {
            uctx::UninterruptibleContext ctx;
            tsc = arch::TSC::make_clock();
            local_apic = arch::APIC::make_clock();
        }

        if (tsc != nullptr)
        {
            clock_sources.push_back(tsc);
            cpu::current().set_clock_source(tsc);
        }
        else
        {
            cpu::current().set_clock_source(local_apic);
        }

        if (local_apic != nullptr)
        {
            clock_sources.push_back(local_apic);
        }

        // default tsc
        cpu::current().set_clock_event(local_apic->get_event());
    }

    for (auto cs : clock_sources)
    {
        cs->calibrate(global_source);
    }
    // check_source(clock_sources.back());

    auto ev = cpu::current().get_clock_event();
    ev->resume();
    get_clock_source()->reinit();

    if (cpu::current().is_bsp())
    {
        timeclock::init();
        timeclock::start_tick();
        irq::register_soft_request_func(irq::soft_vector::timer, on_tick, 0);
    }

    timer_spinlock.unlock();
}

timeclock::microsecond_t get_high_resolution_time()
{
    auto source = get_clock_source();
    if (likely(source != nullptr))
    {
        return source->current();
    }
    return 0;
}

void busywait(timeclock::microsecond_t duration)
{
    timeclock::microsecond_t t = get_high_resolution_time() + duration;
    volatile int v = 0;
    while (t < get_high_resolution_time())
    {
        for (int i = 0; i < 100; i++)
        {
            v = v + duration - i;
        }
    }
}

void add_watcher(u64 expires_delta_time, watcher_func func, u64 user_data)
{
    auto &cpu_timer = *reinterpret_cast<cpu_timer_t *>(cpu::current().get_timer_queue());
    cpu_timer.watcher_list.push_back(watcher_t(expires_delta_time + get_high_resolution_time(), func, user_data));
}

bool add_time_point_watcher(u64 expires_time_point, watcher_func func, u64 user_data)
{
    auto &cpu_timer = *reinterpret_cast<cpu_timer_t *>(cpu::current().get_timer_queue());

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
    auto &cpu_timer = *reinterpret_cast<cpu_timer_t *>(cpu::current().get_timer_queue());

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
        if (it->function == func)
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
