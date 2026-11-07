#include "kernel/physical_resource.hpp"

#include "kernel/ucontext.hpp"

namespace physical_resource
{
namespace
{
constexpr u64 page_size = 4096;

bool add_overflows(u64 left, u64 right) { return right > ~u64(0) - left; }

bool valid_page_cover(u64 base, u64 length, u64 page_base, u64 page_end)
{
    if (length == 0 || add_overflows(base, length))
        return false;

    const u64 expected_page_base = base & ~(page_size - 1);
    const u64 end = base + length;
    const u64 end_offset = end & (page_size - 1);
    u64 expected_page_end = end;
    if (end_offset != 0)
    {
        const u64 padding = page_size - end_offset;
        if (add_overflows(end, padding))
            return false;
        expected_page_end += padding;
    }
    return page_base == expected_page_base && page_end == expected_page_end;
}

bool live(state value) { return value != state::revoked; }

bool access_is_attenuated(access parent, access child) { return parent == access::read_write || parent == child; }

bool access_covers(access parent, access requested) { return parent == access::read_write || parent == requested; }

bool same_shareable_range(const lease &left, const lease &right)
{
    return left.sharing_policy == sharing::shared_read_only && right.sharing_policy == sharing::shared_read_only &&
           left.mode == access::read && right.mode == access::read && left.base == right.base &&
           left.length == right.length && left.page_base == right.page_base && left.page_end == right.page_end &&
           left.resource_kind == right.resource_kind && left.cache_type == right.cache_type;
}

bool shared_read_only_ecam_pages_overlap(const lease &left, const lease &right)
{
    return left.resource_kind == kind::pci_ecam && right.resource_kind == kind::pci_ecam &&
           left.sharing_policy == sharing::shared_read_only && right.sharing_policy == sharing::shared_read_only &&
           left.mode == access::read && right.mode == access::read && left.cache_type == right.cache_type &&
           left.page_base < right.page_end && right.page_base < left.page_end;
}

bool same_address_domain(kind left, kind right)
{
    const bool left_io = left == kind::io_port;
    const bool right_io = right == kind::io_port;
    // Only I/O port numbers are a separate address domain; every other
    // resource kind refers to physical byte addresses.
    return left_io == right_io;
}

bool sharing_is_attenuated(const lease &parent, access child_mode, u64 child_owner, sharing child_sharing)
{
    // The kernel's top-level ECAM lease is writable only for the private
    // quiesce path. User descendants receive an explicitly shared read-only
    // view; preserving the parent's exclusive policy would leak config writes.
    if (parent.resource_kind == kind::pci_ecam && parent.parent == ledger::root_id() &&
        parent.owner == ledger::kernel_owner() && parent.mode == access::read_write &&
        parent.sharing_policy == sharing::exclusive)
    {
        if (child_owner == ledger::kernel_owner())
            return child_mode == access::read_write && child_sharing == sharing::exclusive;
        return child_mode == access::read && child_sharing == sharing::shared_read_only;
    }
    if (child_sharing == parent.sharing_policy)
        return true;
    return parent.resource_kind == kind::pci_ecam && parent.mode == access::read_write &&
           parent.sharing_policy == sharing::exclusive && child_mode == access::read &&
           child_sharing == sharing::shared_read_only;
}
} // namespace

ledger::ledger(freelibcxx::Allocator *allocator)
    : leases_(allocator)
{
    leases_.ensure(max_lease_count());
    capacity_ready_ = leases_.capacity() >= max_lease_count();
}

bool ledger::overlaps(const lease &candidate, const lease &existing) const
{
    if (!live(existing.lifecycle))
        return false;
    if (same_shareable_range(candidate, existing))
        return false;
    // Shared read-only ECAM windows and per-function config pages may overlap
    // safely when both leases explicitly opt into read-only sharing.
    if (shared_read_only_ecam_pages_overlap(candidate, existing))
        return false;
    // Port numbers and physical addresses are separate namespaces even when
    // their numeric ranges happen to match.
    if (!same_address_domain(candidate.resource_kind, existing.resource_kind))
        return false;
    if (candidate.parent != root_id() && is_descendant_locked(candidate.parent, existing.id))
        return false;
    const u64 candidate_end = candidate.base + candidate.length;
    const u64 existing_end = existing.base + existing.length;
    const bool logical_overlap = candidate.base < existing_end && existing.base < candidate_end;
    // I/O ports are scalar address-space units, not pages. Their page_cover
    // is bookkeeping only and must not make adjacent fixed blocks conflict.
    const bool io_port_range = candidate.resource_kind == kind::io_port && existing.resource_kind == kind::io_port;
    // A direct mapping exposes its complete page cover, so overlapping page
    // envelopes conflict even when the logical byte ranges do not overlap.
    const bool envelope_overlap =
        !io_port_range && candidate.page_base < existing.page_end && existing.page_base < candidate.page_end;
    return logical_overlap || envelope_overlap;
}

lease *ledger::find_locked(u64 id)
{
    for (auto &entry : leases_)
    {
        if (entry.id == id)
            return &entry;
    }
    return nullptr;
}

const lease *ledger::find_locked(u64 id) const
{
    for (const auto &entry : leases_)
    {
        if (entry.id == id)
            return &entry;
    }
    return nullptr;
}

bool ledger::is_descendant_locked(u64 candidate, u64 ancestor) const
{
    // The parent chain is normally short, but bound the walk by the number
    // of ledger entries so a corrupted cycle cannot turn revoke into an
    // unbounded kernel loop.
    for (size_t hops = 0; hops <= leases_.size(); ++hops)
    {
        if (candidate == ancestor)
            return true;
        const auto *entry = find_locked(candidate);
        if (entry == nullptr || entry->parent == 0 || entry->parent == candidate)
            return false;
        candidate = entry->parent;
    }
    return false;
}

bool ledger::has_descendant_locked(u64 ancestor) const
{
    for (const auto &entry : leases_)
    {
        if (entry.parent == ancestor)
            return true;
    }
    return false;
}

void ledger::prune_revoked_locked()
{
    for (size_t index = leases_.size(); index != 0;)
    {
        --index;
        const auto &entry = leases_[index];
        if (entry.lifecycle == state::revoked && entry.active_mappings == 0 && !has_descendant_locked(entry.id))
            leases_.remove_at(index);
    }
}

bool ledger::reserve(u64 parent, u64 base, u64 length, u64 page_base, u64 page_end, kind resource_kind,
                     cache cache_type, access mode, u64 generation, lease &result)
{
    return reserve(parent, base, length, page_base, page_end, resource_kind, cache_type, mode, kernel_owner(),
                   sharing::exclusive, generation, result);
}

bool ledger::reserve(const root &authority, u64 base, u64 length, u64 page_base, u64 page_end, kind resource_kind,
                     cache cache_type, access mode, u64 generation, lease &result)
{
    return reserve(authority, base, length, page_base, page_end, resource_kind, cache_type, mode, kernel_owner(),
                   sharing::exclusive, generation, result);
}

bool ledger::reserve(const root &authority, u64 base, u64 length, u64 page_base, u64 page_end, kind resource_kind,
                     cache cache_type, access mode, u64 owner, sharing sharing_policy, u64 generation, lease &result)
{
    if (authority.resources() != this)
        return false;
    return reserve_impl(root_id(), true, base, length, page_base, page_end, resource_kind, cache_type, mode, owner,
                        sharing_policy, generation, result);
}

bool ledger::reserve(u64 parent, u64 base, u64 length, u64 page_base, u64 page_end, kind resource_kind,
                     cache cache_type, access mode, u64 owner, sharing sharing_policy, u64 generation, lease &result)
{
    return reserve_impl(parent, false, base, length, page_base, page_end, resource_kind, cache_type, mode, owner,
                        sharing_policy, generation, result);
}

bool ledger::reserve_impl(u64 parent, bool root_authorized, u64 base, u64 length, u64 page_base, u64 page_end,
                          kind resource_kind, cache cache_type, access mode, u64 owner, sharing sharing_policy,
                          u64 generation, lease &result)
{
    if (parent == 0 || (parent == root_id() && !root_authorized) ||
        !valid_page_cover(base, length, page_base, page_end) ||
        (sharing_policy == sharing::shared_read_only && mode != access::read))
        return false;
    uctx::RawSpinLockUninterruptibleContext guard(lock_);
    if (!capacity_ready_ || leases_.size() >= max_lease_count() || next_id_ == 0)
        return false;
    lease candidate{next_id_, parent,         base, length,     page_base, page_end, resource_kind,  cache_type, mode,
                    owner,    sharing_policy, 0,    generation, 0,         0,        state::reserved};
    if (parent != root_id())
    {
        const auto *parent_lease = find_locked(parent);
        if (parent_lease == nullptr ||
            (parent_lease->lifecycle != state::leased && parent_lease->lifecycle != state::mapped) ||
            (parent_lease->owner != kernel_owner() && parent_lease->owner != owner))
            return false;
        const u64 parent_end = parent_lease->base + parent_lease->length;
        const u64 candidate_end = base + length;
        if (base < parent_lease->base || candidate_end > parent_end || page_base < parent_lease->page_base ||
            page_end > parent_lease->page_end || resource_kind != parent_lease->resource_kind ||
            cache_type != parent_lease->cache_type || !access_is_attenuated(parent_lease->mode, mode) ||
            !sharing_is_attenuated(*parent_lease, mode, owner, sharing_policy) ||
            generation != parent_lease->generation)
            return false;
        candidate.thread_binding = parent_lease->thread_binding;
        candidate.namespace_generation = parent_lease->namespace_generation;
        candidate.device_binding = parent_lease->device_binding;
    }
    for (const auto &entry : leases_)
    {
        if (overlaps(candidate, entry))
            return false;
    }
    leases_.push_back(candidate);
    result = candidate;
    next_id_++;
    return true;
}

bool ledger::transition(u64 id, state expected, state next) { return transition(id, expected, next, kernel_owner()); }

bool ledger::transition(u64 id, state expected, state next, u64 owner)
{
    if (expected == state::revoked || next == state::reserved ||
        (expected == state::reserved && next != state::leased) ||
        (expected == state::leased && next != state::mapped && next != state::revoking) ||
        (expected == state::mapped && next != state::revoking) ||
        (expected == state::revoking && next != state::revoked))
        return false;
    uctx::RawSpinLockUninterruptibleContext guard(lock_);
    auto *entry = find_locked(id);
    if (entry == nullptr || entry->lifecycle != expected || entry->owner != owner)
        return false;
    entry->lifecycle = next;
    return true;
}

bool ledger::map(u64 id, u64 base, u64 length, kind resource_kind, cache cache_type, access mode, u64 generation)
{
    return map(id, base, length, resource_kind, cache_type, mode, generation, kernel_owner());
}

bool ledger::map(u64 id, u64 base, u64 length, kind resource_kind, cache cache_type, access mode_value, u64 generation,
                 u64 owner)
{
    if (resource_kind == kind::io_port || length == 0 || add_overflows(base, length))
        return false;
    const u64 end = base + length;
    if (end > ~u64(0) - (page_size - 1))
        return false;
    const u64 page_base = base & ~(page_size - 1);
    const u64 page_end = (end + page_size - 1) & ~(page_size - 1);
    uctx::RawSpinLockUninterruptibleContext guard(lock_);
    auto *entry = find_locked(id);
    if (entry == nullptr || entry->lifecycle != state::leased || entry->resource_kind != resource_kind ||
        entry->cache_type != cache_type || entry->generation != generation || !access_covers(entry->mode, mode_value) ||
        (entry->owner != owner && (entry->owner != kernel_owner() || owner == kernel_owner())))
        return false;
    const u64 entry_end = entry->base + entry->length;
    if (base < entry->base || end > entry_end || page_base < entry->page_base || page_end > entry->page_end)
        return false;
    // A kernel-reserved root range is transferred to the first mapped
    // authority. This is the handoff used by the read-only ECAM discovery
    // path; it is still serialized with the mapping transition, so a second
    // process cannot race the handoff or reuse the kernel-owned lease.
    if (entry->owner == kernel_owner() && owner != kernel_owner())
        entry->owner = owner;
    entry->lifecycle = state::mapped;
    return true;
}

bool ledger::bind_thread(u64 id, u64 owner, u64 thread_id)
{
    if (owner == kernel_owner() || thread_id == 0)
        return false;
    uctx::RawSpinLockUninterruptibleContext guard(lock_);
    auto *entry = find_locked(id);
    if (entry == nullptr || entry->owner != owner || entry->resource_kind != kind::io_port ||
        (entry->lifecycle != state::reserved && entry->lifecycle != state::leased) ||
        (entry->thread_binding != 0 && entry->thread_binding != thread_id))
        return false;
    entry->thread_binding = thread_id;
    return true;
}

bool ledger::bind_namespace(u64 id, u64 owner, u64 namespace_generation)
{
    if (owner == kernel_owner() || namespace_generation == 0)
        return false;
    uctx::RawSpinLockUninterruptibleContext guard(lock_);
    auto *entry = find_locked(id);
    if (entry == nullptr || entry->owner != owner ||
        (entry->lifecycle != state::reserved && entry->lifecycle != state::leased) ||
        (entry->namespace_generation != 0 && entry->namespace_generation != namespace_generation))
        return false;
    entry->namespace_generation = namespace_generation;
    return true;
}

bool ledger::bind_device(u64 id, u64 owner, u64 device_id)
{
    if (owner == kernel_owner() || device_id == 0)
        return false;
    uctx::RawSpinLockUninterruptibleContext guard(lock_);
    auto *entry = find_locked(id);
    if (entry == nullptr || entry->owner != owner ||
        (entry->lifecycle != state::reserved && entry->lifecycle != state::leased) ||
        (entry->device_binding != 0 && entry->device_binding != device_id))
        return false;
    entry->device_binding = device_id;
    return true;
}

bool ledger::revoke(u64 id) { return revoke(id, kernel_owner()); }

bool ledger::revoke(u64 id, u64 owner)
{
    uctx::RawSpinLockUninterruptibleContext guard(lock_);
    auto *entry = find_locked(id);
    if (entry == nullptr)
        return id > root_id() && id < next_id_;
    if (entry->owner != owner)
        return false;
    if (entry->lifecycle == state::revoked)
        return true;
    if (entry->active_mappings != 0 || has_descendant_locked(id))
        return false;
    if (entry->lifecycle != state::reserved && entry->lifecycle != state::leased && entry->lifecycle != state::mapped &&
        entry->lifecycle != state::revoking)
        return false;
    entry->lifecycle = state::revoking;
    entry->lifecycle = state::revoked;
    prune_revoked_locked();
    return true;
}

bool ledger::revoke_kernel(u64 id)
{
    uctx::RawSpinLockUninterruptibleContext guard(lock_);
    auto *entry = find_locked(id);
    if (entry == nullptr)
        return id > root_id() && id < next_id_;
    if (entry->lifecycle == state::revoked)
        return true;
    if (entry->active_mappings != 0 || has_descendant_locked(id))
        return false;
    if (entry->lifecycle != state::reserved && entry->lifecycle != state::leased && entry->lifecycle != state::mapped &&
        entry->lifecycle != state::revoking)
        return false;
    entry->lifecycle = state::revoking;
    entry->lifecycle = state::revoked;
    prune_revoked_locked();
    return true;
}

bool ledger::revoke_dma_window(u64 physical_page_base, u64 generation)
{
    if (physical_page_base % page_size != 0 || physical_page_base > ~u64(0) - page_size || generation == 0)
        return false;

    uctx::RawSpinLockUninterruptibleContext guard(lock_);
    lease *match = nullptr;
    for (auto &entry : leases_)
    {
        if (entry.parent != root_id() || entry.owner != kernel_owner() || entry.base != physical_page_base ||
            entry.length != page_size || entry.page_base != physical_page_base ||
            entry.page_end != physical_page_base + page_size || entry.resource_kind != kind::dma_window ||
            entry.cache_type != cache::write_back || entry.mode != access::read_write ||
            entry.generation != generation || entry.lifecycle == state::revoked)
            continue;
        if (match != nullptr)
            return false;
        match = &entry;
    }
    if (match == nullptr || match->active_mappings != 0 || has_descendant_locked(match->id) ||
        (match->lifecycle != state::reserved && match->lifecycle != state::leased &&
         match->lifecycle != state::mapped && match->lifecycle != state::revoking))
        return false;

    match->lifecycle = state::revoking;
    match->lifecycle = state::revoked;
    prune_revoked_locked();
    return true;
}

bool ledger::begin_mapping(u64 id, u64 base, u64 length)
{
    if (length == 0 || add_overflows(base, length))
        return false;
    const u64 end = base + length;
    uctx::RawSpinLockUninterruptibleContext guard(lock_);
    auto *entry = find_locked(id);
    if (entry == nullptr || entry->lifecycle != state::mapped || entry->active_mappings == ~u64(0))
        return false;
    if (base < entry->page_base || end > entry->page_end)
        return false;
    entry->lifecycle = state::mapped;
    entry->active_mappings++;
    return true;
}

bool ledger::end_mapping(u64 id)
{
    uctx::RawSpinLockUninterruptibleContext guard(lock_);
    auto *entry = find_locked(id);
    if (entry == nullptr || entry->active_mappings == 0 ||
        (entry->lifecycle != state::mapped && entry->lifecycle != state::revoking))
        return false;
    entry->active_mappings--;
    if (entry->active_mappings == 0)
    {
        if (entry->lifecycle == state::revoking)
            entry->lifecycle = state::revoked;
        prune_revoked_locked();
    }
    return true;
}

bool ledger::unmap_kernel(u64 id)
{
    uctx::RawSpinLockUninterruptibleContext guard(lock_);
    auto *entry = find_locked(id);
    if (entry == nullptr)
        return id > root_id() && id < next_id_;
    if (entry->active_mappings != 0)
        return false;
    if (entry->lifecycle == state::revoked)
        return true;
    if (entry->lifecycle == state::leased)
        return true;
    if (entry->lifecycle == state::revoking)
    {
        if (has_descendant_locked(id))
            return false;
        entry->lifecycle = state::revoked;
        prune_revoked_locked();
        return true;
    }
    if (entry->lifecycle != state::mapped)
        return false;
    if (has_descendant_locked(id))
        return false;
    entry->lifecycle = state::revoked;
    prune_revoked_locked();
    return true;
}

bool ledger::revoke_owner(u64 owner)
{
    if (owner == kernel_owner())
        return false;
    uctx::RawSpinLockUninterruptibleContext guard(lock_);
    bool revoked = false;
    for (auto &entry : leases_)
    {
        if (entry.owner != owner || !live(entry.lifecycle))
            continue;
        entry.lifecycle = entry.active_mappings == 0 ? state::revoked : state::revoking;
        revoked = true;
    }
    prune_revoked_locked();
    return revoked;
}

bool ledger::revoke_thread(u64 owner, u64 thread_id)
{
    if (owner == kernel_owner() || thread_id == 0)
        return false;
    uctx::RawSpinLockUninterruptibleContext guard(lock_);
    bool revoked = false;
    for (auto &entry : leases_)
    {
        if (entry.owner != owner || entry.thread_binding != thread_id || !live(entry.lifecycle))
            continue;
        entry.lifecycle = entry.active_mappings == 0 ? state::revoked : state::revoking;
        revoked = true;
    }
    prune_revoked_locked();
    return revoked;
}

bool ledger::revoke_subtree(u64 id) { return revoke_subtree(id, kernel_owner()); }

bool ledger::revoke_subtree(u64 id, u64 owner)
{
    uctx::RawSpinLockUninterruptibleContext guard(lock_);
    const auto *root = find_locked(id);
    if (root == nullptr)
        return id > root_id() && id < next_id_;
    if (root->owner != owner)
        return false;

    // Validate every live descendant before changing any state. A reserved
    // child has not completed grant admission and cannot be silently turned
    // into a revoked lease as part of a mapping revoke.
    for (const auto &entry : leases_)
    {
        if (!live(entry.lifecycle) || !is_descendant_locked(entry.id, id))
            continue;
        if (entry.owner != owner)
            return false;
        if (entry.lifecycle == state::reserved || entry.active_mappings != 0)
            return false;
        if (entry.lifecycle != state::leased && entry.lifecycle != state::mapped && entry.lifecycle != state::revoking)
            return false;
    }

    // The two passes keep the externally visible lifecycle monotonic even
    // though the whole operation is serialized by one ledger transaction.
    for (auto &entry : leases_)
    {
        if (live(entry.lifecycle) && is_descendant_locked(entry.id, id))
            entry.lifecycle = state::revoking;
    }
    for (auto &entry : leases_)
    {
        if (entry.lifecycle == state::revoking && is_descendant_locked(entry.id, id))
            entry.lifecycle = state::revoked;
    }
    prune_revoked_locked();
    return true;
}

bool ledger::lookup(u64 id, lease &result) const
{
    uctx::RawSpinLockUninterruptibleContext guard(lock_);
    const auto *entry = find_locked(id);
    if (entry == nullptr)
        return false;
    result = *entry;
    return true;
}

bool ledger::covers(u64 id, u64 base, u64 length, kind resource_kind, cache cache_type, access mode,
                    u64 generation) const
{
    if (length == 0 || add_overflows(base, length))
        return false;
    uctx::RawSpinLockUninterruptibleContext guard(lock_);
    const auto *entry = find_locked(id);
    if (entry == nullptr || (entry->lifecycle != state::leased && entry->lifecycle != state::mapped) ||
        entry->resource_kind != resource_kind || entry->cache_type != cache_type || entry->generation != generation ||
        !access_covers(entry->mode, mode))
        return false;
    const u64 end = base + length;
    const u64 entry_end = entry->base + entry->length;
    return entry->base <= base && end <= entry_end;
}

} // namespace physical_resource
