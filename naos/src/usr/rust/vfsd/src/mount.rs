//! Mount topology, the shared reservation table and both ticket state
//! machines (doc/VFS_BLOCK_DEVICE_ADR.md §6.2-§6.4, r7 amendments).
//!
//! This module is pure namespace bookkeeping: it owns no endpoints and no
//! backends, so every rule is host-unit-testable. The endpoint-facing glue
//! lives in [`crate::server`]; RAM-backend instances are keyed by mount id
//! there.
//!
//! r7 rules implemented here:
//! * `prepare_mount` reserves `{owner mount, parent_dir, last component}` in
//!   the table shared with mutations and issues a `PREPARED` ticket; no
//!   NamespaceBinding is pre-issued.
//! * `MountTicket.commit {root NodeKey, generation}` is the only publish
//!   point: `PREPARED → COMMITTING → COMMITTED`; expiry runs during
//!   `PREPARED` only, so `COMMITTING` can never observe `EXPIRED`.
//! * `MutationTicket` shares the canonical state table including
//!   `COMMITTING` (entered when the commit call is admitted), which closes
//!   the "local commit done, reservation timed out" race.
//! * unmount follows `ACTIVE → DRAINING → SYNCING → DETACHED → SHUTDOWN`;
//!   anchors are *not* counted busy (they live in the server layer and are
//!   invalidated at DRAINING), children always detach before parents, and a
//!   failed drain/sync returns the mount to `ACTIVE`.

use alloc::collections::BTreeMap;
use alloc::vec::Vec;
use core::cmp::Ordering;

use crate::errno::Errno;
use crate::internal::NodeKey;

/// The RAM backend serving `/` is mount 0 and can never be unmounted.
pub const ROOT_MOUNT_ID: u64 = 0;

/// Reservation/ticket timeout in driver-defined ticks. Callers advance the
/// clock explicitly (`poll_expiry(now)`), which keeps host tests
/// deterministic; the live server supplies `servicekit::monotonic_ticks()`.
// Production `monotonic_ticks` is expressed in microseconds.  Formatting a
// fresh FAT volume legitimately takes seconds, so the reservation window is
// 30 seconds rather than the old 30-millisecond host-test placeholder.
pub const TICKET_TIMEOUT_TICKS: u64 = 30_000_000;

/// Canonical wire state table (`idl/system/mount_ticket.naidl`), shared by
/// `MountTicket.status` and `MutationTicket.status`.
pub mod ticket_state {
    pub const PREPARED: u32 = 0;
    pub const COMMITTING: u32 = 1;
    pub const COMMITTED: u32 = 2;
    pub const ABORTED: u32 = 3;
    pub const EXPIRED: u32 = 4;
}

/// Explicit mount lifecycle (r7 ④). `SHUTDOWN` is modelled by record
/// removal: after [`Topology::detach`] the record is gone.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum MountState {
    Active,
    Draining,
    Syncing,
    Detached,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum TicketState {
    Prepared,
    Committing,
    Committed,
    Aborted,
    Expired,
}

impl TicketState {
    pub fn wire(self) -> u32 {
        match self {
            TicketState::Prepared => ticket_state::PREPARED,
            TicketState::Committing => ticket_state::COMMITTING,
            TicketState::Committed => ticket_state::COMMITTED,
            TicketState::Aborted => ticket_state::ABORTED,
            TicketState::Expired => ticket_state::EXPIRED,
        }
    }

    /// Terminal states accept no further transitions; `status` keeps
    /// reporting them idempotently.
    pub fn is_terminal(self) -> bool {
        matches!(
            self,
            TicketState::Committed | TicketState::Aborted | TicketState::Expired
        )
    }
}

/// Wire-identical mirror of `Vfs.MountInfo` / `MountTicket.MountInfo`.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct MountInfo {
    pub mount_id: u64,
    pub namespace_generation: u64,
    pub flags: u64,
    pub backend_generation: u64,
    pub device_id: u64,
}

/// One mounted filesystem instance. The root record represents the RAM
/// backend itself (`parent = None`, `mountpoint` unused).
#[derive(Clone, Debug)]
pub struct MountRecord {
    pub parent: Option<u64>,
    /// Covering node inside the *parent* backend (zero for the root).
    pub mountpoint: NodeKey,
    /// Last component under which this mount was attached; used for
    /// `path`/`getcwd` reconstruction along the parent_anchor chain (§6.3).
    pub mountpoint_name: Vec<u8>,
    pub state: MountState,
    /// Effective `prepare_mount` flags echoed by commit (v1: zero|READ_ONLY).
    pub flags: u64,
    pub backend_generation: u64,
    pub device_id: u64,
    pub root_node: NodeKey,
}

/// Resolution result handed to [`Topology::prepare`] by the caller, which
/// performed the owning-mount `lookup_target` under its metadata lock.
#[derive(Clone, Debug)]
pub struct TargetResolution {
    pub owner_mount: u64,
    pub parent_dir: NodeKey,
    /// The resolved directory that will be covered by the new mount.
    pub node: NodeKey,
    pub name: Vec<u8>,
}

#[derive(Clone, Debug, Eq, PartialEq)]
struct ResKey {
    mount: u64,
    parent: NodeKey,
    name: Vec<u8>,
}

impl PartialOrd for ResKey {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}

impl Ord for ResKey {
    fn cmp(&self, other: &Self) -> Ordering {
        self.mount
            .cmp(&other.mount)
            .then_with(|| self.parent.cmp(&other.parent))
            .then_with(|| self.name.cmp(&other.name))
    }
}

struct MountTicketRec {
    state: TicketState,
    target: TargetResolution,
    /// Effective `prepare_mount` flags echoed by the published MountInfo.
    flags: u64,
    prepared_at: u64,
}

struct MutationTicketRec {
    state: TicketState,
    /// One entry per affected `(parent_dir, component)` pair (two at most:
    /// source side and destination side).
    keys: [ResKey; 2],
    key_count: usize,
    prepared_at: u64,
}

/// Whole-namespace topology owned by the `Vfs` service.
pub struct Topology {
    /// Epoch identifier rejecting stale bindings after a vfsd restart (§6.3).
    pub namespace_instance: u64,
    namespace_generation: u64,
    next_mount_id: u64,
    next_device_id: u64,
    next_ticket_id: u64,
    mounts: BTreeMap<u64, MountRecord>,
    /// Shared between prepare_mount and mutations (ADR §6.2 rule 3).
    reservations: BTreeMap<ResKey, u64>,
    mount_tickets: BTreeMap<u64, MountTicketRec>,
    mutation_tickets: BTreeMap<u64, MutationTicketRec>,
}

