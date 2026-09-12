#include "kernel/timer.hpp"
#include "freelibcxx/linked_list.hpp"
#include "freelibcxx/skip_list.hpp"
#include "freelibcxx/vector.hpp"
#include "kernel/arch/acpipm.hpp"
#include "kernel/arch/cpu.hpp"
#include "kernel/arch/hpet.hpp"
#include "kernel/arch/io_apic.hpp"
#include "kernel/arch/local_apic.hpp"
#include "kernel/arch/pit.hpp"
#include "kernel/arch/rtc.hpp"
#include "kernel/arch/tsc.hpp"
#include "kernel/clock.hpp"
#include "kernel/clock/clock_source.hpp"
#include "kernel/cmdline.hpp"
#include "kernel/common.hpp"
#include "kernel/cpu.hpp"
#include "kernel/irq.hpp"
#include "kernel/lock.hpp"
#include "kernel/log.hpp"
#include "kernel/mm/list_node_cache.hpp"
#include "kernel/ucontext.hpp"

KLOG_MODULE(kernel);
namespace timer
{

using clock_source_array_t = freelibcxx::vector<timeclock::clock_source *>;

struct watcher_t
{
    watcher_id id;
    /// target time microsecond
    u64 expires;
    timer_handler handler;

    watcher_t(watcher_id id, u64 expires, timer_handler handler)
        : id(id)
        , expires(expires)
        , handler(handler)
    {
    }

