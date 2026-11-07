#pragma once

#include "kernel/kobject.hpp"
#include "kernel/pci/bar.hpp"
#include "kernel/pci/osc_authority.hpp"
#include <array>

namespace pci
{
inline constexpr u32 max_root_resource_windows = 12;
inline constexpr u32 max_device_resource_windows = 6;
inline constexpr u32 max_pci_device_function_path = 16;

struct root_resource_window_evidence
{
    u32 resource_type = 0;
    u32 descriptor_revision = 0;
    u32 general_flags = 0;
    u32 type_specific_flags = 0;
    u64 granularity = 0;
    u64 minimum = 0;
    u64 maximum = 0;
    u64 translation_offset = 0;
    u64 length = 0;
    u64 type_specific_attributes = 0;
};

struct root_resource_evidence : root_feature_scope
{
    std::array<root_resource_window_evidence, max_root_resource_windows> windows{};
    u32 window_count = 0;
};

struct root_resource_attestation : root_feature_scope
{
    std::array<memory_window, max_root_resource_windows> memory_windows{};
    u32 memory_window_count = 0;
};

struct device_resource_window
{
    u64 base = 0;
    u64 length = 0;
    u32 flags = 0; // bit 0: writable according to ACPI memory _RW
};

/// Caller evidence from one platform-device `_CRS`. The path binds the
/// namespace device to the root-relative `_ADR` chain; the kernel later
/// requires the same chain when associating this authority with a BDF.
struct device_resource_evidence : root_feature_scope
{
    u64 device_id = 0;
    std::array<u8, max_pci_device_function_path> device_function_path{};
    u32 path_length = 0;
    std::array<device_resource_window, max_device_resource_windows> memory_windows{};
    u32 memory_window_count = 0;
};

struct device_resource_attestation : root_feature_scope
{
    u64 device_id = 0;
    std::array<u8, max_pci_device_function_path> device_function_path{};
    u32 path_length = 0;
    std::array<device_resource_window, max_device_resource_windows> memory_windows{};
    u32 memory_window_count = 0;
};

class root_resource_attestation_capability final : public kobject
{
  public:
    static constexpr type_e type_of() { return type_e::pci_root_resource_attestation; }

    root_resource_attestation_capability(process_id issuer, root_resource_attestation authority)
        : kobject(type_of())
        , issuer_(issuer)
        , authority_(authority)
    {
    }

    process_id issuer() const { return issuer_; }
    const root_resource_attestation &authority() const { return authority_; }
    bool capability_is_unique() const override { return true; }
    na_signal_t capability_signals() const override { return NA_SIGNAL_WRITABLE; }

  private:
    process_id issuer_;
    root_resource_attestation authority_;
};

class device_resource_attestation_capability final : public kobject
{
  public:
    static constexpr type_e type_of() { return type_e::pci_device_resource_attestation; }

    device_resource_attestation_capability(process_id issuer, device_resource_attestation authority)
        : kobject(type_of())
        , issuer_(issuer)
        , authority_(authority)
    {
    }

    process_id issuer() const { return issuer_; }
    const device_resource_attestation &authority() const { return authority_; }
    bool capability_is_unique() const override { return true; }
    na_signal_t capability_signals() const override { return NA_SIGNAL_WRITABLE; }