impl Topology {
    pub fn new(namespace_instance: u64) -> Self {
        let mut topology = Topology {
            namespace_instance,
            namespace_generation: 1,
            next_mount_id: ROOT_MOUNT_ID + 1,
            next_device_id: 1,
            next_ticket_id: 1,
            mounts: BTreeMap::new(),
            reservations: BTreeMap::new(),
            mount_tickets: BTreeMap::new(),
            mutation_tickets: BTreeMap::new(),
        };
        topology.mounts.insert(
            ROOT_MOUNT_ID,
            MountRecord {
                parent: None,
                mountpoint: NodeKey::ZERO,
                mountpoint_name: Vec::new(),
                state: MountState::Active,
                flags: 0,
                backend_generation: 1,
                device_id: topology.next_device_id,
                root_node: NodeKey::ZERO,
            },
        );
        topology
    }

    pub fn namespace_generation(&self) -> u64 {
        self.namespace_generation
    }

    pub fn mount(&self, mount_id: u64) -> Option<&MountRecord> {
        self.mounts.get(&mount_id)
    }

    pub fn root(&self) -> &MountRecord {
        self.mounts
            .get(&ROOT_MOUNT_ID)
            .expect("root always present")
    }

    /// Child mount currently covering `mountpoint` in `parent_mount`.
    ///
    /// Traversal visibility: only `ACTIVE` children are returned. A
    /// `DRAINING` mount is invisible to routing (r7: traversal stops at
    /// DRAINING), which makes `enter_child_mount` report plain `ENOENT`.
    pub fn child_at(&self, parent_mount: u64, mountpoint: NodeKey) -> Option<u64> {
        self.mounts
            .values()
            .find(|record| {
                record.parent == Some(parent_mount)
                    && record.mountpoint == mountpoint
                    && record.state == MountState::Active
            })
            .map(|record| self.mount_id_of(record))
    }

    fn mount_id_of(&self, _record: &MountRecord) -> u64 {
        // Records are unique by (parent, mountpoint, root); find the id by
        // scanning -- fine at v1 scale and avoids storing ids twice.
        self.mounts
            .iter()
            .find(|(_, candidate)| core::ptr::eq(*candidate, _record))
            .map(|(id, _)| *id)
            .expect("record belongs to this table")
    }

    /// Live child mounts of `mount_id` (any pre-detach state).
    pub fn children_of(&self, mount_id: u64) -> Vec<u64> {
        self.mounts
            .iter()
            .filter(|(_, record)| record.parent == Some(mount_id))
            .map(|(id, _)| *id)
            .collect()
    }

    fn alloc_ticket(&mut self) -> u64 {
        let id = self.next_ticket_id;
        self.next_ticket_id += 1;
        id
    }

    // -----------------------------------------------------------------------
    // prepare_mount / MountTicket (§6.2, r7)
    // -----------------------------------------------------------------------

    /// Reserve `target` and issue a `PREPARED` MountTicket.
    ///
    /// Errors: `ENOENT` unknown/drained owner mount, `EBUSY` when the last
    /// component is already covered (active mount or in-flight reservation),
    /// or the covering node is itself a mountpoint (v1 forbids stacking).
    pub fn prepare(
        &mut self,
        target: TargetResolution,
        flags: u64,
        now: u64,
    ) -> Result<u64, Errno> {
        let owner = self.mounts.get(&target.owner_mount).ok_or(Errno::ENoent)?;
        if owner.state != MountState::Active {
            return Err(Errno::ENoent);
        }
        // No over-mounting: the covering node must not already carry or be
        // underneath another mount.
        if self.child_at(target.owner_mount, target.node).is_some()
            || self.is_below_mount(target.owner_mount, target.node)
        {
            return Err(Errno::EBusy);
        }
        let key = ResKey {
            mount: target.owner_mount,
            parent: target.parent_dir,
            name: target.name.clone(),
        };
        if self.reservations.contains_key(&key) {
            return Err(Errno::EBusy);
        }
        let ticket = self.alloc_ticket();
        self.reservations.insert(key, ticket);
        self.mount_tickets.insert(
            ticket,
            MountTicketRec {
                state: TicketState::Prepared,
                target,
                flags,
                prepared_at: now,
            },
        );
        Ok(ticket)
    }

    /// Does `node` in `mount` sit strictly below any mountpoint of `mount`?
    /// v1 records keep no subtree keys, so this conservatively reports true
    /// only when the node IS a mountpoint of a child mount.
    fn is_below_mount(&self, mount: u64, node: NodeKey) -> bool {
        self.children_of(mount)
            .into_iter()
            .filter_map(|child| self.mount(child).map(|record| record.mountpoint))
            .any(|mountpoint| mountpoint == node && !mountpoint.is_zero())
    }

    /// Admit `commit {root_node, root_generation}`: the only publish point.
    ///
    /// Enters `COMMITTING` first (freezing expiry), then validates and
    /// publishes. Wrong-state losers get `EINVAL` and must reconcile via
    /// `status` instead of retrying (ADR §6.2).
    pub fn commit_mount(
        &mut self,
        ticket: u64,
        root_node: NodeKey,
        root_generation: u64,
    ) -> Result<MountInfo, Errno> {
        let rec = self.mount_tickets.get_mut(&ticket).ok_or(Errno::EInval)?;
        if rec.state != TicketState::Prepared {
            return Err(Errno::EInval);
        }
        rec.state = TicketState::Committing;
        let target_name = rec.target.name.clone();
        let owner = rec.target.owner_mount;
        let parent_dir = rec.target.parent_dir;
        let mountpoint = rec.target.node;
        let flags = rec.flags;
        let key = ResKey {
            mount: owner,
            parent: parent_dir,
            name: target_name,
        };
        // The reservation must still be ours (it can only have been released
        // together with this ticket's expiry, which would have changed the
        // state checked above, so this is belt-and-braces).
        if self.reservations.get(&key) != Some(&ticket) {
            if let Some(rec) = self.mount_tickets.get_mut(&ticket) {
                rec.state = TicketState::Expired;
            }
            return Err(Errno::EInval);
        }
        if root_generation == 0 {
            // A worker never mints generation 0; reject before publishing.
            self.release_mount_ticket(ticket, TicketState::Aborted);
            return Err(Errno::EInval);
        }
        // NodeKey generation is worker-local.  The root identity must agree
        // with the generation carried by this commit, but equal generations
        // across independent worker mounts remain valid; backend_generation
        // is diagnostic metadata, not a global capability namespace.
        if root_node.generation != root_generation {
            self.release_mount_ticket(ticket, TicketState::Aborted);
            return Err(Errno::EInval);
        }
        self.reservations.remove(&key);
        let mount_id = self.next_mount_id;
        self.next_mount_id += 1;
        let device_id = self.next_device_id + 1;
        self.next_device_id = device_id;
        self.namespace_generation += 1;
        let info = MountInfo {
            mount_id,
            namespace_generation: self.namespace_generation,
            flags,
            backend_generation: root_generation,
            device_id,
        };
        self.mounts.insert(
            mount_id,
            MountRecord {
                parent: Some(owner),
                mountpoint,
                mountpoint_name: self
                    .mount_tickets
                    .get(&ticket)
                    .map(|rec| rec.target.name.clone())
                    .unwrap_or_default(),
                state: MountState::Active,
                flags,
                backend_generation: root_generation,
                device_id,
                root_node,
            },
        );
        if let Some(rec) = self.mount_tickets.get_mut(&ticket) {
            rec.state = TicketState::Committed;
        }
        Ok(info)
    }

