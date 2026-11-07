#pragma once
#include "arch/idt.hpp"
#include "kernel/common.hpp"
#include "kernel/irq/vector_pool.hpp"
#include "types.hpp"
namespace irq
{
bool in_irq_context();


namespace hard_vector
{
enum hard_vector
{
    // Lowest external priority class: a post-EOI barrier for draining a
    // function's higher-priority MSI vector on its destination CPU.
    interrupt_drain = irq::vector_pool::interrupt_drain_vector,
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
    iommu_fault = 7,
    COUNT,
};
} // namespace soft_vector

/// RAII ownership of one dynamically allocated MSI interrupt vector.
class msi_vector_lease
{
  public:
    msi_vector_lease() noexcept = default;
    msi_vector_lease(const msi_vector_lease &) = delete;
    msi_vector_lease &operator=(const msi_vector_lease &) = delete;
    msi_vector_lease(msi_vector_lease &&other) noexcept;
    msi_vector_lease &operator=(msi_vector_lease &&other) noexcept;
    ~msi_vector_lease();

    u8 vector() const noexcept { return vector_; }
    explicit operator bool() const noexcept { return vector_ != 0; }
    /// Return a vector to the shared pool after its source has been quiesced.
    void reset() noexcept;
    /// Keep the vector unavailable if teardown cannot prove it safe to recycle.
    bool quarantine() noexcept;

  private:
    explicit msi_vector_lease(u8 vector) noexcept
        : vector_(vector)
    {
    }

    u8 vector_ = 0;

    friend msi_vector_lease allocate_msi_vector() noexcept;
};

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
    /// Replace this hard handler with a static sink under the vector's writer
    /// lock. Use when an MSI source may still deliver after uncertain teardown.
    bool quarantine() noexcept;
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
[[nodiscard]] u8 allocate_ioapic_vector() noexcept;
bool release_ioapic_vector(u8 vector) noexcept;
[[nodiscard]] msi_vector_lease allocate_msi_vector() noexcept;

[[nodiscard]] registration register_handler(u32 vector, hard_handler handler);
[[nodiscard]] registration register_soft_handler(u32 vector, soft_handler handler);

} // namespace irq
