#pragma once

#include "kernel/types.hpp"

namespace pci
{
inline constexpr u32 driver_profile_read_config = 1u << 0;
inline constexpr u32 driver_profile_dma_domain = 1u << 1;
inline constexpr u32 driver_profile_route_interrupt = 1u << 2;
inline constexpr u32 driver_profile_known_rights =
    driver_profile_read_config | driver_profile_dma_domain | driver_profile_route_interrupt;

inline constexpr u32 pci_function_bar_count = 6;
inline constexpr u32 driver_profile_known_bar_mask = (1u << pci_function_bar_count) - 1;

struct driver_function_profile
{
    u32 rights = 0;
    u32 bar_mask = 0;
};

enum class driver_profile_error : u8
{
    none,
    unknown_rights,
    unsupported_bar,
    bars_require_attestation,
};

enum class driver_target_error : u8
{
    none,
    invalid_process,
    same_process,
    already_started,
    exited,
};

constexpr driver_profile_error validate_driver_function_profile(driver_function_profile profile,
                                                                bool has_device_resource_attestation) noexcept
{
    if ((profile.rights & ~driver_profile_known_rights) != 0)
        return driver_profile_error::unknown_rights;
    if ((profile.bar_mask & ~driver_profile_known_bar_mask) != 0)
        return driver_profile_error::unsupported_bar;
    if (profile.bar_mask != 0 && !has_device_resource_attestation)
        return driver_profile_error::bars_require_attestation;
    return driver_profile_error::none;
}

constexpr bool driver_profile_allows_bar(driver_function_profile profile, u32 index) noexcept
{
    return index < pci_function_bar_count && (profile.bar_mask & (1u << index)) != 0;
}

constexpr driver_target_error validate_driver_process_target(u64 caller_pid, u64 target_pid, bool target_started,
                                                             bool target_exited) noexcept
{
    if (caller_pid == 0 || target_pid == 0)
        return driver_target_error::invalid_process;
    if (caller_pid == target_pid)
        return driver_target_error::same_process;
    if (target_exited)
        return driver_target_error::exited;
    if (target_started)
        return driver_target_error::already_started;
    return driver_target_error::none;
}
} // namespace pci