    /// Roll back a just-published mount when the post-commit worker handshake
    /// cannot be completed.  Publication is still the sole topology change
    /// point, but a failed bind must not leave an ACTIVE mount that has no
    /// usable root capability.  The ticket remains as a terminal ABORTED
    /// record so callers can reconcile the failed commit deterministically.
    pub fn rollback_published_mount(&mut self, ticket: u64, mount_id: u64) -> Result<(), Errno> {
        let state = self
            .mount_tickets
            .get(&ticket)
            .map(|record| record.state)
            .ok_or(Errno::EInval)?;
        if state != TicketState::Committed {
            return Err(Errno::EInval);
        }
        let record = self.mounts.get(&mount_id).ok_or(Errno::ENoent)?;
        if record.state != MountState::Active {
            return Err(Errno::EInval);
        }
        if !self.children_of(mount_id).is_empty() {
            return Err(Errno::EBusy);
        }
        self.invalidate_mount_tickets(mount_id);
        self.mounts.remove(&mount_id);
        self.namespace_generation += 1;
        if let Some(record) = self.mount_tickets.get_mut(&ticket) {
            record.state = TicketState::Aborted;
        }
        Ok(())
    }

    /// Abort a still-`PREPARED` ticket, releasing the reservation.
    pub fn abort_mount(&mut self, ticket: u64) -> Result<(), Errno> {
        let Some(rec) = self.mount_tickets.get(&ticket) else {
            return Err(Errno::EInval);
        };
        match rec.state {
            TicketState::Prepared => {
                self.release_mount_ticket(ticket, TicketState::Aborted);
                Ok(())
            }
            // Already admitted: the mount may publish any moment; abort lost
            // the race and must reconcile via status.
            TicketState::Committing | TicketState::Committed | TicketState::Aborted => {
                Err(Errno::EInval)
            }
            TicketState::Expired => Err(Errno::EInval),
        }
    }

    fn release_mount_ticket(&mut self, ticket: u64, terminal: TicketState) {
        // Terminal records stay in the table so an idempotent `status` can
        // reconcile an OUTCOME_UNKNOWN after the fact (ADR §6.2).
        if let Some(rec) = self.mount_tickets.get_mut(&ticket) {
            let key = ResKey {
                mount: rec.target.owner_mount,
                parent: rec.target.parent_dir,
                name: rec.target.name.clone(),
            };
            if self.reservations.get(&key) == Some(&ticket) {
                self.reservations.remove(&key);
            }
            rec.state = terminal;
        }
    }

    /// Idempotent reconciliation query.
    pub fn mount_status(&self, ticket: u64) -> Result<TicketState, Errno> {
        self.mount_tickets
            .get(&ticket)
            .map(|rec| rec.state)
            .ok_or(Errno::EInval)
    }

    /// Expire one prepared mount ticket when its peer disappears.  This is
    /// deliberately ticket-scoped: a dead worker/control endpoint must not
    /// release unrelated reservations in the same owning mount.
    pub fn expire_mount_ticket(&mut self, ticket: u64) -> bool {
        if self.mount_tickets.get(&ticket).map(|rec| rec.state) == Some(TicketState::Prepared) {
            self.release_mount_ticket(ticket, TicketState::Expired);
            true
        } else {
            false
        }
    }

    // -----------------------------------------------------------------------
    // Expiry (runs only in PREPARED; §6.2 r7 ③)
    // -----------------------------------------------------------------------

    /// Expire every `PREPARED` ticket older than [`TICKET_TIMEOUT_TICKS`],
    /// releasing its reservations. Returns the number expired.
    pub fn poll_expiry(&mut self, now: u64) -> usize {
        let mut expired = Vec::new();
        for (id, rec) in self.mount_tickets.iter() {
            if rec.state == TicketState::Prepared
                && now.saturating_sub(rec.prepared_at) >= TICKET_TIMEOUT_TICKS
            {
                expired.push(*id);
            }
        }
        for (id, rec) in self.mutation_tickets.iter() {
            if rec.state == TicketState::Prepared
                && now.saturating_sub(rec.prepared_at) >= TICKET_TIMEOUT_TICKS
            {
                expired.push(*id);
            }
        }
        let count = expired.len();
        for id in expired {
            if let Some(state) = self.mount_tickets.get(&id).map(|rec| rec.state) {
                if state == TicketState::Prepared {
                    self.release_mount_ticket(id, TicketState::Expired);
                }
                continue;
            }
            if self.mutation_tickets.get(&id).map(|rec| rec.state) == Some(TicketState::Prepared) {
                self.release_mutation_ticket(id, TicketState::Expired);
            }
        }
        count
    }

