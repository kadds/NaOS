#pragma once
#include "common.hpp"
#include "kernel/clock/clock_event.hpp"
#include "kernel/clock/clock_source.hpp"
#include "kernel/irq.hpp"
namespace arch::idt
{
struct regs_t;
} // namespace arch::idt

namespace arch::device::PIT
{
class clock_source;
class clock_event : public ::clock::clock_event
{
    friend class clock_source;
    friend irq::request_result on_event(const arch::idt::regs_t *regs, u64 extra_data, u64 user_data);

  private:
    u64 hz;
    u16 pc;
    bool is_suspend;
    volatile u64 tick_jiff;

  public:
    clock_event()
        : ::clock::clock_event(50){};

    void init(u64 hz) override;
    void destroy() override;
    void suspend() override;
    void resume() override;
    void wait_next_tick() override;

    bool is_valid() override { return true; }
};

class clock_source : public ::clock::clock_source
{

  public:
    void init() override;
    void destroy() override;
    void calibrate(clock::clock_source *cs) override;
    u64 current() override;
};
clock_source *make_clock();
} // namespace arch::device::PIT
