#pragma once
#include "arch/idt.hpp"
#include "kernel/common.hpp"
#include "types.hpp"
namespace irq
{
namespace hard_vector
{
enum hard_vector
{
    local_apic_timer = 128,
    // sent function to excute on other cpu
    IPI_call = 250,
    // send task to other cpu
    IPI_reschedule = 251,
    IPI_tlb = 252,
};
} // namespace hard_vector

namespace soft_vector
{
enum soft_vector
{
    high_task = 0,
    timer = 1,
    net_send = 2,
    net_rec = 3,
    block = 4,
    task = 5,
    sched = 6,
    COUNT,
};
} // namespace soft_vector

class registration
{
  public:
    registration() noexcept = default;
    registration(const registration &) = delete;
    registration &operator=(const registration &) = delete;
    registration(registration &&other) noexcept;
    registration &operator=(registration &&other) noexcept;
    ~registration();

    void reset() noexcept;
    explicit operator bool() const noexcept { return id_ != 0; }

  private:
    enum class kind : u8
    {
        none,
        hard,
        soft,
    };

    registration(kind kind, u32 vector, u64 id) noexcept
        : kind_(kind)
        , vector_(vector)
        , id_(id)
    {
    }

    kind kind_ = kind::none;
    u32 vector_ = 0;
    u64 id_ = 0;

    friend registration register_handler(u32 vector, hard_handler handler);
    friend registration register_soft_handler(u32 vector, soft_handler handler);
};

// fn

void init();
void wakeup_soft_irq_daemon();
void raise_soft_irq(u64 soft_irq_number);

[[nodiscard]] registration register_handler(u32 vector, hard_handler handler);
[[nodiscard]] registration register_soft_handler(u32 vector, soft_handler handler);

} // namespace irq