    /// Invalidate every open ticket belonging to `mount_id` (unmount
    /// completion, media failure, control-channel loss; §6.2/§7).
    pub fn invalidate_mount_tickets(&mut self, mount_id: u64) -> usize {
        let dead: Vec<u64> = self
            .mount_tickets
            .iter()
            .filter(|(_, rec)| rec.target.owner_mount == mount_id && !rec.state.is_terminal())
            .map(|(id, _)| *id)
            .chain(
                self.mutation_tickets
                    .iter()
                    .filter(|(_, rec)| {
                        rec.keys[..rec.key_count]
                            .iter()
                            .any(|key| key.mount == mount_id)
                            && !rec.state.is_terminal()
                    })
                    .map(|(id, _)| *id),
            )
            .collect();
        let count = dead.len();
        for id in dead {
            if self.mount_tickets.contains_key(&id) {
                self.release_mount_ticket(id, TicketState::Expired);
            } else {
                self.release_mutation_ticket(id, TicketState::Expired);
            }
        }
        count
    }

    // -----------------------------------------------------------------------
    // Mutations (§6.4 begin_mutation rules)
    // -----------------------------------------------------------------------

    /// Validate the zero-field convention for `operation` and reserve all
    /// affected `(parent_dir, component)` pairs.
    ///
    /// `RENAME`/`LINK` require all four nodes; `SYMLINK`/`CREATE`/`MKDIR`
    /// only the `new_*` side; `UNLINK`/`RMDIR` only the `old_*` side.
    /// Unknown operations and conflicting reservations fail before any
    /// ticket exists.
    #[allow(clippy::too_many_arguments)]
    pub fn begin_mutation(
        &mut self,
        mount: u64,
        operation: u32,
        old_parent: NodeKey,
        old_target: NodeKey,
        new_parent: NodeKey,
        new_target: NodeKey,
        old_name: &[u8],
        new_name: &[u8],
        now: u64,
    ) -> Result<u64, Errno> {
        use crate::internal::namespace_binding as ns;
        let (need_old, need_new) = match operation {
            ns::OP_RENAME | ns::OP_LINK => (true, true),
            ns::OP_SYMLINK | ns::OP_CREATE | ns::OP_MKDIR => (false, true),
            ns::OP_UNLINK | ns::OP_RMDIR => (true, false),
            _ => return Err(Errno::EInval),
        };
        let mount_generation = self
            .mounts
            .get(&mount)
            .filter(|record| record.state == MountState::Active)
            .map(|record| record.backend_generation)
            .ok_or(Errno::ENoent)?;
        for node in [old_parent, old_target, new_parent, new_target] {
            if !node.is_zero() && node.generation != mount_generation {
                return Err(Errno::EInval);
            }
        }
        let old_side_ok = old_parent != NodeKey::ZERO && !old_name.is_empty();
        let new_side_ok = new_parent != NodeKey::ZERO && !new_name.is_empty();
        if need_old && !old_side_ok {
            return Err(Errno::EInval);
        }
        if need_new && !new_side_ok {
            return Err(Errno::EInval);
        }
        if !need_old {
            // create-family ops must not carry old_* fields at all.
            if old_parent != NodeKey::ZERO || !old_name.is_empty() {
                return Err(Errno::EInval);
            }
        }
        if !need_new {
            // unlink/rmdir must not carry new_* fields.
            if new_parent != NodeKey::ZERO || new_target != NodeKey::ZERO || !new_name.is_empty() {
                return Err(Errno::EInval);
            }
        }
        // In the embedded-worker layout every NodeKey belongs to exactly one
        // backend keyed by mount id; the server layer resolves that mapping
        // and passes consistent keys, so here we only guard against mixing
        // sides of different mounts.
        let mut keys: [ResKey; 2] = [
            ResKey {
                mount: 0,
                parent: NodeKey::ZERO,
                name: Vec::new(),
            },
            ResKey {
                mount: 0,
                parent: NodeKey::ZERO,
                name: Vec::new(),
            },
        ];
        let mut key_count = 0usize;
        if need_old {
            keys[key_count] = ResKey {
                mount,
                parent: old_parent,
                name: Vec::from(old_name),
            };
            key_count += 1;
        }
        if need_new {
            keys[key_count] = ResKey {
                mount,
                parent: new_parent,
                name: Vec::from(new_name),
            };
            key_count += 1;
        }
        for key in &keys[..key_count] {
            if self.reservations.contains_key(key) {
                return Err(Errno::EBusy);
            }
        }
        // Mutations may not touch mountpoints: the covering node of any
        // active child is protected by the reservation table.
        for child in self.children_of(mount) {
            let Some(record) = self.mount(child) else {
                continue;
            };
            let protected = [record.mountpoint, record.root_node];
            if protected.contains(&old_target) && !old_target.is_zero() {
                return Err(Errno::EBusy);
            }
            if protected.contains(&new_target) && !new_target.is_zero() {
                return Err(Errno::EBusy);
            }
        }
        let ticket = self.alloc_ticket();
        for key in &keys[..key_count] {
            self.reservations.insert(key.clone(), ticket);
        }
        self.mutation_tickets.insert(
            ticket,
            MutationTicketRec {
                state: TicketState::Prepared,
                keys,
                key_count,
                prepared_at: now,
            },
        );
        Ok(ticket)
    }

    /// Admit a mutation commit: `PREPARED → COMMITTING`, freezing expiry
    /// while the worker finishes its local metadata commit (r7 ③).
    pub fn admit_mutation(&mut self, ticket: u64) -> Result<(), Errno> {
        let rec = self
            .mutation_tickets
            .get_mut(&ticket)
            .ok_or(Errno::EInval)?;
        if rec.state != TicketState::Prepared {
            return Err(Errno::EInval);
        }
        rec.state = TicketState::Committing;
        Ok(())
    }

    /// Finish an admitted mutation (`COMMITTED` when the local commit was
    /// applied, `ABORTED` when the worker rolled back before any effect).
    pub fn finish_mutation(&mut self, ticket: u64, committed: bool) -> Result<(), Errno> {
        let Some(rec) = self.mutation_tickets.get(&ticket) else {
            return Err(Errno::EInval);
        };
        if rec.state != TicketState::Committing {
            return Err(Errno::EInval);
        }
        self.release_mutation_ticket(
            ticket,
            if committed {
                TicketState::Committed
            } else {
                TicketState::Aborted
            },
        );
        Ok(())
    }

    /// Abort before any local side effect.
    pub fn abort_mutation(&mut self, ticket: u64) -> Result<(), Errno> {
        let Some(rec) = self.mutation_tickets.get(&ticket) else {
            return Err(Errno::EInval);
        };
        if rec.state != TicketState::Prepared {
            return Err(Errno::EInval);
        }
        self.release_mutation_ticket(ticket, TicketState::Aborted);
        Ok(())
    }

