#pragma once
#include "freelibcxx/delegate.hpp"
#include "kernel/common.hpp"
#include "kernel/types.hpp"
namespace timer
{

using timer_handler = freelibcxx::delegate<void(timeclock::microsecond_t) noexcept>;
using watcher_id = u64;
constexpr watcher_id invalid_watcher_id = 0;

struct diagnostics_snapshot
{
    u64 late_deadlines = 0;
    u64 ap_registration_failures = 0;
    u64 stale_interrupts = 0;
    u64 source_arm_failures = 0;
};

void init();

///
/// \brief get current time
///
/// \note the time of each CPU is not synchronized
timeclock::microsecond_t get_high_resolution_time();
timeclock::nanosecond_t get_high_resolution_time_ns() noexcept;

diagnostics_snapshot diagnostics() noexcept;

void busywait(timeclock::microsecond_t duration);

[[nodiscard]] watcher_id schedule_after(timeclock::microsecond_t duration, timer_handler handler);
[[nodiscard]] watcher_id schedule_at(timeclock::microsecond_t deadline, timer_handler handler);
bool cancel(watcher_id id);

} // namespace timer
