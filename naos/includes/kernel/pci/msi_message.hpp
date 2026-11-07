#pragma once

#include "kernel/iommu/backend.hpp"
#include "kernel/irq/vector_pool.hpp"

namespace pci
{
using msi_message = iommu::msi_message;

namespace detail
{
/// Encode the hardware word pair after the caller has allocated an MSI vector.
/// This is not an authorization decision or interrupt-remapping setup: MSI
/// writes to the APIC window bypass VT-d DMA translation on supported hardware.
constexpr bool encode_xapic_msi_message(u32 destination_apic_id, u8 vector, msi_message &result) noexcept
{
    if (destination_apic_id > 0xff || vector < 32 || vector == irq::vector_pool::local_apic_timer_vector ||
        vector == irq::vector_pool::ipi_call_vector || vector == irq::vector_pool::ipi_reschedule_vector ||
        vector == irq::vector_pool::ipi_tlb_vector || vector == irq::vector_pool::spurious_vector)
        return false;

    constexpr u64 local_apic_msi_base = 0xfee00000;
    const msi_message message{local_apic_msi_base | (static_cast<u64>(destination_apic_id) << 12), vector};
    result = message;
    return true;
}

constexpr bool is_valid_msi_message(const msi_message &message) noexcept { return message.data <= 0xffff; }
} // namespace detail
} // namespace pci