    pub fn mutation_status(&self, ticket: u64) -> Result<TicketState, Errno> {
        self.mutation_tickets
            .get(&ticket)
            .map(|rec| rec.state)
            .ok_or(Errno::EInval)
    }

    /// Expire one prepared mutation reservation after its worker peer closes.
    pub fn expire_mutation_ticket(&mut self, ticket: u64) -> bool {
        if self.mutation_tickets.get(&ticket).map(|rec| rec.state) == Some(TicketState::Prepared) {
            self.release_mutation_ticket(ticket, TicketState::Expired);
            true
        } else {
            false
        }
    }

    fn release_mutation_ticket(&mut self, ticket: u64, terminal: TicketState) {
        if let Some(mut rec) = self.mutation_tickets.remove(&ticket) {
            for key in &rec.keys[..rec.key_count] {
                if self.reservations.get(key) == Some(&ticket) {
                    self.reservations.remove(key);
                }
            }
            rec.state = terminal;
            self.mutation_tickets.insert(ticket, rec);
        }
    }

    // -----------------------------------------------------------------------
    // Unmount FSM (§6.2 r7 ④)
    // -----------------------------------------------------------------------

    /// `ACTIVE → DRAINING`. Fails `EBUSY` while child mounts exist (children
    /// always unmount first) or for the root mount.
    pub fn begin_drain(&mut self, mount_id: u64) -> Result<(), Errno> {
        if mount_id == ROOT_MOUNT_ID {
            return Err(Errno::EInval);
        }
        if !self.children_of(mount_id).is_empty() {
            return Err(Errno::EBusy);
        }
        let Some(record) = self.mounts.get_mut(&mount_id) else {
            return Err(Errno::ENoent);
        };
        if record.state != MountState::Active {
            return Err(Errno::EInval);
        }
        record.state = MountState::Draining;
        Ok(())
    }

    /// Roll a failed drain or sync back to `ACTIVE` (mount stays usable).
    pub fn rollback_drain(&mut self, mount_id: u64) -> Result<(), Errno> {
        let Some(record) = self.mounts.get_mut(&mount_id) else {
            return Err(Errno::ENoent);
        };
        match record.state {
            MountState::Draining | MountState::Syncing => {
                record.state = MountState::Active;
                Ok(())
            }
            _ => Err(Errno::EInval),
        }
    }

    /// `DRAINING → SYNCING` once the worker confirmed quiescence.
    pub fn mark_syncing(&mut self, mount_id: u64) -> Result<(), Errno> {
        let Some(record) = self.mounts.get_mut(&mount_id) else {
            return Err(Errno::ENoent);
        };
        if record.state != MountState::Draining {
            return Err(Errno::EInval);
        }
        record.state = MountState::Syncing;
        Ok(())
    }

    /// `SYNCING → DETACHED`: atomically remove the topology record, release
    /// its reservations and expire its open tickets; the namespace
    /// generation advances because the visible topology changed.
    pub fn detach(&mut self, mount_id: u64) -> Result<(), Errno> {
        if mount_id == ROOT_MOUNT_ID {
            return Err(Errno::EInval);
        }
        let Some(record) = self.mounts.get(&mount_id) else {
            return Err(Errno::ENoent);
        };
        if record.state != MountState::Syncing {
            return Err(Errno::EInval);
        }
        self.invalidate_mount_tickets(mount_id);
        // Release this mount's own outstanding reservations.
        let stale: Vec<ResKey> = self
            .reservations
            .keys()
            .filter(|key| key.mount == mount_id)
            .cloned()
            .collect();
        for key in stale {
            self.reservations.remove(&key);
        }
        self.mounts.remove(&mount_id);
        self.namespace_generation += 1;
        Ok(())
    }

    /// Remove a mount whose worker control channel was lost.  Unlike the
    /// orderly detach path this is a failure transition: no worker sync or
    /// shutdown can be attempted, all descendants are invalidated, and the
    /// namespace generation advances once for the resulting topology change.
    pub fn mount_failure_scope(&self, mount_id: u64) -> Vec<u64> {
        if mount_id == ROOT_MOUNT_ID || !self.mounts.contains_key(&mount_id) {
            return Vec::new();
        }
        let mut doomed = Vec::new();
        doomed.push(mount_id);
        let mut index = 0;
        while index < doomed.len() {
            let parent = doomed[index];
            for child in self.children_of(parent) {
                if !doomed.contains(&child) {
                    doomed.push(child);
                }
            }
            index += 1;
        }
        doomed
    }

    pub fn fail_mount(&mut self, mount_id: u64) -> bool {
        let doomed = self.mount_failure_scope(mount_id);
        if doomed.is_empty() {
            return false;
        }
        for id in &doomed {
            self.invalidate_mount_tickets(*id);
        }
        let stale: Vec<ResKey> = self
            .reservations
            .keys()
            .filter(|key| doomed.contains(&key.mount))
            .cloned()
            .collect();
        for key in stale {
            self.reservations.remove(&key);
        }
        for id in doomed {
            self.mounts.remove(&id);
        }
        self.namespace_generation += 1;
        true
    }

    // -----------------------------------------------------------------------
    // Path reconstruction (§6.3 r6: parent_anchor chain)
    // -----------------------------------------------------------------------

    /// Rebuild the full host path of `local_path` relative to the root of
    /// `mount_id` by concatenating mountpoint names up to the root mount.
    ///
    /// Chroot-derived bindings truncate the display at their visible root
    /// before calling this, so the missing out-of-chroot prefix is by design.
    pub fn reconstruct_host_path(&self, mount_id: u64, local_path: &[u8]) -> Option<Vec<u8>> {
        if !self.mounts.contains_key(&mount_id) {
            return None;
        }
        let mut components: Vec<Vec<u8>> = Vec::new();
        let mut current = mount_id;
        loop {
            let record = self.mount(current)?;
            if let Some(parent) = record.parent {
                if !record.mountpoint_name.is_empty() {
                    components.push(record.mountpoint_name.clone());
                }
                current = parent;
            } else {
                break;
            }
        }
        let mut path = Vec::new();
        for component in components.iter().rev() {
            path.push(b'/');
            path.extend_from_slice(component);
        }
        if local_path.is_empty() {
            if path.is_empty() {
                path.push(b'/');
            }
        } else {
            path.extend_from_slice(local_path);
        }
        Some(path)
    }
}

#[cfg(test)]
mod tests {
    extern crate alloc;

    use super::*;

