#pragma once

#include "kernel/iommu/backend.hpp"
#include "kernel/irq/vector_pool.hpp"
#include "kernel/types.hpp"

namespace iommu::intel_vtd
{

inline constexpr u16 interrupt_remapping_entry_count = 256;

struct fault_event_message
{
    u32 address = 0;
    u32 data = 0;
    u32 upper_address = 0;
};

struct interrupt_remapping_entry
{
    u64 low = 0;
    u64 high = 0;
};
static_assert(sizeof(interrupt_remapping_entry) == 16);

/// Encode the Intel VT-d remappable-format MSI message. This encoding is
/// provider-specific and must not be interpreted by generic PCI code.
constexpr bool encode_remappable_msi_message(u16 interrupt_index, msi_message &result) noexcept
{
    if (interrupt_index >= interrupt_remapping_entry_count)
        return false;

    constexpr u64 interrupt_address_base = 0xfee00000;
    constexpr u64 interrupt_index_low_shift = 5;
    constexpr u64 interrupt_format_remappable = 1ull << 4;
    constexpr u64 subhandle_valid = 1ull << 3;
    constexpr u64 interrupt_index_high_bit = 1ull << 2;
    const u64 address = interrupt_address_base |
                        ((static_cast<u64>(interrupt_index) & 0x7fff) << interrupt_index_low_shift) |
                        interrupt_format_remappable | subhandle_valid |
                        ((interrupt_index & 0x8000) != 0 ? interrupt_index_high_bit : 0);
    result = {address, 0};
    return true;
}

constexpr bool is_remappable_msi_message(const msi_message &message) noexcept
{
    constexpr u64 interrupt_index_low_mask = 0x7fff;
    constexpr u64 interrupt_index_high_bit = 1ull << 2;
    if ((message.address & 0xfff00000ull) != 0xfee00000ull ||
        (message.address & ((1ull << 4) | (1ull << 3))) != ((1ull << 4) | (1ull << 3)) || message.data != 0)
        return false;

    const auto interrupt_index = static_cast<u16>(((message.address >> 5) & interrupt_index_low_mask) |
                                                  ((message.address & interrupt_index_high_bit) != 0 ? 0x8000 : 0));
    msi_message expected{};
    return encode_remappable_msi_message(interrupt_index, expected) && expected.address == message.address &&
           expected.data == message.data;
}

constexpr bool is_external_interrupt_vector(u8 vector) noexcept
{
    return vector >= 32 && vector != irq::vector_pool::interrupt_drain_vector &&
           vector != irq::vector_pool::local_apic_timer_vector && vector != irq::vector_pool::ipi_call_vector &&
           vector != irq::vector_pool::ipi_reschedule_vector && vector != irq::vector_pool::ipi_tlb_vector &&
           vector != irq::vector_pool::spurious_vector;
}

/// Build a remapping-hardware fault-event message for xAPIC mode. These
/// messages bypass interrupt remapping and therefore carry a direct physical
/// destination, unlike PCI MSI messages routed through an IRTE.
constexpr bool encode_xapic_fault_event_message(u32 destination_apic_id, u8 vector,
                                                fault_event_message &result) noexcept
{
    if (destination_apic_id > 0xff || !is_external_interrupt_vector(vector))
        return false;

    constexpr u32 local_apic_msi_base = 0xfee00000;
    constexpr u32 destination_shift = 12;
    result = {local_apic_msi_base | (destination_apic_id << destination_shift), vector, 0};
    return true;
}

/// Build a fixed, physical-destination, edge-triggered MSI entry that validates
/// all 16 requester-ID bits. EIME controls the IRTE destination-ID encoding.
constexpr bool encode_pci_msi_irte(u16 requester_id, u32 destination_apic_id, u8 vector, bool extended_interrupt_mode,
                                   interrupt_remapping_entry &result) noexcept
{
    if (!is_external_interrupt_vector(vector) || (!extended_interrupt_mode && destination_apic_id > 0xff))
        return false;

    constexpr u64 present = 1;
    constexpr u64 vector_shift = 16;
    constexpr u64 destination_shift = 32;
    constexpr u64 xapic_destination_field_shift = 8;
    constexpr u64 verify_source_id_and_qualifier = 1ull << 18;

    const u64 encoded_destination = extended_interrupt_mode
                                        ? destination_apic_id
                                        : static_cast<u64>(destination_apic_id) << xapic_destination_field_shift;
    const interrupt_remapping_entry candidate{present | (static_cast<u64>(vector) << vector_shift) |
                                                  (encoded_destination << destination_shift),
                                              verify_source_id_and_qualifier | requester_id};
    result = candidate;
    return true;
}

/// Build a fixed-delivery IOAPIC IRTE. The source ID is the exact PCI
/// requester ID resolved from this IOAPIC's DMAR device scope. The trigger
/// mode must match the IOAPIC RTE.
constexpr bool encode_ioapic_irte(u16 requester_id, u32 destination_apic_id, u8 vector, bool level_triggered,
                                  bool extended_interrupt_mode, interrupt_remapping_entry &result) noexcept
{
    if (!is_external_interrupt_vector(vector) || (!extended_interrupt_mode && destination_apic_id > 0xff))
        return false;

    constexpr u64 present = 1;
    constexpr u64 trigger_mode_level = 1ull << 4;
    constexpr u64 vector_shift = 16;
    constexpr u64 destination_shift = 32;
    constexpr u64 xapic_destination_field_shift = 8;
    constexpr u64 verify_source_id_and_qualifier = 1ull << 18;

    const u64 encoded_destination = extended_interrupt_mode
                                        ? destination_apic_id
                                        : static_cast<u64>(destination_apic_id) << xapic_destination_field_shift;
    const interrupt_remapping_entry candidate{present | (level_triggered ? trigger_mode_level : 0) |
                                                  (static_cast<u64>(vector) << vector_shift) |
                                                  (encoded_destination << destination_shift),
                                              verify_source_id_and_qualifier | requester_id};
    result = candidate;
    return true;
}

/// Encode the Intel IOxAPIC remappable-format RTE. Index[14:0] occupies
/// bits 63:49, Index[15] occupies bit 11, and bit 48 selects remappable format.
constexpr bool encode_ioapic_remappable_rte(u16 index, u8 vector, bool low_polarity, bool level_triggered, bool masked,
                                            u64 &result) noexcept
{
    if (!is_external_interrupt_vector(vector))
        return false;

    constexpr u64 interrupt_format_remappable = 1ull << 48;
    constexpr u64 interrupt_index_low_shift = 49;
    constexpr u64 interrupt_index_high_bit = 1ull << 11;
    const u64 candidate = static_cast<u64>(vector) | interrupt_format_remappable |
                          ((static_cast<u64>(index) & 0x7fff) << interrupt_index_low_shift) |
                          ((index & 0x8000) != 0 ? interrupt_index_high_bit : 0) | (low_polarity ? 1ull << 13 : 0) |
                          (level_triggered ? 1ull << 15 : 0) | (masked ? 1ull << 16 : 0);
    result = candidate;
    return true;
}

} // namespace iommu::intel_vtd
