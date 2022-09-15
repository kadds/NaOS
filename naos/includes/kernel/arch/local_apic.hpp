#pragma once
#include "../clock.hpp"
#include "../clock/clock_event.hpp"
#include "../clock/clock_source.hpp"
#include "../types.hpp"
#include "common.hpp"
#include <atomic>

namespace arch::APIC
{
namespace lvt_index
{
enum lvt_index : u8
{
    cmci,
    timer,
    thermal,
    performance,
    lint0,
    lint1,
    error,
};
}

void local_init();
void local_software_enable();
void local_software_disable();
void local_enable(u8 vector);
void local_irq_setup(u8 index, u8 vector, u8 flags);
void local_disable(u8 vector);
void local_EOI(u8 index);
void local_post_init_IPI();
void local_post_start_up(u64 addr);
void local_post_IPI_all(u64 intr);
void local_post_IPI_all_notself(u64 intr);
void local_post_IPI_self(u64 intr);
void local_post_IPI_mask(u64 intr, u64 mask0);

u64 local_ID();

class clock_source;

class clock_event : public ::timeclock::clock_event
{
  private:
    friend irq::request_result on_apic_event(const irq::interrupt_info *, u64 extra_data, u64 user_data);
    friend class clock_source;
    std::atomic_bool is_suspend_ = false;
    std::atomic_uint64_t jiff_ = 0;
    u32 counter_ = 0;
    static constexpr u32 divide_ = 7;
    u64 bus_frequency_ = 0;
    u64 id_ = 0;
    std::atomic_uint64_t last_tick_ = 0;

    u64 hz_ = 0;
    bool builtin_frequency_ = false;

  public:
    clock_event() {}
    void init(u64 HZ) override;
    void destroy() override;

    void suspend() override;
    void resume() override;
    bool is_valid() override { return true; }
};

class clock_source : public ::timeclock::clock_source
{
  private:
    friend class clock_event;

  public:
    clock_source()
        : ::timeclock::clock_source("local_apic")
    {
    }
    void init() override;
    void destroy() override;
    u64 current() override;
    u64 count();
    u64 jiff() override;

    void calibrate(::timeclock::clock_source *cs) override;

    u64 calibrate_apic(::timeclock::clock_source *cs);
    u64 calibrate_counter(::timeclock::clock_source *cs);
};

clock_source *make_clock();
} // namespace arch::APIC