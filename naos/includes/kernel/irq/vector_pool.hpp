#pragma once

#include "kernel/common.hpp"

namespace irq
{

/// Fixed-capacity ownership table for x86 interrupt vectors. The caller
/// serializes operations; the pool itself performs no allocation and is safe
/// to construct before the kernel heap is online.
class vector_pool
{
  public:
    static constexpr u32 vector_count = 256;
    /// Lowest vector is reserved for the post-EOI drain IPI. MSI vectors
    /// start in a higher APIC priority class so a drain barrier cannot pass
    /// an already-pending MSI.
    static constexpr u8 interrupt_drain_vector = 32;
    static constexpr u8 first_ioapic_vector = 33;
    static constexpr u8 first_msi_vector = 48;
    static constexpr u8 local_apic_timer_vector = 128;
    static constexpr u8 ipi_call_vector = 250;
    static constexpr u8 ipi_reschedule_vector = 251;
    static constexpr u8 ipi_tlb_vector = 252;
    static constexpr u8 spurious_vector = 255;

    constexpr vector_pool() noexcept
    {
        for (u32 vector = 0; vector < 32; ++vector)
            owners_[vector] = owner::architectural;
        owners_[interrupt_drain_vector] = owner::architectural;
        owners_[local_apic_timer_vector] = owner::architectural;
        owners_[ipi_call_vector] = owner::architectural;
        owners_[ipi_reschedule_vector] = owner::architectural;
        owners_[ipi_tlb_vector] = owner::architectural;
        owners_[spurious_vector] = owner::architectural;
    }

    u8 allocate_ioapic_vector() noexcept { return allocate(owner::ioapic, first_ioapic_vector); }
    u8 allocate_msi_vector() noexcept { return allocate(owner::msi, first_msi_vector); }

    bool release_ioapic_vector(u8 vector) noexcept
    {
        if (owners_[vector] != owner::ioapic)
            return false;
        owners_[vector] = owner::free;
        return true;
    }

    bool release_msi_vector(u8 vector) noexcept
    {
        if (owners_[vector] != owner::msi)
            return false;
        owners_[vector] = owner::free;
        return true;
    }

    bool quarantine_msi_vector(u8 vector) noexcept
    {
        if (owners_[vector] != owner::msi)
            return false;
        owners_[vector] = owner::quarantined;
        return true;
    }

    bool unavailable(u8 vector) const noexcept { return owners_[vector] != owner::free; }

  private:
    enum class owner : u8
    {
        free,
        architectural,
        ioapic,
        msi,
        quarantined,
    };

    u8 allocate(owner requested_owner, u32 first_vector) noexcept
    {
        for (u32 vector = first_vector; vector < spurious_vector; ++vector)
        {
            if (owners_[vector] != owner::free)
                continue;
            owners_[vector] = requested_owner;
            return static_cast<u8>(vector);
        }
        return 0;
    }

    owner owners_[vector_count]{};
};

} // namespace irq
