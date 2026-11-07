#pragma once

namespace memory::vm::fork_policy
{

/// Physical resource mappings are process-scoped authority. Fork cannot
/// duplicate that authority through the page table when the backing handle
/// itself is deliberately non-duplicable.
constexpr bool can_inherit_mapping(bool direct_physical_mapping) noexcept { return !direct_physical_mapping; }

} // namespace memory::vm::fork_policy