  private:
    process_id issuer_;
    device_resource_attestation authority_;
};

enum class root_resource_error : u8
{
    none,
    invalid_scope,
    root_mismatch,
    stale_scope,
    invalid_window_count,
    invalid_descriptor,
    invalid_range,
    invalid_translation,
    missing_bus_window,
    invalid_bus_window,
    missing_memory_window,
};

/// Validate platformd's complete root `_CRS` window evidence against the
/// kernel-bound root lineage. Only translated memory windows are retained as
/// authority; I/O and bus descriptors are validated but never become MMIO
/// mapping ranges.
inline root_resource_error validate_root_resource_evidence(const root_resource_evidence &evidence,
                                                           const root_feature_scope &expected,
                                                           root_resource_attestation &authority) noexcept
{
    authority = {};
    if (expected.root_id == 0 || expected.boot_epoch == 0 || expected.firmware_generation == 0 ||
        expected.aml_namespace_generation == 0 || expected.platform_topology_generation == 0 ||
        expected.pci_fabric_generation == 0 || expected.compatibility_profile_hash == 0 || expected.segment > 0xffff ||
        expected.start_bus > expected.end_bus || expected.end_bus > 0xff)
        return root_resource_error::invalid_scope;

    if (evidence.root_id != expected.root_id || evidence.segment != expected.segment ||
        evidence.start_bus != expected.start_bus || evidence.end_bus != expected.end_bus)
        return root_resource_error::root_mismatch;
    if (evidence.boot_epoch != expected.boot_epoch || evidence.firmware_generation != expected.firmware_generation ||
        evidence.aml_namespace_generation != expected.aml_namespace_generation ||
        evidence.platform_topology_generation != expected.platform_topology_generation ||
        evidence.pci_fabric_generation != expected.pci_fabric_generation ||
        evidence.compatibility_profile_hash != expected.compatibility_profile_hash)
        return root_resource_error::stale_scope;
    if (evidence.window_count == 0 || evidence.window_count > max_root_resource_windows)
        return root_resource_error::invalid_window_count;

    root_resource_attestation candidate{};
    bool bus_window_found = false;
    for (u32 index = 0; index < evidence.window_count; ++index)
    {
        const auto &window = evidence.windows[index];
        if (window.resource_type == 2)
        {
            if (window.descriptor_revision > 1 || (window.general_flags & 0xf0u) != 0 ||
                window.type_specific_flags != 0 || window.granularity != 0 || window.translation_offset != 0 ||
                window.length == 0 || window.minimum > window.maximum ||
                window.minimum > ~u64{0} - (window.length - 1) || window.minimum > 0xff)
                return root_resource_error::invalid_bus_window;

            const u64 last_bus = window.minimum + window.length - 1;
            if (last_bus > window.maximum || last_bus > 0xff || window.minimum != expected.start_bus ||
                last_bus != expected.end_bus || bus_window_found)
                return root_resource_error::invalid_bus_window;
            bus_window_found = true;
            continue;
        }

        if (window.resource_type >= 192 && window.resource_type <= 255)
            continue;
        if (window.resource_type > 1)
            return root_resource_error::invalid_descriptor;

        memory_window translated{};
        const auto decode_error = decode_root_address_window(
            window.resource_type, window.descriptor_revision, window.general_flags, window.granularity, window.minimum,
            window.maximum, window.translation_offset, window.length, translated);
        switch (decode_error)
        {
            case root_window_error::none:
                break;
            case root_window_error::unsupported_resource_type:
            case root_window_error::invalid_descriptor:
                return root_resource_error::invalid_descriptor;
            case root_window_error::invalid_range:
                return root_resource_error::invalid_range;
            case root_window_error::invalid_translation:
                return root_resource_error::invalid_translation;
        }

        if (window.resource_type == 0)
        {
            candidate.memory_windows[candidate.memory_window_count++] = translated;
        }
    }

    if (!bus_window_found)
        return root_resource_error::missing_bus_window;
    if (candidate.memory_window_count == 0)
        return root_resource_error::missing_memory_window;

    static_cast<root_feature_scope &>(candidate) = expected;
    authority = candidate;
    return root_resource_error::none;
}

enum class device_resource_error : u8
{
    none,
    invalid_root,
    root_mismatch,
    stale_scope,
    invalid_device,
    invalid_path,
    invalid_window_count,
    invalid_range,
    outside_root,
    overlapping_windows,
};

/// Validate a platformd-issued device `_CRS` claim against its already
/// kernel-validated root authority. This mints only bounded resource facts;
/// it does not itself authorize a physical mapping.
inline device_resource_error validate_device_resource_evidence(const device_resource_evidence &evidence,
                                                               const root_resource_attestation &root,
                                                               device_resource_attestation &authority) noexcept
{
    authority = {};
    if (root.root_id == 0 || root.boot_epoch == 0 || root.firmware_generation == 0 ||
        root.aml_namespace_generation == 0 || root.platform_topology_generation == 0 ||
        root.pci_fabric_generation == 0 || root.compatibility_profile_hash == 0 || root.segment > 0xffff ||
        root.start_bus > root.end_bus || root.end_bus > 0xff || root.memory_window_count == 0 ||
        root.memory_window_count > max_root_resource_windows)
        return device_resource_error::invalid_root;

    if (evidence.root_id != root.root_id || evidence.segment != root.segment || evidence.start_bus != root.start_bus ||
        evidence.end_bus != root.end_bus)
        return device_resource_error::root_mismatch;
    if (evidence.boot_epoch != root.boot_epoch || evidence.firmware_generation != root.firmware_generation ||
        evidence.aml_namespace_generation != root.aml_namespace_generation ||
        evidence.platform_topology_generation != root.platform_topology_generation ||
        evidence.pci_fabric_generation != root.pci_fabric_generation ||
        evidence.compatibility_profile_hash != root.compatibility_profile_hash)
        return device_resource_error::stale_scope;
    if (evidence.device_id == 0)
        return device_resource_error::invalid_device;
    if (evidence.path_length == 0 || evidence.path_length > max_pci_device_function_path)
        return device_resource_error::invalid_path;
    if (evidence.memory_window_count > max_device_resource_windows)
        return device_resource_error::invalid_window_count;

    for (u32 index = 0; index < root.memory_window_count; ++index)
    {
        const auto &window = root.memory_windows[index];
        if (window.length == 0 || window.base > ~u64{0} - window.length)
            return device_resource_error::invalid_root;
    }

    for (u32 index = 0; index < evidence.memory_window_count; ++index)
    {
        const auto &window = evidence.memory_windows[index];
        if ((window.flags & ~u32{1}) != 0 || window.length == 0 || window.base > ~u64{0} - window.length)
            return device_resource_error::invalid_range;
        const u64 window_end = window.base + window.length;
        bool contained = false;
        for (u32 root_index = 0; root_index < root.memory_window_count; ++root_index)
        {
            const auto &root_window = root.memory_windows[root_index];
            const u64 root_end = root_window.base + root_window.length;
            if (window.base >= root_window.base && window_end <= root_end)
            {
                contained = true;
                break;
            }
        }
        if (!contained)
            return device_resource_error::outside_root;

        for (u32 previous = 0; previous < index; ++previous)
        {
            const auto &other = evidence.memory_windows[previous];
            const u64 other_end = other.base + other.length;
            if (window.base < other_end && other.base < window_end)
                return device_resource_error::overlapping_windows;
        }
    }

    static_cast<root_feature_scope &>(authority) = static_cast<const root_feature_scope &>(root);
    authority.device_id = evidence.device_id;
    authority.path_length = evidence.path_length;
    authority.memory_window_count = evidence.memory_window_count;
    for (u32 index = 0; index < evidence.path_length; ++index)
        authority.device_function_path[index] = evidence.device_function_path[index];
    for (u32 index = 0; index < evidence.memory_window_count; ++index)
        authority.memory_windows[index] = evidence.memory_windows[index];
    return device_resource_error::none;
}
} // namespace pci
