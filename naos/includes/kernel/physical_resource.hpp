#pragma once

#include "freelibcxx/allocator.hpp"
#include "freelibcxx/vector.hpp"
#include "kernel/lock.hpp"
#include "kernel/types.hpp"

namespace physical_resource
{

class root;

enum class kind : u8
{
    kernel_reserved,
    acpi_fixed_hardware,
    iommu_register,
    firmware_snapshot,
    device_mmio,
    firmware_reserved,
    firmware_state,
    io_port,
    pci_ecam,
    pci_bar,
    dma_window,
};

enum class cache : u8
{
    uncacheable,
    write_back,
};

enum class access : u8
{
    read,
    read_write,
    access,
};

enum class sharing : u8
{
    exclusive,
    shared_read_only,
};

enum class state : u8
{
    reserved,
    leased,
    mapped,
    revoking,
    revoked,
};

struct lease
{
    u64 id = 0;
    u64 parent = 0;
    u64 base = 0;
    u64 length = 0;
    u64 page_base = 0;
    u64 page_end = 0;
    kind resource_kind = kind::kernel_reserved;
    cache cache_type = cache::uncacheable;
    access mode = access::read;
    u64 owner = 0;
    sharing sharing_policy = sharing::exclusive;
    u64 thread_binding = 0;
    u64 generation = 0;
    u64 namespace_generation = 0;
    u64 device_binding = 0;
    state lifecycle = state::reserved;
    u64 active_mappings = 0;
};

/// Kernel-owned physical resource ledger.  Physical addresses are accepted
/// only after the caller has established provenance; all overlap, page-cover,
/// cache and lifecycle checks are serialized here before a user mapping is
/// created.
class ledger
{
  public:
    static constexpr u64 root_id() { return 1; }
    static constexpr u64 kernel_owner() { return 0; }
    // Includes one lease for each live DMA physical page. The ledger vector is
    // backed by the kernel virtual allocator, not a fixed-size slab.
    static constexpr size_t max_lease_count() { return 512; }

    explicit ledger(freelibcxx::Allocator *allocator);

    bool reserve(const root &authority, u64 base, u64 length, u64 page_base, u64 page_end, kind resource_kind,
                 cache cache_type, access mode, u64 generation, lease &result);
    bool reserve(const root &authority, u64 base, u64 length, u64 page_base, u64 page_end, kind resource_kind,
                 cache cache_type, access mode, u64 owner, sharing sharing_policy, u64 generation, lease &result);
    /// Derived leases name their parent lease ID. Passing root_id() here is
    /// rejected; top-level reservations must present the kernel root object.
    bool reserve(u64 parent, u64 base, u64 length, u64 page_base, u64 page_end, kind resource_kind, cache cache_type,
                 access mode, u64 generation, lease &result);
    /// Derive a child only from a leased/mapped parent. The child's logical
    /// range and page cover must be contained; kind/cache/generation and
    /// inherited bindings cannot be widened. The one sharing transition is a
    /// kernel-owned writable/exclusive ECAM root to either a kernel-only
    /// writable child or a user-owned shared read-only child.
    bool reserve(u64 parent, u64 base, u64 length, u64 page_base, u64 page_end, kind resource_kind, cache cache_type,
                 access mode, u64 owner, sharing sharing_policy, u64 generation, lease &result);
    bool map(u64 id, u64 base, u64 length, kind resource_kind, cache cache_type, access mode, u64 generation);
    bool map(u64 id, u64 base, u64 length, kind resource_kind, cache cache_type, access mode, u64 generation,
             u64 owner);
    /// Revoke a lease at any post-admission state. Repeated revoke calls are
    /// idempotent so a failed grant, IPC delivery, and eventual object
    /// destruction can close the same transaction safely. An owner-facing
    /// revoke refuses a lease while any direct VMA mapping is live.
    bool revoke(u64 id);
    bool revoke(u64 id, u64 owner);
    /// Revoke from a kernel-owned capability destructor. This path is not a
    /// caller-facing authority; the capability object already owns the lease
    /// lifecycle and supplies the opaque id directly from its destructor.
    bool revoke_kernel(u64 id);
    /// Retire the unique kernel-owned DMA page lease after the DMA domain has
    /// confirmed the page is no longer reachable by the device.
    bool revoke_dma_window(u64 physical_page_base, u64 generation);
    /// Atomically pin a live physical lease before publishing a direct VMA.
    /// Revocation and new mappings are serialized by the ledger lock.
    bool begin_mapping(u64 id, u64 base, u64 length);
    /// Release one mapping after page-table removal and TLB shootdown. The
    /// last mapping leaves a live map lease mapped, or completes a pending
    /// owner revoke.
    bool end_mapping(u64 id);
    /// Close a direct map lease after its object has no VMA mappings. This
    /// also completes a pending owner revoke or failed grant transaction.
    bool unmap_kernel(u64 id);
    /// Revoke every live lease owned by a process during teardown. The
    /// caller is the kernel lifecycle path, not a user-visible authority.
    bool revoke_owner(u64 owner);
    /// Revoke thread-bound leases when their execution thread exits or is
    /// replaced by exec. This is kernel lifecycle bookkeeping, not a
    /// user-visible authority.
    bool revoke_thread(u64 owner, u64 thread_id);
    /// Atomically revoke a lease and every live descendant derived from it.
    /// Admission is validated for the complete subtree before any state is
    /// changed, so a reserved child cannot leave a partially revoked tree.
    bool revoke_subtree(u64 id);
    bool revoke_subtree(u64 id, u64 owner);
    bool transition(u64 id, state expected, state next);
    bool transition(u64 id, state expected, state next, u64 owner);
    bool bind_thread(u64 id, u64 owner, u64 thread_id);
    bool bind_namespace(u64 id, u64 owner, u64 namespace_generation);
    bool bind_device(u64 id, u64 owner, u64 device_id);
    bool lookup(u64 id, lease &result) const;
    bool covers(u64 id, u64 base, u64 length, kind resource_kind, cache cache_type, access mode, u64 generation) const;

  private:
    bool overlaps(const lease &candidate, const lease &existing) const;
    bool reserve_impl(u64 parent, bool root_authorized, u64 base, u64 length, u64 page_base, u64 page_end,
                      kind resource_kind, cache cache_type, access mode, u64 owner, sharing sharing_policy,
                      u64 generation, lease &result);
    bool is_descendant_locked(u64 candidate, u64 ancestor) const;
    bool has_descendant_locked(u64 ancestor) const;
    void prune_revoked_locked();
    lease *find_locked(u64 id);
    const lease *find_locked(u64 id) const;

    mutable lock::spinlock_t lock_;
    freelibcxx::vector<lease> leases_;
    bool capacity_ready_ = false;
    u64 next_id_ = root_id() + 1;
};

/// Kernel-only authority root for the physical-resource ledger. Top-level
/// physical leases must be admitted through this object; it is never
/// registered in ServiceDirectory. User-visible hardware capabilities are
/// derived by kernel adapters after checking this root's ledger.
class root
{
  public:
    explicit root(ledger *ledger)
        : ledger_(ledger)
    {
    }

    ledger *resources() const { return ledger_; }
    static constexpr u64 resource_id() { return ledger::root_id(); }

  private:
    ledger *ledger_;
};

} // namespace physical_resource
