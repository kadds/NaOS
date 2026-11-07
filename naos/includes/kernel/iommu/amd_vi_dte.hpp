#pragma once

#include "kernel/types.hpp"

namespace iommu::amd_vi
{
inline constexpr u32 device_table_entry_bytes = 32;
inline constexpr u32 maximum_device_id_count = 1u << 16;
inline constexpr u64 dte_page_size = 4096;
inline constexpr u64 dte_page_address_mask = 0x000ffffffffff000ull;
inline constexpr u32 full_device_table_bytes = device_table_entry_bytes * maximum_device_id_count;
inline constexpr u16 default_deny_domain_id = 0xffff;

struct device_table_entry
{
    u64 words[4]{};
};
static_assert(sizeof(device_table_entry) == device_table_entry_bytes);
static_assert(full_device_table_bytes == 2 * 1024 * 1024);

/// AMD IOMMU 48882 §2.2.2 Tables 7–9: valid, translated-format DTE with
/// translation disabled, both DMA permissions clear, and fixed/arbitrated
/// interrupts aborted. DomainID zero is reserved by this kernel's allocation
/// policy; the caller supplies an allocated nonzero ID.
constexpr bool encode_default_deny_device_table_entry(u16 domain_id, device_table_entry &result) noexcept
{
    if (domain_id == 0)
        return false;

    constexpr u64 valid = 1ull << 0;
    constexpr u64 translation_valid = 1ull << 1;
    constexpr u64 interrupt_valid = 1ull << 0;
    constexpr u32 domain_id_shift_in_word_1 = 0;

    device_table_entry candidate{};
    candidate.words[0] = valid | translation_valid;
    candidate.words[1] = static_cast<u64>(domain_id) << domain_id_shift_in_word_1;
    candidate.words[2] = interrupt_valid; // IntCtl=00b: abort fixed/arbitrated interrupts.
    result = candidate;
    return true;
}

/// Preserve the IVHD-declared interrupt behavior for an IOAPIC's special
/// requester while leaving its DMA permissions denied. IntCtl=01b forwards
/// fixed/arbitrated interrupts without remapping; callers may use this only
/// until the platform has installed an AMD interrupt table for that source.
constexpr bool encode_ioapic_default_deny_device_table_entry(u16 domain_id, u8 ivhd_dte_setting,
                                                             device_table_entry &result) noexcept
{
    if (domain_id == 0 || (ivhd_dte_setting & (1u << 3)) != 0)
        return false; // ATCPermit has no meaning for an IOAPIC requester.

    constexpr u64 interrupt_valid = 1ull << 0;
    constexpr u64 interrupt_control_passthrough = 1ull << 60; // DTE[189:188] = 01b.
    constexpr u32 domain_id_shift_in_word_1 = 0;

    device_table_entry candidate{};
    candidate.words[0] = (1ull << 0) | (1ull << 1); // V=1, TV=1, Mode=000b; IR=IW=0.
    candidate.words[1] = static_cast<u64>(domain_id) << domain_id_shift_in_word_1;
    candidate.words[1] |= static_cast<u64>(ivhd_dte_setting & 0x30u) << 36; // DTE[SysMgt]=IVHD[5:4].
    candidate.words[2] = interrupt_valid | interrupt_control_passthrough;
    candidate.words[2] |= static_cast<u64>(ivhd_dte_setting & 0x04u) << 56; // NMIPass -> DTE bit 186.
    candidate.words[2] |= static_cast<u64>(ivhd_dte_setting & 0x02u) << 56; // EIntPass -> DTE bit 185.
    candidate.words[2] |= static_cast<u64>(ivhd_dte_setting & 0x01u) << 56; // InitPass -> DTE bit 184.
    candidate.words[2] |= static_cast<u64>(ivhd_dte_setting & 0x40u) << 56; // Lint0Pass -> DTE bit 190.
    candidate.words[2] |= static_cast<u64>(ivhd_dte_setting & 0x80u) << 56; // Lint1Pass -> DTE bit 191.
    result = candidate;
    return true;
}

/// Encode a four-level translated DMA domain. The DTE allows both access
/// directions at the device level; each leaf PTE narrows access to the exact
/// authorized mapping. IV=1 with IntCtl=00b keeps interrupts aborted until an
/// interrupt-remapping table is installed for the device.
constexpr bool encode_dma_domain_device_table_entry(u16 domain_id, u64 root_table_physical,
                                                    device_table_entry &result) noexcept
{
    if (domain_id == 0 || domain_id == default_deny_domain_id || (root_table_physical & (dte_page_size - 1)) != 0 ||
        root_table_physical == 0 || (root_table_physical & ~dte_page_address_mask) != 0)
        return false;

    constexpr u64 valid = 1ull << 0;
    constexpr u64 translation_valid = 1ull << 1;
    constexpr u64 four_level_mode = 4ull << 9;
    constexpr u64 read = 1ull << 61;
    constexpr u64 write = 1ull << 62;
    constexpr u32 domain_id_shift_in_word_1 = 0;
    constexpr u64 interrupt_valid = 1ull << 0;

    device_table_entry candidate{};
    candidate.words[0] = root_table_physical | four_level_mode | translation_valid | valid | read | write;
    candidate.words[1] = static_cast<u64>(domain_id) << domain_id_shift_in_word_1;
    candidate.words[2] = interrupt_valid;
    result = candidate;
    return true;
}

constexpr bool is_default_deny_device_table_entry(const device_table_entry &entry, u16 domain_id) noexcept
{
    constexpr u64 valid_and_translation_valid = (1ull << 0) | (1ull << 1);
    constexpr u32 domain_id_shift_in_word_1 = 0;
    constexpr u64 interrupt_valid = 1ull << 0;

    return domain_id != 0 && entry.words[0] == valid_and_translation_valid &&
           entry.words[1] == (static_cast<u64>(domain_id) << domain_id_shift_in_word_1) &&
           entry.words[2] == interrupt_valid && entry.words[3] == 0;
}

/// Fill a physically contiguous full-width DTE table with valid, translated-
/// format entries that deny DMA and fixed/arbitrated interrupts. The caller
/// must publish the table only after flushing the complete 2 MiB range.
inline bool initialize_full_default_deny_device_table(void *storage, u32 storage_bytes, u16 domain_id) noexcept
{
    if (storage == nullptr || storage_bytes != full_device_table_bytes ||
        (reinterpret_cast<u64>(storage) & (dte_page_size - 1)) != 0 || domain_id == 0)
        return false;

    device_table_entry deny_entry{};
    if (!encode_default_deny_device_table_entry(domain_id, deny_entry))
        return false;
    auto *entries = static_cast<device_table_entry *>(storage);
    for (u32 index = 0; index < maximum_device_id_count; ++index)
        entries[index] = deny_entry;
    return true;
}
} // namespace iommu::amd_vi