    fn target(name: &[u8]) -> TargetResolution {
        TargetResolution {
            owner_mount: ROOT_MOUNT_ID,
            parent_dir: NodeKey {
                node_id: 10,
                generation: 1,
            },
            node: NodeKey {
                node_id: 11,
                generation: 1,
            },
            name: Vec::from(name),
        }
    }

    #[test]
    fn prepare_commit_publishes_mount_and_bumps_generation() {
        let mut t = Topology::new(7);
        let gen_before = t.namespace_generation();
        let ticket = t.prepare(target(b"mnt"), 0, 100).expect("prepare");
        assert_eq!(t.mount_status(ticket), Ok(TicketState::Prepared));

        let info = t
            .commit_mount(
                ticket,
                NodeKey {
                    node_id: 500,
                    generation: 2,
                },
                2,
            )
            .expect("commit");
        assert_eq!(info.mount_id, 1);
        assert_eq!(info.backend_generation, 2);
        assert_eq!(info.namespace_generation, gen_before + 1);
        assert_ne!(info.device_id, t.root().device_id);

        let record = t.mount(info.mount_id).expect("published");
        assert_eq!(record.state, MountState::Active);
        assert_eq!(record.parent, Some(ROOT_MOUNT_ID));
        assert_eq!(
            t.child_at(ROOT_MOUNT_ID, target(b"mnt").node),
            Some(info.mount_id)
        );
        assert_eq!(record.root_node.generation, 2);
        assert_eq!(t.mount_status(ticket), Ok(TicketState::Committed));
    }

    #[test]
    fn failed_post_commit_binding_rolls_back_published_mount() {
        let mut t = Topology::new(7);
        let ticket = t.prepare(target(b"mnt"), 0, 100).expect("prepare");
        let info = t
            .commit_mount(
                ticket,
                NodeKey {
                    node_id: 500,
                    generation: 2,
                },
                2,
            )
            .expect("commit");
        let generation_after_publish = t.namespace_generation();

        t.rollback_published_mount(ticket, info.mount_id)
            .expect("rollback");
        assert!(t.mount(info.mount_id).is_none());
        assert_eq!(t.mount_status(ticket), Ok(TicketState::Aborted));
        assert_eq!(t.namespace_generation(), generation_after_publish + 1);
        t.prepare(target(b"mnt"), 0, 101)
            .expect("rollback releases mountpoint");
    }

    #[test]
    fn mismatched_root_generation_aborts_without_publishing_mount() {
        let mut t = Topology::new(1);
        let second_target = TargetResolution {
            owner_mount: ROOT_MOUNT_ID,
            parent_dir: NodeKey {
                node_id: 10,
                generation: 1,
            },
            node: NodeKey {
                node_id: 12,
                generation: 1,
            },
            name: Vec::from(b"data" as &[u8]),
        };
        let second = t
            .prepare(second_target.clone(), 0, 0)
            .expect("second prepare");
        assert_eq!(
            t.commit_mount(
                second,
                NodeKey {
                    node_id: 600,
                    generation: 3,
                },
                2,
            ),
            Err(Errno::EInval)
        );
        assert_eq!(t.mount_status(second), Ok(TicketState::Aborted));
        assert!(
            t.mount(1).is_none(),
            "rejected commit must not publish a mount"
        );
        t.prepare(second_target, 0, 1)
            .expect("rejected commit released its reservation");
    }

    #[test]
    fn abort_releases_reservation_for_reuse() {
        let mut t = Topology::new(1);
        let ticket = t.prepare(target(b"mnt"), 0, 0).expect("prepare");
        t.abort_mount(ticket).expect("abort");
        assert_eq!(t.mount_status(ticket), Ok(TicketState::Aborted));
        // The freed slot accepts a fresh reservation.
        t.prepare(target(b"mnt"), 0, 1).expect("re-prepare");
    }

    #[test]
    fn abort_after_admission_loses_race() {
        let mut t = Topology::new(1);
        let ticket = t.prepare(target(b"mnt"), 0, 0).expect("prepare");
        // commit with generation 0 is rejected after admission and rolls the
        // ticket back to ABORTED, releasing the reservation.
        assert_eq!(t.commit_mount(ticket, NodeKey::ZERO, 0), Err(Errno::EInval));
        assert_eq!(t.mount_status(ticket), Ok(TicketState::Aborted));

        // Double abort: the second caller loses the single terminal
        // transition and must reconcile via status instead.
        let ticket2 = t.prepare(target(b"mnt"), 0, 1).expect("re-prepare");
        t.abort_mount(ticket2).expect("first abort wins");
        assert_eq!(t.abort_mount(ticket2), Err(Errno::EInval));
        assert_eq!(t.mount_status(ticket2), Ok(TicketState::Aborted));
    }

    #[test]
    fn expiry_only_touches_prepared_tickets() {
        let mut t = Topology::new(1);
        let ticket = t.prepare(target(b"mnt"), 0, 0).expect("prepare");
        // Before the deadline nothing expires.
        assert_eq!(t.poll_expiry(TICKET_TIMEOUT_TICKS - 1), 0);
        assert_eq!(t.mount_status(ticket), Ok(TicketState::Prepared));
        // At the deadline the PREPARED ticket expires and frees the table.
        assert_eq!(t.poll_expiry(TICKET_TIMEOUT_TICKS), 1);
        assert_eq!(t.mount_status(ticket), Ok(TicketState::Expired));
        t.prepare(target(b"mnt"), 0, TICKET_TIMEOUT_TICKS + 1)
            .expect("reservation released by expiry");
    }

    #[test]
    fn peer_close_expires_only_the_affected_ticket() {
        let mut t = Topology::new(1);
        let first = t.prepare(target(b"mnt"), 0, 0).expect("first prepare");
        let second = t
            .prepare(
                TargetResolution {
                    owner_mount: ROOT_MOUNT_ID,
                    parent_dir: NodeKey {
                        node_id: 10,
                        generation: 1,
                    },
                    node: NodeKey {
                        node_id: 12,
                        generation: 1,
                    },
                    name: Vec::from(b"data" as &[u8]),
                },
                0,
                0,
            )
            .expect("second prepare");
        assert!(t.expire_mount_ticket(first));
        assert_eq!(t.mount_status(first), Ok(TicketState::Expired));
        assert_eq!(t.mount_status(second), Ok(TicketState::Prepared));
        assert!(!t.expire_mount_ticket(first));
    }

