#pragma once
#include "../clock/clock_event.hpp"
#include "../clock/clock_source.hpp"
#include "../types.hpp"
#include "common.hpp"
#include "kernel/lock.hpp"
#include <atomic>

namespace arch::device::HPET
{
class clock_source;
class clock_event : public ::timeclock::clock_event
{
    friend class clock_source;
    friend irq::request_result on_hpet_event(const irq::interrupt_info *, u64 extra_data, u64 user_data);

  public:
    volatile u64 *base_ = nullptr;
    u64 freq_ = 0;
    u64 hz_ = 0;

    u64 counter_ = 0;
    u64 init_val_ = 0;

    std::atomic_uint64_t target_counter_ = 0;
    std::atomic_uint64_t jiff_ = 0;
    std::atomic_uint64_t last_tick_ = 0;
    std::atomic_bool is_suspend = false;

    bool mode64_ = false;
    bool mode_periodic_ = false;

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
        : ::timeclock::clock_source("hpet")
    {
    }
    void init() override;
    void destroy() override;
    void calibrate(::timeclock::clock_source *cs) override;
    u64 current() override;
    u64 jiff() override;
};
clock_source *make_clock();
} // namespace arch::device::HPET