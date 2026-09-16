#pragma once

#include "kernel/time.hpp"

namespace timeclock
{

/// A monotonic timeline.  Implementations are per-CPU and their read path is
/// suitable for interrupt context: it must not allocate, lock, or probe a VM.
class event_clock
{
  public:
    virtual ~event_clock() = default;

    virtual bool start_cpu() noexcept = 0;
    virtual void stop_cpu() noexcept = 0;
    virtual nanosecond_t now_ns() noexcept = 0;
    virtual const char *name() const noexcept = 0;
    virtual bool cross_cpu_monotonic() const noexcept = 0;
};

} // namespace timeclock