    #[test]
    fn committing_window_is_immune_to_expiry() {
        let mut t = Topology::new(1);
        let ticket = t.prepare(target(b"mnt"), 0, 0).expect("prepare");

        // Mutation side: admit a mutation commit, then run the clock far
        // past the timeout -- r7 ③ requires COMMITTING to freeze expiry.
        let mutation = t
            .begin_mutation(
                ROOT_MOUNT_ID,
                crate::internal::namespace_binding::OP_CREATE,
                NodeKey::ZERO,
                NodeKey::ZERO,
                NodeKey {
                    node_id: 20,
                    generation: 1,
                },
                NodeKey::ZERO,
                &[],
                b"file",
                5,
            )
            .expect("begin_mutation");
        t.admit_mutation(mutation).expect("admit");
        assert_eq!(t.poll_expiry(TICKET_TIMEOUT_TICKS * 10), 1); // only the mount ticket
        assert_eq!(
            t.mutation_status(mutation),
            Ok(TicketState::Committing),
            "admitted mutation must not expire while committing"
        );
        t.finish_mutation(mutation, true).expect("finish");
        assert_eq!(t.mutation_status(mutation), Ok(TicketState::Committed));

        // The expired mount ticket's status still reconciles idempotently...
        assert_eq!(t.poll_expiry(TICKET_TIMEOUT_TICKS * 20), 0);
        // ...and an expired MountTicket can no longer publish.
        assert_eq!(
            t.commit_mount(
                ticket,
                NodeKey {
                    node_id: 1,
                    generation: 1
                },
                1
            ),
            Err(Errno::EInval)
        );
    }

    #[test]
    fn stacking_is_rejected_with_ebusy() {
        let mut t = Topology::new(1);
        let first = t.prepare(target(b"mnt"), 0, 0).expect("prepare");
        t.commit_mount(
            first,
            NodeKey {
                node_id: 1,
                generation: 2,
            },
            2,
        )
        .expect("commit");

        // Same node again: active mount in the way.
        assert_eq!(t.prepare(target(b"mnt"), 0, 0), Err(Errno::EBusy));
        // A second in-flight reservation on the same name conflicts too.
        let second = TargetResolution {
            owner_mount: ROOT_MOUNT_ID,
            parent_dir: NodeKey {
                node_id: 10,
                generation: 1,
            },
            node: NodeKey {
                node_id: 12,
                generation: 1,
            },
            name: Vec::from(b"data" as &[u8]),
        };
        let ticket = t.prepare(second.clone(), 0, 0).expect("second target");
        assert_eq!(t.prepare(second, 0, 0), Err(Errno::EBusy));
        t.abort_mount(ticket).expect("release");
    }

    #[test]
    fn unmount_fsm_child_first_and_rollback() {
        let mut t = Topology::new(1);
        let parent = t.prepare(target(b"mnt"), 0, 0).expect("prepare");
        let mnt = t
            .commit_mount(
                parent,
                NodeKey {
                    node_id: 1,
                    generation: 2,
                },
                2,
            )
            .expect("commit")
            .mount_id;
        let nested_target = TargetResolution {
            owner_mount: mnt,
            parent_dir: NodeKey {
                node_id: 50,
                generation: 4,
            },
            node: NodeKey {
                node_id: 51,
                generation: 4,
            },
            name: Vec::from(b"sub" as &[u8]),
        };
        let sub_ticket = t.prepare(nested_target, 0, 0).expect("nested prepare");
        let sub = t
            .commit_mount(
                sub_ticket,
                NodeKey {
                    node_id: 60,
                    generation: 3,
                },
                3,
            )
            .expect("nested commit")
            .mount_id;

        // Parent cannot drain while a child exists.
        assert_eq!(t.begin_drain(mnt), Err(Errno::EBusy));

        // Child unmounts first; a failed sync rolls back to ACTIVE.
        t.begin_drain(sub).expect("child drain");
        assert_eq!(
            t.child_at(
                mnt,
                NodeKey {
                    node_id: 51,
                    generation: 4
                }
            ),
            None,
            "DRAINING children are invisible to traversal"
        );
        t.mark_syncing(sub).expect("syncing");
        t.rollback_drain(sub).expect("sync failure rolls back");
        assert_eq!(t.mount(sub).unwrap().state, MountState::Active);
        t.begin_drain(sub).expect("redrain");
        t.mark_syncing(sub).expect("syncing again");
        t.detach(sub).expect("detach");
        assert!(t.mount(sub).is_none(), "DETACHED record is destroyed");

        // A ticket prepared before the drain is expired by detach; a new
        // prepare against the draining mount itself is rejected.
        let open_ticket = t
            .prepare(
                TargetResolution {
                    owner_mount: mnt,
                    parent_dir: NodeKey {
                        node_id: 70,
                        generation: 9,
                    },
                    node: NodeKey {
                        node_id: 71,
                        generation: 9,
                    },
                    name: Vec::from(b"x" as &[u8]),
                },
                0,
                0,
            )
            .expect("ticket on active mount");
        t.begin_drain(mnt).expect("parent drain");
        assert_eq!(
            t.prepare(
                TargetResolution {
                    owner_mount: mnt,
                    parent_dir: NodeKey {
                        node_id: 70,
                        generation: 9
                    },
                    node: NodeKey {
                        node_id: 72,
                        generation: 9
                    },
                    name: Vec::from(b"y" as &[u8]),
                },
                0,
                0
            ),
            Err(Errno::ENoent),
            "draining mounts accept no new reservations"
        );
        t.mark_syncing(mnt).expect("parent syncing");
        t.detach(mnt).expect("parent detach");
        assert_eq!(
            t.mount_status(open_ticket),
            Ok(TicketState::Expired),
            "unmount completion expires outstanding tickets"
        );
        // Root never detaches.
        assert_eq!(t.detach(ROOT_MOUNT_ID), Err(Errno::EInval));
        assert_eq!(t.begin_drain(ROOT_MOUNT_ID), Err(Errno::EInval));
    }

