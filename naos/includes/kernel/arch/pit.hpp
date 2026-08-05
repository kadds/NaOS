#pragma once
#include "../clock/clock_event.hpp"
#include "../clock/clock_source.hpp"
#include "../types.hpp"
#include "kernel/common.hpp"
#include "kernel/irq.hpp"
#include "kernel/lock.hpp"
#include <atomic>

namespace arch::device::PIT
{
class clock_source;
class clock_event : public ::timeclock::clock_event
{
    friend class clock_source;

  private:
    u64 hz_ = 0;
    u16 divisor_ = 0;
    std::atomic_uint16_t init_val_ = 0;

    std::atomic_bool is_suspend_ = false;
    std::atomic_uint64_t jiff_ = 0;
    std::atomic_uint64_t last_tick_ = 0;
    u64 update_tsc_ = 0;
    irq::registration irq_registration_;

    irq::request_result on_interrupt(const irq::interrupt_info *, u64) noexcept;

  public:
    clock_event() {}

    void init(u64 hz) override;
    void destroy() override;
    void suspend() override;
    void resume() override;

    bool is_valid() override { return true; }
};

class clock_source : public ::timeclock::clock_source
{
  public:
    clock_source()
        : ::timeclock::clock_source("pit")
    {
    }
    void init() override;
    void destroy() override;
    void calibrate(::timeclock::clock_source *cs) override;
    u64 current() override;
    u64 jiff() override;
};
clock_source *make_clock();
void disable_all();
} // namespace arch::device::PIT
