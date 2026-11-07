#pragma once

#include "kernel/pci/msi_capability.hpp"
#include "kernel/pci/msi_message.hpp"

namespace pci
{

enum class msi_config_error : u8
{
    none,
    invalid_operations,
    invalid_capability,
    invalid_message,
    msi_already_enabled,
    msix_already_enabled,
    msi_still_enabled,
    config_read_failed,
    route_unavailable,
    stale_identity,
    resource_unavailable,
    hardware_outcome_unknown,
};

struct msi_config_ops
{
    pci_config_read32 read32 = nullptr;
    bool (*write32)(void *context, u16 offset, u32 value) = nullptr;
    void *context = nullptr;
};

struct msi_config_snapshot
{
    msi_capability capability{};
    u32 capability_header = 0;
    u32 address_low = 0;
    u32 address_high = 0;
    u32 data = 0;
    u32 vector_mask = 0;
    msi_message programmed_message{};
};

inline constexpr u32 max_msix_table_entries = 2048;
inline constexpr u32 msix_mask_snapshot_words = max_msix_table_entries / 64;

struct msix_config_ops
{
    pci_config_read32 read_config32 = nullptr;
    bool (*write_config32)(void *context, u16 offset, u32 value) = nullptr;
    bool (*read_table32)(void *context, u64 offset, u32 &value) = nullptr;
    bool (*write_table32)(void *context, u64 offset, u32 value) = nullptr;
    void *context = nullptr;
    u64 table_bytes = 0;
};

struct msix_config_snapshot
{
    bool valid = false;
    msix_capability capability{};
    u16 original_control = 0;
    u32 original_entry0_address_low = 0;
    u32 original_entry0_address_high = 0;
    u32 original_entry0_data = 0;
    u32 original_entry0_vector_control = 0;
    msi_message programmed_message{};
    std::array<u64, msix_mask_snapshot_words> original_vector_masks{};
};

enum class interrupt_route_config_mode : u8
{
    conventional_msi,
    msix,
};

struct msix_table_mapping
{
    u64 virtual_base = 0;
    u64 page_count = 0;
    u64 table_bytes = 0;
    u64 table_offset = 0;
    u64 resource_lease_id = 0;
};

struct interrupt_route_config_snapshot
{
    interrupt_route_config_mode mode = interrupt_route_config_mode::conventional_msi;
    msi_config_snapshot msi{};
    msix_config_snapshot msix{};
    msix_table_mapping table_mapping{};
};

namespace detail
{
inline constexpr u8 pci_msi_capability_id = 0x05;
inline constexpr u32 pci_msi_enable_bit = 1u << 16;
inline constexpr u32 pci_msi_multiple_message_enable_mask = 0x7u << 20;
inline constexpr u32 pci_msi_multiple_message_capable_mask = 0x7u << 17;
inline constexpr u32 pci_msi_64_bit_capable = 1u << 23;
inline constexpr u32 pci_msi_per_vector_masking = 1u << 24;
inline constexpr u32 pci_msi_address_64_bit = 1u << 4;
inline constexpr u32 pci_msi_mask_offset_64_bit = 16;
inline constexpr u32 pci_msi_mask_offset_32_bit = 12;
inline constexpr u32 pci_msi_data_offset_64_bit = 12;
inline constexpr u32 pci_msi_data_offset_32_bit = 8;
inline constexpr u32 pci_msi_address_low_offset = 4;
inline constexpr u32 pci_msi_address_high_offset = 8;

inline bool validate_msi_capability(const msi_config_ops &operations, const msi_capability &capability, u32 &header,
                                    msi_config_error &failure) noexcept
{
    if (operations.read32 == nullptr || operations.write32 == nullptr)
    {
        failure = msi_config_error::invalid_operations;
        return false;
    }
    if (capability.offset < 0x40 || capability.offset > 0xfc || (capability.offset & 3) != 0 ||
        capability.maximum_messages == 0)
    {
        failure = msi_config_error::invalid_capability;
        return false;
    }
    if (!operations.read32(operations.context, capability.offset, header))
    {
        failure = msi_config_error::config_read_failed;
        return false;
    }

    const u16 control = static_cast<u16>(header >> 16);
    const u8 message_count_encoding = static_cast<u8>((control >> 1) & 0x7);
    const u8 maximum_messages = message_count_encoding <= 5 ? static_cast<u8>(1u << message_count_encoding) : 0;
    if (static_cast<u8>(header & 0xff) != pci_msi_capability_id || maximum_messages == 0 ||
        maximum_messages != capability.maximum_messages || ((control & (1u << 7)) != 0) != capability.address_64_bit ||
        ((control & (1u << 8)) != 0) != capability.per_vector_masking)
    {
        failure = msi_config_error::invalid_capability;
        return false;
    }
    return true;
}

inline bool validate_msix_disabled(const msi_config_ops &operations, msi_config_error &failure) noexcept
{
    msix_capability capability{};
    msix_capability_error capability_failure = msix_capability_error::none;
    if (find_msix_capability(operations.read32, operations.context, capability, capability_failure))
    {
        if (capability.enabled)
        {
            failure = msi_config_error::msix_already_enabled;
            return false;
        }
        return true;
    }

    switch (capability_failure)
    {
        case msix_capability_error::capability_not_found:
            return true;
        case msix_capability_error::config_reader_unavailable:
            failure = msi_config_error::invalid_operations;
            return false;
        case msix_capability_error::config_read_failed:
            failure = msi_config_error::config_read_failed;
            return false;
        case msix_capability_error::none:
            break;
        case msix_capability_error::capability_list_unavailable:
        case msix_capability_error::invalid_capability_pointer:
        case msix_capability_error::capability_chain_too_long:
        case msix_capability_error::duplicate_msix_capability:
        case msix_capability_error::invalid_bar_index:
        case msix_capability_error::invalid_capability:
        case msix_capability_error::truncated_capability:
        case msix_capability_error::invalid_bar_aperture:
        case msix_capability_error::table_out_of_bounds:
        case msix_capability_error::pba_out_of_bounds:
        case msix_capability_error::table_pba_overlap:
            break;
    }
    failure = msi_config_error::invalid_capability;
    return false;
}

inline bool write_config_and_verify(const msi_config_ops &operations, u16 offset, u32 expected, bool &hardware_touched,
                                    msi_config_error &failure) noexcept
{
    if (!operations.write32(operations.context, offset, expected))
    {
        hardware_touched = true;
        failure = msi_config_error::hardware_outcome_unknown;
        return false;
    }
    hardware_touched = true;

    u32 observed = 0;
    if (!operations.read32(operations.context, offset, observed))
    {
        failure = msi_config_error::hardware_outcome_unknown;
        return false;
    }
    if (observed != expected)
    {
        failure = msi_config_error::hardware_outcome_unknown;
        return false;
    }
    return true;
}

inline u16 data_offset(const msi_capability &capability)
{
    return static_cast<u16>(capability.offset +
                            (capability.address_64_bit ? pci_msi_data_offset_64_bit : pci_msi_data_offset_32_bit));
}

inline u16 mask_offset(const msi_capability &capability)
{
    return static_cast<u16>(capability.offset +
                            (capability.address_64_bit ? pci_msi_mask_offset_64_bit : pci_msi_mask_offset_32_bit));
}

inline bool validate_programmed_message(const msi_config_ops &operations, const msi_config_snapshot &snapshot,
                                        u32 capability_header, msi_config_error &failure) noexcept
{
    if (!validate_msix_disabled(operations, failure))
        return false;
    if (!is_valid_msi_message(snapshot.programmed_message) ||
        (capability_header & pci_msi_multiple_message_enable_mask) != 0)
    {
        failure = msi_config_error::invalid_message;
        return false;
    }

    u32 address_low = 0;
    u32 address_high = 0;
    u32 data = 0;
    if (!operations.read32(operations.context, snapshot.capability.offset + pci_msi_address_low_offset, address_low) ||
        (snapshot.capability.address_64_bit &&
         !operations.read32(operations.context, snapshot.capability.offset + pci_msi_address_high_offset,
                            address_high)) ||
        !operations.read32(operations.context, data_offset(snapshot.capability), data))
    {
        failure = msi_config_error::config_read_failed;
        return false;
    }

    const u64 address = static_cast<u64>(address_low) | (static_cast<u64>(address_high) << 32);
    if (address != snapshot.programmed_message.address ||
        static_cast<u16>(data) != static_cast<u16>(snapshot.programmed_message.data))
    {
        failure = msi_config_error::invalid_message;
        return false;
    }
    return true;
}

inline bool validate_msix_operations(const msix_config_ops &operations, msi_config_error &failure) noexcept
{
    if (operations.read_config32 == nullptr || operations.write_config32 == nullptr ||
        operations.read_table32 == nullptr || operations.write_table32 == nullptr)
    {
        failure = msi_config_error::invalid_operations;
        return false;
    }
    return true;
}

inline bool validate_conventional_msi_disabled(const msix_config_ops &operations, msi_config_error &failure) noexcept
{
    msi_capability capability{};
    msi_capability_error capability_failure = msi_capability_error::none;
    if (!find_msi_capability(operations.read_config32, operations.context, capability, capability_failure))
    {
        if (capability_failure == msi_capability_error::msi_capability_not_found ||
            capability_failure == msi_capability_error::capability_list_unavailable)
            return true;
        failure =
            capability_failure == msi_capability_error::config_reader_unavailable ? msi_config_error::invalid_operations
            : capability_failure == msi_capability_error::config_read_failed      ? msi_config_error::config_read_failed
                                                                             : msi_config_error::invalid_capability;
        return false;
    }

    u32 header = 0;
    if (!operations.read_config32(operations.context, capability.offset, header))
    {
        failure = msi_config_error::config_read_failed;
        return false;
    }
    if ((header & pci_msi_enable_bit) != 0)
    {
        failure = msi_config_error::msi_already_enabled;
        return false;
    }
    return true;
}

inline bool read_msix_control(const msix_config_ops &operations, const msix_capability &expected, u32 &header,
                              msi_config_error &failure) noexcept
{
    msix_capability current{};
    msix_capability_error capability_failure = msix_capability_error::none;
    if (!find_msix_capability(operations.read_config32, operations.context, current, capability_failure))
    {
        failure = capability_failure == msix_capability_error::config_read_failed ? msi_config_error::config_read_failed
                  : capability_failure == msix_capability_error::config_reader_unavailable
                      ? msi_config_error::invalid_operations
                      : msi_config_error::invalid_capability;
        return false;
    }
    if (current.offset != expected.offset || current.table_size != expected.table_size ||
        current.table_bar != expected.table_bar || current.table_offset != expected.table_offset ||
        current.pba_bar != expected.pba_bar || current.pba_offset != expected.pba_offset)
    {
        failure = msi_config_error::invalid_capability;
        return false;
    }
    if (!operations.read_config32(operations.context, expected.offset, header))
    {
        failure = msi_config_error::config_read_failed;
        return false;
    }
    return true;
}

inline bool write_msix_config_and_verify(const msix_config_ops &operations, u16 offset, u32 expected,
                                         msi_config_error &failure) noexcept
{
    if (!operations.write_config32(operations.context, offset, expected))
    {
        failure = msi_config_error::hardware_outcome_unknown;
        return false;
    }
    u32 observed = 0;
    if (!operations.read_config32(operations.context, offset, observed) || observed != expected)
    {
        failure = msi_config_error::hardware_outcome_unknown;
        return false;
    }
    return true;
}

inline bool read_msix_table_dword(const msix_config_ops &operations, u64 offset, u32 &value,
                                  msi_config_error &failure) noexcept
{
    if ((offset & 3) != 0 || offset > operations.table_bytes || sizeof(u32) > operations.table_bytes - offset ||
        !operations.read_table32(operations.context, offset, value))
    {
        failure = msi_config_error::resource_unavailable;
        return false;
    }
    return true;
}

inline bool write_msix_table_and_verify(const msix_config_ops &operations, u64 offset, u32 expected,
                                        msi_config_error &failure) noexcept
{
    if ((offset & 3) != 0 || offset > operations.table_bytes || sizeof(u32) > operations.table_bytes - offset ||
        !operations.write_table32(operations.context, offset, expected))
    {
        failure = msi_config_error::hardware_outcome_unknown;
        return false;
    }
    u32 observed = 0;
    if (!operations.read_table32(operations.context, offset, observed) || observed != expected)
    {
        failure = msi_config_error::hardware_outcome_unknown;
        return false;
    }
    return true;
}

inline constexpr u32 msix_enable_bit = 1u << 31;
inline constexpr u32 msix_function_mask_bit = 1u << 30;
inline constexpr u32 msix_table_size_mask = 0x7ffu << 16;
inline constexpr u32 msix_reserved_control_mask = 0x3800u << 16;
inline constexpr u64 msix_entry_bytes = 16;
inline constexpr u64 msix_vector_control_offset = 12;

inline u64 msix_vector_control_address(u32 vector) noexcept
{
    return static_cast<u64>(vector) * msix_entry_bytes + msix_vector_control_offset;
}

inline bool original_vector_masked(const msix_config_snapshot &snapshot, u32 vector) noexcept
{
    return (snapshot.original_vector_masks[vector / 64] & (u64{1} << (vector % 64))) != 0;
}

inline void set_original_vector_mask(msix_config_snapshot &snapshot, u32 vector, bool masked) noexcept
{
    const u64 bit = u64{1} << (vector % 64);
    auto &word = snapshot.original_vector_masks[vector / 64];
    word = masked ? word | bit : word & ~bit;
}

inline bool validate_msix_table_masks(const msix_config_ops &operations, u16 table_size, bool vector0_masked,
                                      msi_config_error &failure) noexcept
{
    for (u32 vector = 0; vector < table_size; ++vector)
    {
        u32 control = 0;
        if (!read_msix_table_dword(operations, msix_vector_control_address(vector), control, failure))
            return false;
        const bool expected_mask = vector == 0 ? vector0_masked : true;
        if ((control & ~1u) != 0 || ((control & 1u) != 0) != expected_mask)
        {
            failure = msi_config_error::invalid_capability;
            return false;
        }
    }
    return true;
}

inline bool validate_msix_programmed_table(const msix_config_ops &operations, const msix_config_snapshot &snapshot,
                                           bool vector0_masked, msi_config_error &failure) noexcept
{
    if (!validate_msix_table_masks(operations, snapshot.capability.table_size, vector0_masked, failure))
        return false;
    std::array<u32, 3> message_words{};
    for (u32 index = 0; index < message_words.size(); ++index)
    {
        if (!read_msix_table_dword(operations, index * sizeof(u32), message_words[index], failure))
            return false;
    }
    if (message_words[0] != static_cast<u32>(snapshot.programmed_message.address) ||
        message_words[1] != static_cast<u32>(snapshot.programmed_message.address >> 32) ||
        message_words[2] != snapshot.programmed_message.data)
    {
        failure = msi_config_error::invalid_message;
        return false;
    }
    return true;
}
} // namespace detail

/// Snapshot a disabled MSI capability and install a single remappable MSI
/// message while leaving the device's MSI Enable bit clear and vector 0 masked.
inline bool prepare_remappable_msi(const msi_config_ops &operations, const msi_capability &capability,
                                   const msi_message &message, msi_config_snapshot &previous,
                                   msi_config_error &failure) noexcept
{
    failure = msi_config_error::none;
    previous = {};
    if (!detail::is_valid_msi_message(message) || (!capability.address_64_bit && message.address > 0xffffffffull))
    {
        failure = msi_config_error::invalid_message;
        return false;
    }

    u32 capability_header = 0;
    if (!detail::validate_msi_capability(operations, capability, capability_header, failure))
        return false;
    if ((capability_header & detail::pci_msi_enable_bit) != 0)
    {
        failure = msi_config_error::msi_already_enabled;
        return false;
    }
    if (!detail::validate_msix_disabled(operations, failure))
        return false;

    msi_config_snapshot candidate{};
    candidate.capability = capability;
    candidate.capability_header = capability_header;
    candidate.programmed_message = message;
    if (!operations.read32(operations.context, capability.offset + detail::pci_msi_address_low_offset,
                           candidate.address_low) ||
        (capability.address_64_bit &&
         !operations.read32(operations.context, capability.offset + detail::pci_msi_address_high_offset,
                            candidate.address_high)) ||
        !operations.read32(operations.context, detail::data_offset(capability), candidate.data) ||
        (capability.per_vector_masking &&
         !operations.read32(operations.context, detail::mask_offset(capability), candidate.vector_mask)))
    {
        failure = msi_config_error::config_read_failed;
        return false;
    }
    previous = candidate;

    bool hardware_touched = false;
    const u32 disabled_control =
        capability_header & ~(detail::pci_msi_enable_bit | detail::pci_msi_multiple_message_enable_mask);
    if (disabled_control != capability_header &&
        !detail::write_config_and_verify(operations, capability.offset, disabled_control, hardware_touched, failure))
        return false;

    if (capability.per_vector_masking)
    {
        const u32 masked_vector = candidate.vector_mask | 1u;
        if (masked_vector != candidate.vector_mask &&
            !detail::write_config_and_verify(operations, detail::mask_offset(capability), masked_vector,
                                             hardware_touched, failure))
            return false;
    }

    const u32 address_low = static_cast<u32>(message.address);
    if (address_low != candidate.address_low &&
        !detail::write_config_and_verify(operations, capability.offset + detail::pci_msi_address_low_offset,
                                         address_low, hardware_touched, failure))
        return false;
    if (capability.address_64_bit)
    {
        const u32 address_high = static_cast<u32>(message.address >> 32);
        if (address_high != candidate.address_high &&
            !detail::write_config_and_verify(operations, capability.offset + detail::pci_msi_address_high_offset,
                                             address_high, hardware_touched, failure))
            return false;
    }

    const u32 data = (candidate.data & 0xffff0000u) | (message.data & 0xffffu);
    if (data != candidate.data &&
        !detail::write_config_and_verify(operations, detail::data_offset(capability), data, hardware_touched, failure))
        return false;
    return true;
}

/// Enable/disable the single prepared MSI. A maskable capability masks vector
/// 0 before disabling and unmasks it before enabling. Without PVM, MSI Enable
/// is the mask control.
inline bool set_remappable_msi_enabled(const msi_config_ops &operations, const msi_config_snapshot &snapshot,
                                       bool enabled, msi_config_error &failure) noexcept
{
    failure = msi_config_error::none;
    u32 header = 0;
    if (!detail::validate_msi_capability(operations, snapshot.capability, header, failure))
        return false;
    if (!detail::validate_programmed_message(operations, snapshot, header, failure))
        return false;
    if (!enabled && (header & detail::pci_msi_enable_bit) == 0 && !snapshot.capability.per_vector_masking)
        return true;

    bool hardware_touched = false;
    if (snapshot.capability.per_vector_masking)
    {
        const u16 offset = detail::mask_offset(snapshot.capability);
        u32 mask = 0;
        if (!operations.read32(operations.context, offset, mask))
        {
            failure = msi_config_error::config_read_failed;
            return false;
        }
        const u32 updated_mask = enabled ? (mask & ~1u) : (mask | 1u);
        if (updated_mask != mask &&
            !detail::write_config_and_verify(operations, offset, updated_mask, hardware_touched, failure))
            return false;
    }

    const u32 updated_header = enabled ? (header | detail::pci_msi_enable_bit) : (header & ~detail::pci_msi_enable_bit);
    if (updated_header != header && !detail::write_config_and_verify(operations, snapshot.capability.offset,
                                                                     updated_header, hardware_touched, failure))
        return false;
    return true;
}

/// Restore the pre-route address/data/control only after MSI has been disabled
/// and its source has been drained. The original MSI Enable bit is required to
/// be clear; an active firmware route is never re-enabled by teardown.
inline bool restore_msi_configuration(const msi_config_ops &operations, const msi_config_snapshot &snapshot,
                                      msi_config_error &failure) noexcept
{
    failure = msi_config_error::none;
    u32 header = 0;
    if (!detail::validate_msi_capability(operations, snapshot.capability, header, failure))
        return false;
    if (!detail::validate_programmed_message(operations, snapshot, header, failure))
        return false;

    u32 current_mask = 0;
    if (snapshot.capability.per_vector_masking &&
        !operations.read32(operations.context, detail::mask_offset(snapshot.capability), current_mask))
    {
        failure = msi_config_error::config_read_failed;
        return false;
    }
    if ((header & detail::pci_msi_enable_bit) != 0 ||
        (snapshot.capability.per_vector_masking && (current_mask & 1u) == 0))
    {
        failure = msi_config_error::msi_still_enabled;
        return false;
    }
    if ((snapshot.capability_header & detail::pci_msi_enable_bit) != 0)
    {
        failure = msi_config_error::msi_already_enabled;
        return false;
    }

    bool hardware_touched = false;
    if (!detail::write_config_and_verify(operations, snapshot.capability.offset + detail::pci_msi_address_low_offset,
                                         snapshot.address_low, hardware_touched, failure))
        return false;
    if (snapshot.capability.address_64_bit &&
        !detail::write_config_and_verify(operations, snapshot.capability.offset + detail::pci_msi_address_high_offset,
                                         snapshot.address_high, hardware_touched, failure))
        return false;
    if (!detail::write_config_and_verify(operations, detail::data_offset(snapshot.capability), snapshot.data,
                                         hardware_touched, failure))
        return false;
    if (!detail::write_config_and_verify(operations, snapshot.capability.offset,
                                         snapshot.capability_header & ~detail::pci_msi_enable_bit, hardware_touched,
                                         failure))
        return false;
    if (snapshot.capability.per_vector_masking &&
        !detail::write_config_and_verify(operations, detail::mask_offset(snapshot.capability), snapshot.vector_mask,
                                         hardware_touched, failure))
        return false;
    return true;
}

/// Prepare one MSI-X vector (entry zero) using a kernel-owned table mapping.
/// Every other vector is masked before the function can be enabled, so no
/// unowned table entry can emit a message through the provider route.
inline bool prepare_remappable_msix(const msix_config_ops &operations, const msi_message &message,
                                    msix_config_snapshot &previous, msi_config_error &failure) noexcept
{
    failure = msi_config_error::none;
    previous = {};
    if (!detail::is_valid_msi_message(message))
    {
        failure = msi_config_error::invalid_message;
        return false;
    }
    if (!detail::validate_msix_operations(operations, failure))
        return false;

    msix_capability capability{};
    msix_capability_error capability_failure = msix_capability_error::none;
    if (!find_msix_capability(operations.read_config32, operations.context, capability, capability_failure))
    {
        failure = capability_failure == msix_capability_error::config_read_failed ? msi_config_error::config_read_failed
                  : capability_failure == msix_capability_error::config_reader_unavailable
                      ? msi_config_error::invalid_operations
                      : msi_config_error::invalid_capability;
        return false;
    }
    if (capability.enabled)
    {
        failure = msi_config_error::msix_already_enabled;
        return false;
    }
    if (!detail::validate_conventional_msi_disabled(operations, failure))
        return false;

    const u64 required_table_bytes = static_cast<u64>(capability.table_size) * detail::msix_entry_bytes;
    if (capability.table_size == 0 || capability.table_size > max_msix_table_entries ||
        operations.table_bytes < required_table_bytes)
    {
        failure = msi_config_error::resource_unavailable;
        return false;
    }

    u32 capability_header = 0;
    if (!detail::read_msix_control(operations, capability, capability_header, failure))
        return false;
    if ((capability_header & detail::msix_enable_bit) != 0)
    {
        failure = msi_config_error::msix_already_enabled;
        return false;
    }
    if ((capability_header & detail::msix_table_size_mask) != (static_cast<u32>(capability.table_size - 1) << 16) ||
        (capability_header & detail::msix_reserved_control_mask) != 0)
    {
        failure = msi_config_error::invalid_capability;
        return false;
    }

    msix_config_snapshot candidate{};
    candidate.capability = capability;
    candidate.original_control = static_cast<u16>(capability_header >> 16);
    candidate.programmed_message = message;
    if (!detail::read_msix_table_dword(operations, 0, candidate.original_entry0_address_low, failure) ||
        !detail::read_msix_table_dword(operations, sizeof(u32), candidate.original_entry0_address_high, failure) ||
        !detail::read_msix_table_dword(operations, 2 * sizeof(u32), candidate.original_entry0_data, failure) ||
        !detail::read_msix_table_dword(operations, detail::msix_vector_control_offset,
                                       candidate.original_entry0_vector_control, failure))
        return false;

    for (u32 vector = 0; vector < capability.table_size; ++vector)
    {
        u32 vector_control = 0;
        if (!detail::read_msix_table_dword(operations, detail::msix_vector_control_address(vector), vector_control,
                                           failure))
            return false;
        if ((vector_control & ~1u) != 0)
        {
            failure = msi_config_error::invalid_capability;
            return false;
        }
        detail::set_original_vector_mask(candidate, vector, (vector_control & 1u) != 0);
    }
    candidate.valid = true;
    previous = candidate;

    bool hardware_touched = false;
    const u32 function_masked_header = capability_header | detail::msix_function_mask_bit;
    if (function_masked_header != capability_header &&
        !detail::write_msix_config_and_verify(operations, capability.offset, function_masked_header, failure))
        return false;
    hardware_touched = function_masked_header != capability_header;

    for (u32 vector = 0; vector < capability.table_size; ++vector)
    {
        if (detail::original_vector_masked(candidate, vector))
            continue;
        if (!detail::write_msix_table_and_verify(operations, detail::msix_vector_control_address(vector), 1, failure))
        {
            hardware_touched = true;
            return false;
        }
        hardware_touched = true;
    }

    const std::array<u32, 3> message_words{
        {static_cast<u32>(message.address), static_cast<u32>(message.address >> 32), message.data}};
    for (u32 index = 0; index < message_words.size(); ++index)
    {
        u32 current = 0;
        if (!detail::read_msix_table_dword(operations, index * sizeof(u32), current, failure))
        {
            failure = hardware_touched ? msi_config_error::hardware_outcome_unknown : failure;
            return false;
        }
        if (current != message_words[index] &&
            !detail::write_msix_table_and_verify(operations, index * sizeof(u32), message_words[index], failure))
        {
            hardware_touched = true;
            return false;
        }
        hardware_touched = hardware_touched || current != message_words[index];
    }

    if (!detail::validate_msix_table_masks(operations, capability.table_size, true, failure))
    {
        failure = hardware_touched ? msi_config_error::hardware_outcome_unknown : failure;
        return false;
    }
    return true;
}

/// Mask/unmask the single routed MSI-X function. Function Mask is asserted
/// before disabling or modifying vector zero; unmasking enables the function
/// while globally masked, unmasks entry zero, then clears Function Mask.
inline bool set_remappable_msix_enabled(const msix_config_ops &operations, const msix_config_snapshot &snapshot,
                                        bool enabled, msi_config_error &failure) noexcept
{
    failure = msi_config_error::none;
    if (!detail::validate_msix_operations(operations, failure) || !snapshot.valid ||
        !detail::is_valid_msi_message(snapshot.programmed_message))
    {
        if (failure == msi_config_error::none)
            failure = msi_config_error::invalid_capability;
        return false;
    }
    if (!detail::validate_conventional_msi_disabled(operations, failure))
        return false;

    u32 header = 0;
    if (!detail::read_msix_control(operations, snapshot.capability, header, failure))
        return false;
    const bool function_masked = (header & detail::msix_function_mask_bit) != 0;
    const bool currently_enabled = (header & detail::msix_enable_bit) != 0;
    if (!detail::validate_msix_programmed_table(operations, snapshot, function_masked || !currently_enabled, failure))
        return false;
    if (function_masked && currently_enabled)
    {
        failure = msi_config_error::hardware_outcome_unknown;
        return false;
    }
    if (!function_masked && !currently_enabled)
    {
        failure = msi_config_error::invalid_capability;
        return false;
    }

    if (enabled && currently_enabled && !function_masked)
        return true;
    if (!enabled && !currently_enabled && function_masked)
        return true;

    if (enabled)
    {
        if (!function_masked)
        {
            failure = msi_config_error::invalid_capability;
            return false;
        }
        if (!currently_enabled)
        {
            const u32 enabled_header = header | detail::msix_enable_bit;
            if (!detail::write_msix_config_and_verify(operations, snapshot.capability.offset, enabled_header, failure))
                return false;
            header = enabled_header;
        }

        if (!detail::write_msix_table_and_verify(operations, detail::msix_vector_control_offset, 0, failure))
            return false;
        const u32 unmasked_header = header & ~detail::msix_function_mask_bit;
        return unmasked_header == header ||
               detail::write_msix_config_and_verify(operations, snapshot.capability.offset, unmasked_header, failure);
    }

    if (!function_masked)
    {
        const u32 function_masked_header = header | detail::msix_function_mask_bit;
        if (!detail::write_msix_config_and_verify(operations, snapshot.capability.offset, function_masked_header,
                                                  failure))
            return false;
        header = function_masked_header;
    }
    if (!detail::write_msix_table_and_verify(operations, detail::msix_vector_control_offset, 1, failure))
        return false;
    const u32 disabled_header = header & ~detail::msix_enable_bit;
    return disabled_header == header ||
           detail::write_msix_config_and_verify(operations, snapshot.capability.offset, disabled_header, failure);
}

/// Restore the original table-zero message and per-vector mask bits after the
/// source is disabled and the provider route is retired. The original MSI-X Enable bit is
/// never restored, so teardown cannot reactivate a firmware-owned route.
inline bool restore_msix_configuration(const msix_config_ops &operations, const msix_config_snapshot &snapshot,
                                       msi_config_error &failure) noexcept
{
    failure = msi_config_error::none;
    if (!detail::validate_msix_operations(operations, failure) || !snapshot.valid)
    {
        if (failure == msi_config_error::none)
            failure = msi_config_error::invalid_capability;
        return false;
    }
    if (!detail::validate_conventional_msi_disabled(operations, failure))
        return false;

    u32 header = 0;
    if (!detail::read_msix_control(operations, snapshot.capability, header, failure))
        return false;
    if ((header & detail::msix_enable_bit) != 0 || (header & detail::msix_function_mask_bit) == 0)
    {
        failure = msi_config_error::msi_still_enabled;
        return false;
    }
    if (!detail::validate_msix_programmed_table(operations, snapshot, true, failure))
        return false;

    const std::array<u32, 4> original_entry0{{snapshot.original_entry0_address_low,
                                              snapshot.original_entry0_address_high, snapshot.original_entry0_data,
                                              snapshot.original_entry0_vector_control}};
    for (u32 index = 0; index < original_entry0.size(); ++index)
    {
        if (!detail::write_msix_table_and_verify(operations, index * sizeof(u32), original_entry0[index], failure))
            return false;
    }
    for (u32 vector = 1; vector < snapshot.capability.table_size; ++vector)
    {
        if (!detail::original_vector_masked(snapshot, vector) &&
            !detail::write_msix_table_and_verify(operations, detail::msix_vector_control_address(vector), 0, failure))
            return false;
    }

    const u16 restored_control = snapshot.original_control & ~static_cast<u16>(1u << 15);
    const u32 restored_header = (header & 0xffffu) | (static_cast<u32>(restored_control) << 16);
    if (restored_header != header &&
        !detail::write_msix_config_and_verify(operations, snapshot.capability.offset, restored_header, failure))
        return false;
    return true;
}

} // namespace pci