    #[test]
    fn failed_worker_removes_mount_and_expires_owned_tickets() {
        let mut t = Topology::new(1);
        let ticket = t.prepare(target(b"mnt"), 0, 0).expect("prepare");
        let mount = t
            .commit_mount(
                ticket,
                NodeKey {
                    node_id: 1,
                    generation: 2,
                },
                2,
            )
            .expect("commit")
            .mount_id;
        let child_ticket = t
            .prepare(
                TargetResolution {
                    owner_mount: mount,
                    parent_dir: NodeKey {
                        node_id: 10,
                        generation: 2,
                    },
                    node: NodeKey {
                        node_id: 11,
                        generation: 2,
                    },
                    name: Vec::from(b"child" as &[u8]),
                },
                0,
                0,
            )
            .expect("child prepare");

        assert!(t.fail_mount(mount));
        assert!(t.mount(mount).is_none());
        assert_eq!(t.mount_status(child_ticket), Ok(TicketState::Expired));
        assert!(!t.fail_mount(mount));
    }

    #[test]
    fn mutations_validate_shape_and_share_reservations() {
        use crate::internal::namespace_binding as ns;
        let mut t = Topology::new(1);
        let dir = NodeKey {
            node_id: 30,
            generation: 1,
        };
        let file = NodeKey {
            node_id: 31,
            generation: 1,
        };

        // Unknown operation.
        assert_eq!(
            t.begin_mutation(
                ROOT_MOUNT_ID,
                99,
                NodeKey::ZERO,
                NodeKey::ZERO,
                dir,
                NodeKey::ZERO,
                &[],
                b"a",
                0
            ),
            Err(Errno::EInval)
        );
        // unlink without old side / with new fields.
        assert_eq!(
            t.begin_mutation(
                ROOT_MOUNT_ID,
                ns::OP_UNLINK,
                NodeKey::ZERO,
                NodeKey::ZERO,
                dir,
                NodeKey::ZERO,
                &[],
                &[],
                0
            ),
            Err(Errno::EInval)
        );
        assert_eq!(
            t.begin_mutation(
                ROOT_MOUNT_ID,
                ns::OP_CREATE,
                NodeKey::ZERO,
                NodeKey::ZERO,
                dir,
                NodeKey::ZERO,
                b"ghost",
                b"f",
                0
            ),
            Err(Errno::EInval)
        );

        // create reserves (dir, "f"); rename of another file onto it is EBUSY.
        let create = t
            .begin_mutation(
                ROOT_MOUNT_ID,
                ns::OP_CREATE,
                NodeKey::ZERO,
                NodeKey::ZERO,
                dir,
                NodeKey::ZERO,
                &[],
                b"f",
                0,
            )
            .expect("create reservation");
        assert_eq!(
            t.begin_mutation(
                ROOT_MOUNT_ID,
                ns::OP_RENAME,
                dir,
                file,
                dir,
                NodeKey::ZERO,
                b"g",
                b"f",
                0
            ),
            Err(Errno::EBusy)
        );
        t.admit_mutation(create).expect("admit");
        t.finish_mutation(create, true).expect("release");

        // After release the same names are free again.
        let rename = t
            .begin_mutation(
                ROOT_MOUNT_ID,
                ns::OP_RENAME,
                dir,
                file,
                dir,
                NodeKey::ZERO,
                b"g",
                b"f",
                1,
            )
            .expect("rename reservation");
        t.abort_mutation(rename).expect("abort before effects");

        // A pending mount reservation blocks touching the same component.
        let _ticket = t.prepare(target(b"mnt"), 0, 2).expect("mount prepare");
        assert_eq!(
            t.begin_mutation(
                ROOT_MOUNT_ID,
                ns::OP_RENAME,
                dir,
                file,
                NodeKey {
                    node_id: 10,
                    generation: 1
                },
                NodeKey::ZERO,
                b"g",
                b"mnt",
                3
            ),
            Err(Errno::EBusy)
        );
    }

    #[test]
    fn host_path_reconstruction_walks_parent_chain() {
        let mut t = Topology::new(1);
        let mnt = {
            let ticket = t.prepare(target(b"mnt"), 0, 0).expect("prepare");
            t.commit_mount(
                ticket,
                NodeKey {
                    node_id: 1,
                    generation: 2,
                },
                2,
            )
            .expect("commit")
            .mount_id
        };
        let deep_target = TargetResolution {
            owner_mount: mnt,
            parent_dir: NodeKey {
                node_id: 50,
                generation: 4,
            },
            node: NodeKey {
                node_id: 51,
                generation: 4,
            },
            name: Vec::from(b"data" as &[u8]),
        };
        let deep = {
            let ticket = t.prepare(deep_target, 0, 0).expect("deep prepare");
            t.commit_mount(
                ticket,
                NodeKey {
                    node_id: 60,
                    generation: 3,
                },
                3,
            )
            .expect("commit")
            .mount_id
        };
        assert_eq!(
            t.reconstruct_host_path(deep, b"/etc/hosts"),
            Some(Vec::from(&b"/mnt/data/etc/hosts"[..]))
        );
        assert_eq!(
            t.reconstruct_host_path(mnt, &[]),
            Some(Vec::from(&b"/mnt"[..]))
        );
        assert_eq!(
            t.reconstruct_host_path(ROOT_MOUNT_ID, &[]),
            Some(Vec::from(&b"/"[..]))
        );
        assert_eq!(t.reconstruct_host_path(deep + 5, b""), None);
    }

    #[test]
    fn invalidate_expires_all_open_tickets_of_a_mount() {
        let mut t = Topology::new(1);
        let mnt = {
            let ticket = t.prepare(target(b"mnt"), 0, 0).expect("prepare");
            t.commit_mount(
                ticket,
                NodeKey {
                    node_id: 1,
                    generation: 2,
                },
                2,
            )
            .expect("commit")
            .mount_id
        };
        let mount_ticket = t
            .prepare(
                TargetResolution {
                    owner_mount: mnt,
                    parent_dir: NodeKey {
                        node_id: 70,
                        generation: 9,
                    },
                    node: NodeKey {
                        node_id: 71,
                        generation: 9,
                    },
                    name: Vec::from(b"y" as &[u8]),
                },
                0,
                0,
            )
            .expect("mount ticket under child");
        let mutation = t
            .begin_mutation(
                mnt,
                crate::internal::namespace_binding::OP_MKDIR,
                NodeKey::ZERO,
                NodeKey::ZERO,
                NodeKey {
                    node_id: 80,
                    generation: 2,
                },
                NodeKey::ZERO,
                &[],
                b"d",
                0,
            )
            .expect("mutation under child");
        assert_eq!(t.invalidate_mount_tickets(mnt), 2);
        assert_eq!(t.mount_status(mount_ticket), Ok(TicketState::Expired));
        assert_eq!(t.mutation_status(mutation), Ok(TicketState::Expired));
    }
}
