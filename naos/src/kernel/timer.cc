#include "kernel/timer.hpp"

#include "freelibcxx/skip_list.hpp"
#include "freelibcxx/vector.hpp"
#include "kernel/arch/acpipm.hpp"
#include "kernel/arch/cpu.hpp"
#include "kernel/arch/hpet.hpp"
#include "kernel/arch/io_apic.hpp"
#include "kernel/arch/kvm_pvclock.hpp"
#include "kernel/arch/local_apic.hpp"
#include "kernel/arch/pit.hpp"
#include "kernel/arch/tsc.hpp"
#include "kernel/clock.hpp"
#include "kernel/cmdline.hpp"
#include "kernel/common.hpp"
#include "kernel/cpu.hpp"
#include "kernel/irq.hpp"
#include "kernel/lock.hpp"
#include "kernel/log.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/ucontext.hpp"

KLOG_MODULE(kernel);
namespace timer
{
namespace
{
struct watcher_lifetime
{
    watcher_id id = invalid_watcher_id;
    std::atomic_uint32_t references{1};
    std::atomic_bool completed{false};
    watcher_lifetime *active_next = nullptr;
};

struct watcher_t
{
    watcher_id id;
    timeclock::nanosecond_t expires_ns;
    timer_handler handler;
    watcher_lifetime *lifetime;

    watcher_t(watcher_id id, timeclock::nanosecond_t expires_ns, timer_handler handler, watcher_lifetime *lifetime)
        : id(id)
        , expires_ns(expires_ns)
        , handler(handler)
        , lifetime(lifetime)
    {
    }

    bool operator==(const watcher_t &other) const { return id == other.id; }
    bool operator<(const watcher_t &other) const
    {
        return expires_ns != other.expires_ns ? expires_ns < other.expires_ns : id < other.id;
    }
};

using watcher_list_t = freelibcxx::skip_list<watcher_t>;

struct cpu_timer_t
{
    watcher_list_t watchers;
    u64 deadline_generation = 0;

