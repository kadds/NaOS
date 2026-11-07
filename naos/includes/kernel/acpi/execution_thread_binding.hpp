#pragma once

#include "kernel/types.hpp"

namespace acpi::detail
{

struct executor_thread_binding
{
    u64 owner = 0;
    u64 thread_id = 0;
    // A worker/owner exit ends this boot's AML execution lineage. Rebinding
    // would silently replay methods whose hardware side effects may persist.
    bool revoked = false;
};

constexpr bool bind_executor_thread(executor_thread_binding &binding, u64 owner, bool registrar_is_main_thread,
                                    u64 thread_id) noexcept
{
    if (binding.revoked || owner == 0 || !registrar_is_main_thread || thread_id == 0)
        return false;
    if (binding.owner == 0 && binding.thread_id == 0)
    {
        binding.owner = owner;
        binding.thread_id = thread_id;
        return true;
    }
    return binding.owner == owner && binding.thread_id == thread_id;
}

constexpr bool executor_thread_matches(const executor_thread_binding &binding, u64 owner, u64 thread_id) noexcept
{
    return owner != 0 && thread_id != 0 && binding.owner == owner && binding.thread_id == thread_id;
}

constexpr bool executor_owner_matches(const executor_thread_binding &binding, u64 owner) noexcept
{
    return owner != 0 && binding.owner == owner && binding.thread_id != 0 && !binding.revoked;
}

/// Rust platform generations encode the kernel PID in the high word and a
/// process-global monotonic sequence in the low word. The platform topology
/// incarnation is allocated after the AML namespace incarnation.
constexpr bool platform_generations_match_owner(u64 owner, u64 aml_namespace_generation,
                                                u64 platform_topology_generation) noexcept
{
    constexpr u64 sequence_mask = 0xffffffffULL;
    return owner != 0 && owner <= sequence_mask && (aml_namespace_generation >> 32) == owner &&
           (platform_topology_generation >> 32) == owner && (aml_namespace_generation & sequence_mask) != 0 &&
           (platform_topology_generation & sequence_mask) > (aml_namespace_generation & sequence_mask);
}

constexpr void unbind_executor_thread(executor_thread_binding &binding, u64 owner, u64 thread_id) noexcept
{
    if (executor_thread_matches(binding, owner, thread_id))
    {
        binding.owner = 0;
        binding.thread_id = 0;
        binding.revoked = true;
    }
}

constexpr bool unbind_executor_owner(executor_thread_binding &binding, u64 owner) noexcept
{
    if (owner == 0 || binding.owner != owner)
        return false;

    binding.owner = 0;
    binding.thread_id = 0;
    binding.revoked = true;
    return true;
}

template <typename InvalidateTopology>
constexpr bool invalidate_topology_on_executor_owner_exit(
    bool owner_revoked, InvalidateTopology invalidate_topology) noexcept(noexcept(invalidate_topology()))
{
    if (!owner_revoked)
        return false;
    invalidate_topology();
    return true;
}

} // namespace acpi::detail
