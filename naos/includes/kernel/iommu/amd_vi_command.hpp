#pragma once

#include "kernel/types.hpp"

namespace iommu::amd_vi
{
inline constexpr u64 maximum_guest_address = (1ull << 48) - 1;
inline constexpr u64 invalidate_all_domain_pages_address = 0x7ffffffffffff000ull;

struct command_entry
{
    u64 words[2]{};
};
static_assert(sizeof(command_entry) == 16);

inline constexpr u64 command_opcode_shift = 60;
inline constexpr u64 completion_wait_opcode = 0x01;
inline constexpr u64 invalidate_device_table_entry_opcode = 0x02;
inline constexpr u64 invalidate_iommu_pages_opcode = 0x03;
inline constexpr u64 invalidate_interrupt_table_opcode = 0x05;
inline constexpr u64 invalidate_iommu_all_opcode = 0x08;

/// Build the ordered AMD command that completes all prior commands and raises
/// the IOMMU completion status bit. The controller polls and acknowledges that
/// status; this command does not rely on an MSI handler.
constexpr command_entry encode_completion_wait() noexcept
{
    constexpr u64 flush_queue = 1ull << 2;
    constexpr u64 completion_interrupt_status = 1ull << 1;
    return {{(completion_wait_opcode << command_opcode_shift) | flush_queue | completion_interrupt_status, 0}};
}

constexpr command_entry encode_device_table_entry_invalidation(u16 device_id) noexcept
{
    return {{(invalidate_device_table_entry_opcode << command_opcode_shift) | device_id, 0}};
}

/// Invalidate one 4 KiB IOVA or every translation in a Mode=4 domain. PDE is
/// set in both forms so cached directory entries cannot outlive changed tables.
constexpr bool encode_iommu_pages_invalidation(u16 domain_id, u64 iova, bool entire_domain,
                                               command_entry &result) noexcept
{
    result = {};
    if (entire_domain)
        iova = invalidate_all_domain_pages_address;
    else if ((iova & 0xfff) != 0 || iova > maximum_guest_address || iova > maximum_guest_address - 0xfff)
        return false;

    constexpr u64 invalidate_page_directory = 1ull << 1;
    constexpr u64 size_is_encoded_by_address = 1ull;
    const u64 flags = invalidate_page_directory | (entire_domain ? size_is_encoded_by_address : 0);
    command_entry candidate{
        {(invalidate_iommu_pages_opcode << command_opcode_shift) | (static_cast<u64>(domain_id) << 32), iova | flags}};
    result = candidate;
    return true;
}

constexpr command_entry encode_interrupt_table_invalidation(u16 device_id) noexcept
{
    return {{(invalidate_interrupt_table_opcode << command_opcode_shift) | device_id, 0}};
}

constexpr command_entry encode_invalidate_iommu_all() noexcept
{
    return {{invalidate_iommu_all_opcode << command_opcode_shift, 0}};
}
} // namespace iommu::amd_vi
