#pragma once

#include "kernel/types.hpp"

namespace pci
{
inline constexpr u32 implemented_pci_osc_support_mask = 1u << 3; // segment groups
inline constexpr u32 implemented_pci_osc_control_mask = 0;       // no native PCI control methods are active yet

struct root_feature_scope
{
    u64 root_id = 0;
    u64 boot_epoch = 0;
    u64 firmware_generation = 0;
    u64 aml_namespace_generation = 0;
    u64 platform_topology_generation = 0;
    u64 pci_fabric_generation = 0;
    u64 compatibility_profile_hash = 0;
    u32 segment = 0;
    u32 start_bus = 0;
    u32 end_bus = 0;
};

constexpr root_feature_scope bind_root_feature_scope(u64 root_id, u64 boot_epoch, u64 firmware_generation,
                                                     u64 aml_namespace_generation, u64 platform_topology_generation,
                                                     u64 pci_fabric_generation, u64 compatibility_profile_hash,
                                                     u32 segment, u32 start_bus, u32 end_bus) noexcept
{
    return {root_id,
            boot_epoch,
            firmware_generation,
            aml_namespace_generation,
            platform_topology_generation,
            pci_fabric_generation,
            compatibility_profile_hash,
            segment,
            start_bus,
            end_bus};
}

struct root_feature_evidence : root_feature_scope
{
    u32 requested_support = 0;
    u32 requested_control = 0;
    u32 query_support = 0;
    u32 query_granted_control = 0;
    u32 commit_support = 0;
    u32 granted_control = 0;
    u32 denied_control = 0;
    u32 masked_control = 0;
    u32 query_errors = 0;
    u32 commit_errors = 0;
};

struct root_feature_policy
{
    u32 implemented_support_mask = 0;
    u32 implemented_control_mask = 0;
};

enum class root_feature_error : u8
{
    none,
    invalid_scope,
    stale_scope,
    root_mismatch,
    unsupported_mask,
    query_failed,
    commit_failed,
    incomplete_negotiation,
    inconsistent_result,
    no_control_granted,
};

/// Validate platformd's typed _OSC result against the kernel's expected root
/// lineage and local feature masks. This does not create control authority.
constexpr root_feature_error validate_root_feature_evidence(const root_feature_evidence &evidence,
                                                            const root_feature_scope &expected,
                                                            const root_feature_policy &policy) noexcept
{
    if (expected.root_id == 0 || expected.boot_epoch == 0 || expected.firmware_generation == 0 ||
        expected.aml_namespace_generation == 0 || expected.platform_topology_generation == 0 ||
        expected.pci_fabric_generation == 0 || expected.compatibility_profile_hash == 0 || expected.segment > 0xffff ||
        expected.start_bus > expected.end_bus || expected.end_bus > 0xff)
        return root_feature_error::invalid_scope;

    if (evidence.root_id != expected.root_id || evidence.segment != expected.segment ||
        evidence.start_bus != expected.start_bus || evidence.end_bus != expected.end_bus)
        return root_feature_error::root_mismatch;
    if (evidence.boot_epoch != expected.boot_epoch || evidence.firmware_generation != expected.firmware_generation ||
        evidence.aml_namespace_generation != expected.aml_namespace_generation ||
        evidence.platform_topology_generation != expected.platform_topology_generation ||
        evidence.pci_fabric_generation != expected.pci_fabric_generation ||
        evidence.compatibility_profile_hash != expected.compatibility_profile_hash)
        return root_feature_error::stale_scope;

    if ((evidence.requested_support & ~policy.implemented_support_mask) != 0 ||
        (evidence.requested_control & ~policy.implemented_control_mask) != 0)
        return root_feature_error::unsupported_mask;
    if (evidence.query_errors != 0)
        return root_feature_error::query_failed;
    if (evidence.commit_errors != 0)
        return root_feature_error::commit_failed;
    if (evidence.query_support != evidence.requested_support || evidence.commit_support != evidence.requested_support)
        return root_feature_error::incomplete_negotiation;

    if ((evidence.query_granted_control & ~evidence.requested_control) != 0 ||
        evidence.masked_control != (evidence.requested_control & ~evidence.query_granted_control) ||
        (evidence.granted_control & ~evidence.query_granted_control) != 0 ||
        evidence.denied_control != (evidence.requested_control & ~evidence.granted_control))
        return root_feature_error::inconsistent_result;
    if (evidence.granted_control == 0)
        return root_feature_error::no_control_granted;

    return root_feature_error::none;
}
} // namespace pci