    cpu_timer_t()
        : watchers(memory::KernelCommonAllocatorV)
    {
    }
};

enum class clock_kind : u8
{
    none,
    pvclock,
    tsc,
    platform,
};

cpu_timer_t *timer_queues[arch::cpu::max_cpu_support]{};
watcher_lifetime *active_watchers = nullptr;
watcher_lifetime *active_callbacks[arch::cpu::max_cpu_support]{};
lock::spinlock_t timer_queue_lock;
lock::spinlock_t timer_spinlock;
std::atomic_uint64_t next_watcher_id{1};
irq::registration *tick_registration = nullptr;
timeclock::event_clock *global_platform_clock = nullptr;
clock_kind selected_clock_kind = clock_kind::none;
u64 selected_tsc_frequency_hz = 0;
std::atomic_uint64_t late_deadlines{0};
std::atomic_uint64_t ap_registration_failures{0};
std::atomic_uint64_t stale_interrupts{0};
std::atomic_uint64_t source_arm_failures{0};

template <typename Atomic> void increment_saturated(Atomic &counter) noexcept
{
    auto old = counter.load(std::memory_order_relaxed);
    while (old != static_cast<u64>(-1) &&
           !counter.compare_exchange_weak(old, old + 1, std::memory_order_relaxed, std::memory_order_relaxed))
    {
    }
}

void retain_watcher(watcher_lifetime *lifetime) noexcept
{
    lifetime->references.fetch_add(1, std::memory_order_relaxed);
}

void release_watcher(watcher_lifetime *lifetime) noexcept
{
    if (lifetime->references.fetch_sub(1, std::memory_order_acq_rel) == 1)
        memory::KernelCommonAllocatorV->Delete(lifetime);
}

void add_active_watcher_locked(watcher_lifetime *lifetime) noexcept
{
    lifetime->active_next = active_watchers;
    active_watchers = lifetime;
}

void remove_active_watcher_locked(watcher_lifetime *lifetime) noexcept
{
    watcher_lifetime **current = &active_watchers;
    while (*current != nullptr)
    {
        if (*current == lifetime)
        {
            *current = lifetime->active_next;
            lifetime->active_next = nullptr;
            return;
        }
        current = &(*current)->active_next;
    }
}

watcher_lifetime *find_active_watcher_locked(watcher_id id) noexcept
{
    for (auto *current = active_watchers; current != nullptr; current = current->active_next)
    {
        if (current->id == id)
            return current;
    }
    return nullptr;
}

bool is_current_callback(watcher_lifetime *lifetime) noexcept
{
    if (!cpu::has_init())
        return false;
    const u32 cpu_id = cpu::current().id();
    return cpu_id < arch::cpu::max_cpu_support && active_callbacks[cpu_id] == lifetime;
}

timeclock::event_clock *get_event_clock() noexcept
{
    if (!cpu::has_init())
        return nullptr;
    return cpu::current().get_event_clock();
}

timeclock::event_source *get_event_source() noexcept
{
    if (!cpu::has_init())
        return nullptr;
    return cpu::current().get_event_source();
}

cpu_timer_t *current_timer_queue() noexcept
{
    if (!cpu::has_init())
        return nullptr;
    const u32 id = cpu::current().id();
    if (id >= arch::cpu::max_cpu_support)
        return nullptr;
    return timer_queues[id];
}

void rearm_current_cpu_locked(cpu_timer_t &queue) noexcept
{
    auto *source = get_event_source();
    auto *clock = get_event_clock();
    if (source == nullptr || clock == nullptr)
        return;

    ++queue.deadline_generation;
    if (queue.deadline_generation == 0)
        ++queue.deadline_generation;

    if (queue.watchers.empty())
    {
        source->cancel(queue.deadline_generation);
        return;
    }

    const auto deadline_ns = queue.watchers.begin()->expires_ns;
    const auto now_ns = clock->now_ns();
    // A deadline can become due while waiting for timer_queue_lock.  Keep the
    // request structurally valid and let the soft timer path perform the
    // authoritative time-domain check.
    const timeclock::deadline_request request{now_ns < deadline_ns ? now_ns : deadline_ns, deadline_ns,
                                              queue.deadline_generation};
    if (source->arm(request) != timeclock::arm_result::armed)
        increment_saturated(source_arm_failures);
}

void on_tick(u64 vector) noexcept
{
    (void)vector;
    auto *queue = current_timer_queue();
    auto *clock = get_event_clock();
    if (queue == nullptr || clock == nullptr)
    {
        increment_saturated(stale_interrupts);
        return;
    }

    if (selected_clock_kind == clock_kind::pvclock &&
        static_cast<arch::kvm_pvclock::clock *>(clock)->take_retry_warning())
        KLOG_WARN("KVM pvclock seqlock remained unstable; using cached conversion parameters");

    for (;;)
    {
        timer_handler handler;
        watcher_lifetime *lifetime = nullptr;
        timeclock::nanosecond_t expires_ns = 0;
        const auto now_ns = clock->now_ns();
        {
            uctx::RawSpinLockUninterruptibleContext guard(timer_queue_lock);
            auto it = queue->watchers.begin();
            if (it == queue->watchers.end() || it->expires_ns > now_ns)
            {
                rearm_current_cpu_locked(*queue);
                break;
            }
            handler = it->handler;
            lifetime = it->lifetime;
            expires_ns = it->expires_ns;
            add_active_watcher_locked(lifetime);
            queue->watchers.remove(it);
            if (now_ns > expires_ns)
                increment_saturated(late_deadlines);
        }
        // Removing before invoking is what makes cancellation safe when a
        // callback races with a caller on another CPU.
        active_callbacks[cpu::current().id()] = lifetime;
        handler(expires_ns / timeclock::nanoseconds_per_microsecond);
        active_callbacks[cpu::current().id()] = nullptr;
        lifetime->completed.store(true, std::memory_order_release);
        {
            uctx::RawSpinLockUninterruptibleContext guard(timer_queue_lock);
            remove_active_watcher_locked(lifetime);
        }
        release_watcher(lifetime);
    }
}

timeclock::event_clock *make_platform_clock() noexcept
{
    const bool enable_hpet = cmdline::get_bool("hpet", false);
    const bool enable_acpipm = cmdline::get_bool("acpipm", true);
    const bool enable_pit = cmdline::get_bool("pit", true);

    arch::device::PIT::disable_all();
    if (enable_hpet)
    {
        auto *clock = arch::device::HPET::make_event_clock();
        if (clock != nullptr && clock->start_cpu())
            return clock;
    }
    if (enable_acpipm)
    {
        auto *clock = arch::device::ACPI::make_event_clock();
        if (clock != nullptr && clock->start_cpu())
            return clock;
    }
    if (enable_pit && arch::APIC::exist(arch::APIC::gsi_vector::pit))
    {
        auto *clock = arch::device::PIT::make_event_clock();
        if (clock != nullptr && clock->start_cpu())
            return clock;
    }
    return nullptr;
}

timeclock::event_clock *select_bsp_clock(arch::kvm_pvclock::policy kvm_policy) noexcept
{
    if (kvm_policy != arch::kvm_pvclock::policy::disabled)
    {
        const auto features = arch::kvm_pvclock::detect();
        KLOG_INFO("KVM pvclock probe clocksource2={} stable-bit={} available={}", features.clocksource2,
                  features.stable_bit, features.available);
        auto *pvclock = arch::kvm_pvclock::make_event_clock(kvm_policy);
        if (pvclock != nullptr && pvclock->start_cpu())
        {
            selected_clock_kind = clock_kind::pvclock;
            return pvclock;
        }
        if (kvm_policy == arch::kvm_pvclock::policy::required)
            KLOG_PANIC("kvmclock=on requested, but KVM CLOCKSOURCE2 or the first pvclock sample is unavailable");
        KLOG_WARN("KVM pvclock unavailable; falling back to a non-PV event clock");
    }
    else
    {
        KLOG_INFO("kvmclock=off: KVM pvclock probing and MSR registration disabled");
    }

    auto *tsc = arch::TSC::make_event_clock();
    if (tsc != nullptr && tsc->start_cpu())
    {
        selected_tsc_frequency_hz = static_cast<arch::TSC::clock *>(tsc)->frequency_hz();
        selected_clock_kind = clock_kind::tsc;
        return tsc;
    }

    global_platform_clock = make_platform_clock();
    if (global_platform_clock == nullptr)
        KLOG_PANIC("timer is not available: no KVM pvclock, TSC, HPET, ACPI PM, or PIT clock");

    if (tsc != nullptr)
    {
        auto *tsc_clock = static_cast<arch::TSC::clock *>(tsc);
        if (tsc_clock->calibrate(*global_platform_clock) && tsc_clock->start_cpu())
        {
            selected_tsc_frequency_hz = tsc_clock->frequency_hz();
            selected_clock_kind = clock_kind::tsc;
            return tsc_clock;
        }
    }
    selected_clock_kind = clock_kind::platform;
    return global_platform_clock;
}

timeclock::event_clock *select_ap_clock() noexcept
{
    switch (selected_clock_kind)
    {
        case clock_kind::pvclock: {
            auto *clock = arch::kvm_pvclock::make_event_clock(arch::kvm_pvclock::policy::required);
            if (clock != nullptr && clock->start_cpu())
                return clock;
            increment_saturated(ap_registration_failures);
            KLOG_PANIC("AP {} failed to register its KVM pvclock page", cpu::current().id());
        }
        case clock_kind::tsc: {
            auto *clock = arch::TSC::make_event_clock(selected_tsc_frequency_hz);
            if (clock != nullptr && clock->start_cpu())
                return clock;
            increment_saturated(ap_registration_failures);
            KLOG_PANIC("AP {} failed to start the selected TSC event clock", cpu::current().id());
        }
        case clock_kind::platform:
            return global_platform_clock;
        case clock_kind::none:
            break;
    }
    return nullptr;
}
} // namespace

void init()
{
    uctx::RawSpinLockUninterruptibleContext timer_guard(timer_spinlock);
    auto *queue = memory::New<cpu_timer_t>(memory::KernelCommonAllocatorV);
    if (queue == nullptr)
        KLOG_PANIC("unable to allocate CPU timer queue");
    cpu::current().set_timer_queue(queue);
    timer_queues[cpu::current().id()] = queue;

    timeclock::event_clock *clock = nullptr;
    if (cpu::current().is_bsp())
    {
        auto selection = arch::kvm_pvclock::policy::auto_select;
        if (auto configured = cmdline::get("kvmclock"); configured.has_value())
            selection = arch::kvm_pvclock::parse_policy(configured.value().data());
        clock = select_bsp_clock(selection);
    }
    else
    {
        clock = select_ap_clock();
    }
    if (clock == nullptr)
        KLOG_PANIC("CPU {} has no event clock", cpu::current().id());
    cpu::current().set_event_clock(clock);

    auto *source = arch::APIC::make_event_source();
    if (source == nullptr || !source->calibrate(*clock) || !source->start_cpu())
        KLOG_PANIC("CPU {} failed to start Local APIC event source", cpu::current().id());
    cpu::current().set_event_source(source);

    if (cpu::current().is_bsp())
    {
        KLOG_INFO("event-clock={} event-source={} cross-cpu-monotonic={}", clock->name(), source->name(),
                  clock->cross_cpu_monotonic());
        if (selected_clock_kind == clock_kind::pvclock)
        {
            const auto health = static_cast<arch::kvm_pvclock::clock *>(clock)->health();
            KLOG_INFO("pvclock health stable-samples={} seqlock-retries={} cached-fallbacks={} backward-clamps={} "
                      "invalid-samples={} paused-samples={}",
                      health.stable_samples, health.seqlock_retries, health.cached_fallbacks, health.backward_clamps,
                      health.invalid_samples, health.paused_samples);
        }
        const auto timer_health = diagnostics();
        KLOG_INFO(
            "timer health late-deadlines={} AP-registration-failures={} stale-interrupts={} source-arm-failures={}",
            timer_health.late_deadlines, timer_health.ap_registration_failures, timer_health.stale_interrupts,
            timer_health.source_arm_failures);
        timeclock::init();
        timeclock::start_tick();
        tick_registration = memory::New<irq::registration>(memory::KernelCommonAllocatorV);
        *tick_registration = irq::register_soft_handler(irq::soft_vector::timer, irq::soft_handler::bind<&on_tick>());
    }
    else
    {
        KLOG_INFO("AP {} event-clock={} event-source={} registered", cpu::current().id(), clock->name(),
                  source->name());
    }
}

timeclock::nanosecond_t get_high_resolution_time_ns() noexcept
{
    auto *clock = get_event_clock();
    return clock == nullptr ? 0 : clock->now_ns();
}

timeclock::microsecond_t get_high_resolution_time()
{
    return get_high_resolution_time_ns() / timeclock::nanoseconds_per_microsecond;
}

void busywait(timeclock::microsecond_t duration)
{
    const auto start = get_high_resolution_time();
    timeclock::microsecond_t deadline = 0;
    if (!timeclock::try_add_microseconds(start, duration, deadline))
        return;
    volatile int value = 0;
    while (get_high_resolution_time() < deadline)
    {
        for (int i = 0; i < 100; i++)
            value = value + static_cast<int>(duration) - i;
    }
}

watcher_id schedule_after(timeclock::microsecond_t duration, timer_handler handler)
{
    if (handler == nullptr)
        return invalid_watcher_id;
    auto *queue = current_timer_queue();
    if (queue == nullptr)
        return invalid_watcher_id;

    timeclock::nanosecond_t duration_ns = 0;
    if (!timeclock::try_microseconds_to_nanoseconds(duration, duration_ns))
        return invalid_watcher_id;
    const auto now_ns = get_high_resolution_time_ns();
    timeclock::nanosecond_t expires_ns = 0;
    if (!timeclock::try_add_nanoseconds(now_ns, duration_ns, expires_ns))
        return invalid_watcher_id;

    auto *lifetime = memory::KernelCommonAllocatorV->New<watcher_lifetime>();
    if (lifetime == nullptr)
        return invalid_watcher_id;

    uctx::RawSpinLockUninterruptibleContext guard(timer_queue_lock);
    const watcher_id id = next_watcher_id.fetch_add(1, std::memory_order_relaxed);
    lifetime->id = id;
    const bool was_earliest = queue->watchers.empty() || expires_ns < queue->watchers.begin()->expires_ns;
    queue->watchers.insert(id, expires_ns, handler, lifetime);
    if (was_earliest)
        rearm_current_cpu_locked(*queue);
    return id;
}

watcher_id schedule_at(timeclock::microsecond_t expires_time_point, timer_handler handler)
{
    if (handler == nullptr)
        return invalid_watcher_id;
    auto *queue = current_timer_queue();
    if (queue == nullptr)
        return invalid_watcher_id;
    timeclock::nanosecond_t expires_ns = 0;
    if (!timeclock::try_microseconds_to_nanoseconds(expires_time_point, expires_ns))
        return invalid_watcher_id;
    if (get_high_resolution_time_ns() >= expires_ns)
        return invalid_watcher_id;

    auto *lifetime = memory::KernelCommonAllocatorV->New<watcher_lifetime>();
    if (lifetime == nullptr)
        return invalid_watcher_id;

    uctx::RawSpinLockUninterruptibleContext guard(timer_queue_lock);
    const watcher_id id = next_watcher_id.fetch_add(1, std::memory_order_relaxed);
    lifetime->id = id;
    const bool was_earliest = queue->watchers.empty() || expires_ns < queue->watchers.begin()->expires_ns;
    queue->watchers.insert(id, expires_ns, handler, lifetime);
    if (was_earliest)
        rearm_current_cpu_locked(*queue);
    return id;
}

bool cancel(watcher_id id)
{
    if (id == invalid_watcher_id)
        return false;

    watcher_lifetime *removed = nullptr;
    watcher_lifetime *active = nullptr;
    {
        uctx::RawSpinLockUninterruptibleContext guard(timer_queue_lock);
        const u64 count = cpu::count();
        for (u64 cpu_id = 0; cpu_id < count && cpu_id < arch::cpu::max_cpu_support; cpu_id++)
        {
            auto *queue = timer_queues[cpu_id];
            if (queue == nullptr)
                continue;
            for (auto it = queue->watchers.begin(); it != queue->watchers.end(); ++it)
            {
                if (it->id == id)
                {
                    const bool was_earliest = it == queue->watchers.begin();
                    removed = it->lifetime;
                    queue->watchers.remove(it);
                    // A remote source will ignore the resulting stale interrupt;
                    // the owning CPU will rearm at its next soft timer pass.
                    if (was_earliest && cpu_id == cpu::current().id())
                        rearm_current_cpu_locked(*queue);
                    break;
                }
            }
            if (removed != nullptr)
                break;
        }
        if (removed == nullptr)
        {
            active = find_active_watcher_locked(id);
            if (active != nullptr && !is_current_callback(active))
                retain_watcher(active);
        }
    }

    if (removed != nullptr)
    {
        removed->completed.store(true, std::memory_order_release);
        release_watcher(removed);
        return true;
    }
    if (active == nullptr || is_current_callback(active))
        return false;

    while (!active->completed.load(std::memory_order_acquire))
        cpu_pause();
    release_watcher(active);
    return false;
}

diagnostics_snapshot diagnostics() noexcept
{
    return {late_deadlines.load(std::memory_order_relaxed), ap_registration_failures.load(std::memory_order_relaxed),
            stale_interrupts.load(std::memory_order_relaxed), source_arm_failures.load(std::memory_order_relaxed)};
}

} // namespace timer
