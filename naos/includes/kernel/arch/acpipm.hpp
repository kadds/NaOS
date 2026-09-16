#pragma once

#include "../time/event_clock.hpp"
#include "../types.hpp"
#include "kernel/common.hpp"

namespace arch::device::ACPI
{

class clock final : public ::timeclock::event_clock
{
  public:
    clock() noexcept = default;
    ~clock() override = default;

    bool start_cpu() noexcept override;
    void stop_cpu() noexcept override {}
    timeclock::nanosecond_t now_ns() noexcept override;
    const char *name() const noexcept override { return "acpipm"; }
    bool cross_cpu_monotonic() const noexcept override { return false; }

  private:
    u64 address_ = 0;
    u32 (*read_register_)(u64) = nullptr;
    u32 initial_count_ = 0;
    bool bit32_mode_ = false;
    bool started_ = false;
};

clock *make_event_clock() noexcept;

} // namespace arch::device::ACPI
