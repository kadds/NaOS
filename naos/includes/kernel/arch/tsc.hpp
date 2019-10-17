#pragma once
#include "common.hpp"
#include "kernel/clock/clock_event.hpp"
#include "kernel/clock/clock_source.hpp"

namespace arch::TSC
{

class clock_event : public ::clock::clock_event
{
  private:
  public:
    clock_event()
        : ::clock::clock_event(0){};

    void init(u64 HZ) override{};
    void destroy() override{};

    void suspend() override {}
    void resume() override {}

    void wait_next_tick() override{};
    bool is_valid() override { return true; }
};

class clock_source : public ::clock::clock_source
{

  private:
    u64 tsc_tick_per_microsecond;
    u64 fill_tsc;

  public:
    void init() override;
    void destroy() override;
    void calibrate(::clock::clock_source *cs) override;
    u64 calibrate_tsc(::clock::clock_source *cs);
    u64 current() override;
};

clock_source *make_clock();

} // namespace arch::TSC
