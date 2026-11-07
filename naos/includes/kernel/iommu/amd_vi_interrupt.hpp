#pragma once

#include "kernel/iommu/amd_vi_dte.hpp"
#include "kernel/types.hpp"

namespace iommu::amd_vi
{
enum class interrupt_control : u8
{
    abort = 0,
    passthrough = 1,
    remap = 2,
};

enum class interrupt_delivery_mode : u8
{
    fixed = 0,
    arbitrated = 1,
};

struct interrupt_remap_entry
{
    u32 value = 0;
};
static_assert(sizeof(interrupt_remap_entry) == 4);

struct interrupt_message_encoding
{
    u64 address = 0;
    u32 data = 0;
};

inline constexpr u64 interrupt_table_root_pointer_mask = 0x000fffffffffffc0ull;
inline constexpr u32 maximum_basic_interrupt_table_entries = 512;
inline constexpr u8 maximum_basic_interrupt_table_length = 9;
inline constexpr u64 basic_interrupt_msi_address = 0xfee00000ull;

/// AMD IOMMU 48882 §2.2.2 Table 7: update only the DTE interrupt-control
/// fields, preserving the DMA translation and permission fields. The basic
/// remapping mode supports at most 512 32-bit IRTEs per requester. A caller
/// may publish a nonzero table pointer while control remains abort during the
/// spec-required table-invalidate-before-enable transition.
constexpr bool set_device_table_interrupt_state(const device_table_entry &base, interrupt_control control,
                                                u64 table_root_physical, u8 table_length, bool ignore_unmapped,
                                                device_table_entry &result) noexcept
{
    if ((base.words[0] & 1u) == 0 || static_cast<u8>(control) > static_cast<u8>(interrupt_control::remap) ||
        table_length > maximum_basic_interrupt_table_length ||
        (control == interrupt_control::remap && table_root_physical == 0) ||
        (table_root_physical == 0 && (table_length != 0 || ignore_unmapped)) ||
        (table_root_physical != 0 &&
         ((table_root_physical & 0x7fu) != 0 || (table_root_physical & ~interrupt_table_root_pointer_mask) != 0)) ||
        (control != interrupt_control::remap && ignore_unmapped))
        return false;

    constexpr u64 interrupt_valid = 1ull;
    constexpr u64 ignore_unmapped_mask = 1ull << 5;
    constexpr u64 interrupt_table_length_mask = 0xfull << 1;
    constexpr u64 interrupt_table_root_mask = interrupt_table_root_pointer_mask;
    constexpr u64 interrupt_control_mask = 3ull << 60;

    device_table_entry candidate = base;
    candidate.words[2] &= ~(ignore_unmapped_mask | interrupt_table_length_mask | interrupt_table_root_mask |
                            interrupt_control_mask | interrupt_valid);
    candidate.words[2] |= interrupt_valid | (static_cast<u64>(control) << 60);
    if (table_root_physical != 0)
    {
        candidate.words[2] |= table_root_physical;
        candidate.words[2] |= static_cast<u64>(table_length) << 1;
        if (control == interrupt_control::remap)
            candidate.words[2] |= ignore_unmapped ? ignore_unmapped_mask : 0;
    }
    result = candidate;
    return true;
}

/// AMD IOMMU 48882 §2.2.5 Table 20 basic 32-bit IRTE. Reserved fields,
/// GuestMode, and RqEoi remain zero; xAPIC delivery uses physical destination.
constexpr bool encode_interrupt_remap_entry(u32 destination_apic_id, u8 vector, interrupt_delivery_mode delivery,
                                            bool suppress_fault_event, interrupt_remap_entry &result) noexcept
{
    result = {};
    if (destination_apic_id > 0xff || static_cast<u8>(delivery) > 1)
        return false;

    constexpr u32 remap_enable = 1u;
    constexpr u32 suppress_io_page_fault = 1u << 1;
    constexpr u32 interrupt_type_shift = 2;
    constexpr u32 destination_shift = 8;
    constexpr u32 vector_shift = 16;
    const u32 encoded = (static_cast<u32>(vector) << vector_shift) | (destination_apic_id << destination_shift) |
                        (static_cast<u32>(delivery) << interrupt_type_shift) | remap_enable |
                        (suppress_fault_event ? suppress_io_page_fault : 0);
    result = {encoded};
    return true;
}

/// In basic AMD MSI remapping, MSI data bits [10:0] select the IRTE. Keep
/// every other data bit zero so the IOMMU receives the fixed-delivery encoding.
constexpr bool encode_interrupt_table_offset_message(u16 table_index, interrupt_message_encoding &result) noexcept
{
    result = {};
    if (table_index >= maximum_basic_interrupt_table_entries)
        return false;
    result = {basic_interrupt_msi_address, table_index};
    return true;
}
} // namespace iommu::amd_vi
