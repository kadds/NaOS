#pragma once
#include "../clock/clock_event.hpp"
#include "../clock/clock_source.hpp"
#include "../types.hpp"
#include "common.hpp"
#include "kernel/lock.hpp"
#include <atomic>
namespace arch::device::ACPI
{
typedef u32 (*read_register_func)(u64 address);

class clock_source;
class clock_event : public ::timeclock::clock_event
{
    friend class clock_source;
    friend irq::request_result on_acpipm_event(u64 user_data);

  private:
    std::atomic_uint32_t init_val_ = 0;
    std::atomic_uint64_t last_tick_ = 0;
    std::atomic_uint64_t jiff_ = 0;
    std::atomic_bool is_suspend_ = false;

    u64 address_ = 0;
    read_register_func read_register_ = nullptr;

    bool bit32_mode_ = false;

    u32 periods_ = 0;

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
        : ::timeclock::clock_source("acpipm")
    {
    }
    void init() override;
    void destroy() override;
    void calibrate(::timeclock::clock_source *cs) override;
    u64 current() override;
    u64 jiff() override;
};
clock_source *make_clock();
} // namespace arch::device::ACPI