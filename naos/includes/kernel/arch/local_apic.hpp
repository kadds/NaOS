#pragma once

#include "../time/event_source.hpp"
#include "kernel/common.hpp"
#include "kernel/irq.hpp"
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

class event_source final : public ::timeclock::event_source
{
  public:
    event_source() noexcept = default;
    ~event_source() override { stop_cpu(); }

    bool calibrate(::timeclock::event_clock &reference) noexcept override;
    bool start_cpu() noexcept override;
    void stop_cpu() noexcept override;
    ::timeclock::arm_result arm(const ::timeclock::deadline_request &request) noexcept override;
    void cancel(u64 generation) noexcept override;
    bool is_armed() const noexcept override { return armed_.load(std::memory_order_acquire); }
    u64 armed_generation() const noexcept override { return armed_generation_.load(std::memory_order_acquire); }
    const char *name() const noexcept override { return "local-apic"; }

    u64 bus_frequency_hz() const noexcept { return bus_frequency_; }

  private:
    static constexpr u32 divide_ = 0;
    static constexpr u32 maximum_counter = 0xFFFF'FFFF;

    irq::request_result on_interrupt(const irq::interrupt_info *, u64) noexcept;
    void program_counter(u32 ticks) noexcept;
    void refresh_frequency() noexcept;
    bool calibrate_frequency(::timeclock::event_clock &clock) noexcept;

    u64 bus_frequency_ = 0;
    u32 cpu_id_ = 0;
    std::atomic_uint64_t generation_{0};
    std::atomic_uint64_t armed_generation_{0};
    std::atomic_bool armed_{false};
    std::atomic_bool started_{false};
    irq::registration irq_registration_;
};

event_source *make_event_source() noexcept;

} // namespace arch::APIC