    bool operator==(const watcher_t &w) const { return id == w.id; }
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

cpu_timer_t *timer_queues[arch::cpu::max_cpu_support]{};
lock::spinlock_t timer_queue_lock;

cpu_timer_t *current_timer_queue()
{
    if (!cpu::has_init())
        return nullptr;
    const auto id = cpu::current().id();
    if (id >= arch::cpu::max_cpu_support)
        return nullptr;
    return timer_queues[id];
}

void on_tick(u64 vector) noexcept
{
    (void)vector;
    auto *cpu_timer = current_timer_queue();
    auto *source = get_clock_source();
    if (cpu_timer == nullptr || source == nullptr)
        return;
    const u64 us = source->current();

    {
        // add to tick list
        uctx::RawSpinLockUninterruptibleContext guard(timer_queue_lock);
        for (auto &ws : cpu_timer->watcher_list)
        {
            cpu_timer->tick_list.insert(ws);
        }
        cpu_timer->watcher_list.clear();
    }

    for (;;)
    {
        timer_handler handler;
        u64 expires = 0;
        {
            uctx::RawSpinLockUninterruptibleContext guard(timer_queue_lock);
            auto it = cpu_timer->tick_list.begin();
            if (it == cpu_timer->tick_list.end() || it->expires > us)
                break;

            handler = it->handler;
            expires = it->expires;
            // Remove before invoking the callback so cancellation on another
            // CPU cannot invalidate the iterator held by this loop.
            cpu_timer->tick_list.remove(it);
        }
        handler(expires);
    }
}

bool check_source(timeclock::clock_source *cs)
{
    if (cs->get_event())
    {
        cs->get_event()->resume();
    }
    u64 last = cs->current();
    for (u64 i = 0; i < source_validation_samples; i++)
    {
        u64 v = cs->current();
        if (v < last || v > last + 10'000)
        {
            KLOG_WARN("check clock source {} current {} last {} jiff {} at {}", cs->name(), v, last, cs->jiff(), i);
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
std::atomic_uint64_t next_watcher_id{1};
irq::registration *tick_registration;
void init()
{
    timer_spinlock.lock();
    clock_source_array_t clock_sources(memory::KernelCommonAllocatorV);
    {
        bool enable_pit = cmdline::get_bool("pit", true);
        bool enable_hpet = cmdline::get_bool("hpet", false);
        bool enable_acpipm = cmdline::get_bool("acpipm", true);

        auto cpu_timer = memory::New<cpu_timer_t>(memory::KernelCommonAllocatorV);
        if (cpu_timer == nullptr)
            KLOG_PANIC("unable to allocate CPU timer queue");
        cpu::current().set_timer_queue(cpu_timer);
        timer_queues[cpu::current().id()] = cpu_timer;
        arch::device::PIT::disable_all();

        if (enable_hpet && global_source == nullptr)
        {
            {
                uctx::UninterruptibleContext ctx;
                global_source = arch::device::HPET::make_clock();
            }
            if (global_source && !check_source(global_source))
            {
                KLOG_WARN("hpet timer is not stable");
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
                KLOG_WARN("acpi pm timer is not stable");
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
                KLOG_WARN("pit timer is not stable");
                global_source = nullptr;
            }
        }

        if (global_source == nullptr)
        {
            KLOG_PANIC("timer is not available");
        }
        if (cpu::current().is_bsp())
        {
            KLOG_INFO("Use {} as timer", global_source->name());
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
        tick_registration = memory::New<irq::registration>(memory::KernelCommonAllocatorV);
        *tick_registration = irq::register_soft_handler(irq::soft_vector::timer, irq::soft_handler::bind<&on_tick>());
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
    const auto start = get_high_resolution_time();
    timeclock::microsecond_t deadline = 0;
    if (!timeclock::try_add_microseconds(start, duration, deadline))
        return;
    volatile int v = 0;
    while (get_high_resolution_time() < deadline)
    {
        for (int i = 0; i < 100; i++)
        {
            v = v + duration - i;
        }
    }
}

watcher_id schedule_after(timeclock::microsecond_t duration, timer_handler handler)
{
    if (handler == nullptr)
        return invalid_watcher_id;
    auto *cpu_timer = current_timer_queue();
    if (cpu_timer == nullptr)
        return invalid_watcher_id;
    const auto now = get_high_resolution_time();
    timeclock::microsecond_t expires = 0;
    if (!timeclock::try_add_microseconds(now, duration, expires))
        return invalid_watcher_id;

    uctx::RawSpinLockUninterruptibleContext guard(timer_queue_lock);
    const auto id = next_watcher_id.fetch_add(1);
    cpu_timer->watcher_list.push_back(watcher_t(id, expires, handler));
    return id;
}

watcher_id schedule_at(timeclock::microsecond_t expires_time_point, timer_handler handler)
{
    if (handler == nullptr)
        return invalid_watcher_id;
    auto *cpu_timer = current_timer_queue();
    if (cpu_timer == nullptr)
        return invalid_watcher_id;

    if (get_high_resolution_time() < expires_time_point)
    {
        uctx::RawSpinLockUninterruptibleContext guard(timer_queue_lock);
        const auto id = next_watcher_id.fetch_add(1);
        cpu_timer->watcher_list.push_back(watcher_t(id, expires_time_point, handler));
        return id;
    }
    return invalid_watcher_id;
}

bool cancel(watcher_id id)
{
    if (id == invalid_watcher_id)
        return false;
    uctx::RawSpinLockUninterruptibleContext guard(timer_queue_lock);
    const auto count = cpu::count();
    for (u64 cpu_id = 0; cpu_id < count && cpu_id < arch::cpu::max_cpu_support; cpu_id++)
    {
        auto *cpu_timer = timer_queues[cpu_id];
        if (cpu_timer == nullptr)
            continue;

        for (auto it = cpu_timer->watcher_list.begin(); it != cpu_timer->watcher_list.end(); ++it)
        {
            if (it->id == id)
            {
                cpu_timer->watcher_list.remove(it);
                return true;
            }
        }

        for (auto it = cpu_timer->tick_list.begin(); it != cpu_timer->tick_list.end(); ++it)
        {
            if (it->id == id)
            {
                cpu_timer->tick_list.remove(it);
                return true;
            }
        }
    }
    return false;
}

} // namespace timer
