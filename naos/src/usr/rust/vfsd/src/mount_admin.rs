//! Control-plane endpoint glue between the wire protocols and
//! [`crate::mount::Topology`] (doc/VFS_BLOCK_DEVICE_ADR.md §6.2-§6.4, r6/r7).
//!
//! [`crate::server`] owns the File/Directory data plane; this module serves
//! everything else:
//!
//! * `Vfs` (scope 16) — get_root / prepare_mount / unmount / sync /
//!   get_mount_info, served through the generated dispatcher;
//! * `MountTicket` (scope 19) — commit {root NodeKey, generation} is the only
//!   publish point (r7); abort/status reconcile without re-sending commit;
//! * `MutationTicket` (scope 22) — reservation lease: commit admits
//!   (`COMMITTING`, freezing expiry) and finishes (`COMMITTED`);
//! * `NamespaceBinding` (scope 18, generated private IDL) — resolve_absolute / route_above /
//!   enter_child_mount / derive_chroot / begin_mutation (§6.3 r6: entering a
//!   child mount keeps the visible root; derive_chroot narrows it);
//! * `MountControl` (scope 21, generated private IDL) — bind_root / bind_node /
//!   sync / prepare_unmount / shutdown / lookup_target.
//!
//! Production mounts are owned by an external worker and are published only
//! after its `MountTicket.commit`; vfsd then performs the post-commit
//! `MountControl.bind_node` handshake and retains the root anchor. The
//! embedded RAM backend remains available only for host tests/fallback mode.

use alloc::collections::BTreeMap;
use alloc::vec::Vec;
use core::mem::ManuallyDrop;

use naos_idl::directory;
use naos_idl::mount_control as mc_wire;
use naos_idl::mount_ticket as mt_wire;
use naos_idl::mutation_ticket as mutation_wire;
use naos_idl::namespace_binding as namespace_wire;
use naos_idl::vfs;
use naos_idl::{
    CallError, DispatchOutcome, FailInvocation, IncomingRequest, Invocation, MethodReply,
    OwnedHandle, ProtocolClientEndpoint, ProtocolServerEndpoint, ReceivedResources, ResourceSlot,
    ResourceTransferBuilder,
};
use naos_sys as sys;

use crate::backend::{FileKind, MAX_PATH_BYTES, NodeId, RamFs};
use crate::errno::Errno;
use crate::internal::{self, NodeKey};
use crate::mount::{ROOT_MOUNT_ID, TargetResolution, TicketState, Topology};
use crate::server::{Binding, Entry, NsBindingState, PendingRegistration};

/// Default global symlink budget for control-plane walks that arrive without
/// one (prepare_mount/unmount targets). Route requests carry their own.
const DEFAULT_SYMLINK_BUDGET: u32 = 8;

/// Byte budget of the embedded RAM worker placeholder used only by the host
/// test/fallback mode; production `VfsServer::new_external` does not create
/// a second backend for an external worker mount.
const EMBEDDED_BACKEND_BYTES: u64 = 4 << 20;

fn fail(errno: Errno) -> FailInvocation {
    FailInvocation::domain(-(errno.to_i32() as i64))
}

fn strip_nul(path: &[u8]) -> &[u8] {
    let mut end = path.len();
    while end > 0 && path[end - 1] == 0 {
        end -= 1;
    }
    &path[..end]
}

fn backend_of<'a>(
    root_fs: &'a RamFs,
    backends: &'a BTreeMap<u64, RamFs>,
    mount: u64,
) -> Option<&'a RamFs> {
    if mount == ROOT_MOUNT_ID {
        Some(root_fs)
    } else {
        backends.get(&mount)
    }
}

fn node_key(topology: &Topology, mount: u64, id: NodeId) -> NodeKey {
    NodeKey {
        node_id: id,
        generation: topology
            .mount(mount)
            .map(|record| record.backend_generation)
            .unwrap_or(0),
    }
}

// ---------------------------------------------------------------------------
// Ticket → published-mount link for MountControl endpoints
// ---------------------------------------------------------------------------

/// Links each MountTicket id to the mount published by its commit, so a
/// `Binding::MountControl` endpoint can find the instance it controls after
/// publication. The topology record keeps no ticket reference and this module
/// cannot annotate it, so the link lives here; entries are bounded by the
/// number of mounts ever committed in this process and removed on shutdown.
static CONTROL_MOUNTS: SpinLock<BTreeMap<u64, u64>> = SpinLock::new(BTreeMap::new());

/// Minimal spin lock: the serve loop is single-threaded, but `cargo test`
/// runs test functions on independent threads sharing the process registry.
struct SpinLock<T> {
    held: core::sync::atomic::AtomicBool,
    value: core::cell::UnsafeCell<T>,
}

impl<T> SpinLock<T> {
    const fn new(value: T) -> Self {
        Self {
            held: core::sync::atomic::AtomicBool::new(false),
            value: core::cell::UnsafeCell::new(value),
        }
    }

    fn with<R>(&self, operation: impl FnOnce(&mut T) -> R) -> R {
        use core::sync::atomic::Ordering;
        while self.held.swap(true, Ordering::Acquire) {
            core::hint::spin_loop();
        }
        // SAFETY: exclusive access is guaranteed by the held flag above.
        let result = operation(unsafe { &mut *self.value.get() });
        self.held.store(false, Ordering::Release);
        result
    }
}

// SAFETY: access is serialized by the spin lock; T itself needs no Sync.
unsafe impl<T: Send> Sync for SpinLock<T> {}

fn link_control_mount(ticket: u64, mount: u64) {
    CONTROL_MOUNTS.with(|registry| registry.insert(ticket, mount));
}

fn control_mount(ticket: u64) -> Option<u64> {
    CONTROL_MOUNTS.with(|registry| registry.get(&ticket).copied())
}

pub(crate) fn unlink_control_mount(mount: u64) {
    CONTROL_MOUNTS.with(|registry| registry.retain(|_, &mut m| m != mount));
}

pub(crate) fn control_mount_ticket_for_mount(mount: u64) -> Option<u64> {
    CONTROL_MOUNTS.with(|registry| {
        registry
            .iter()
            .find_map(|(ticket, value)| (*value == mount).then_some(*ticket))
    })
}

/// Apply the lifecycle consequence of a worker/control peer closing.  A
/// prepared ticket is a reservation lease, so its peer close must release
/// exactly that lease; a published MountControl close invalidates all
/// outstanding tickets owned by the now-dead mount.
pub(crate) fn peer_closed_control(
    ticket: u64,
    topology: &mut Topology,
    pending_backends: &mut BTreeMap<u64, RamFs>,
) -> Vec<u64> {
    let mut invalidated_mounts = Vec::new();
    if let Some(mount) = control_mount(ticket) {
        invalidated_mounts = topology.mount_failure_scope(mount);
        topology.fail_mount(mount);
        topology.invalidate_mount_tickets(mount);
        let stale: Vec<u64> = pending_backends
            .keys()
            .filter(|id| topology.mount_status(**id) == Ok(TicketState::Expired))
            .copied()
            .collect();
        for id in stale {
            pending_backends.remove(&id);
        }
    } else if topology.expire_mount_ticket(ticket) {
        // The worker can disappear before committing. In that window there
        // is no ticket→mount link yet, so expire the reservation directly.
        pending_backends.remove(&ticket);
    }
    CONTROL_MOUNTS.with(|registry| {
        registry.remove(&ticket);
    });
    invalidated_mounts
}

// ---------------------------------------------------------------------------
// Topology-aware path routing (§6.3)
// ---------------------------------------------------------------------------

/// A fully resolved path component chain: owning mount, parent directory,
/// final component name and resolved final node (all local identities of the
/// owning backend).
#[derive(Clone, Debug)]
pub struct ResolvedTarget {
    pub mount: u64,
    pub parent: NodeId,
    pub name: Vec<u8>,
    pub node: NodeId,
}

/// Walk `path` starting from `state.visible_root`, crossing into active child
/// mounts at mountpoint hits and restarting at the visible root on absolute
/// symlink targets (chroot containment, §6.3).
///
/// Every symlink expansion consumes one unit of `budget`; exhaustion reports
/// `ELOOP`. The final component is followed only when `follow_final` holds
/// (mount targets must not be followed, ADR §6.2).
pub fn resolve_route(
    topology: &Topology,
    root_fs: &RamFs,
    backends: &BTreeMap<u64, RamFs>,
    state: NsBindingState,
    path: &[u8],
    budget: &mut u32,
    follow_final: bool,
) -> Result<ResolvedTarget, Errno> {
    let components: Vec<&[u8]> = path
        .split(|&byte| byte == b'/')
        .filter(|c| !c.is_empty())
        .collect();
    if components.is_empty() {
        return Err(Errno::EInval);
    }
    let mut mount = state.visible_root_mount;
    let mut dir = state.visible_root;
    for index in 0..components.len() {
        let component = components[index];
        let last = index + 1 == components.len();
        let backend = backend_of(root_fs, backends, mount).ok_or(Errno::ENoent)?;
        // Scope anchor: the visible root clamps `..` in the issuing mount's
        // backend; child backends are naturally contained by their own root.
        let anchor = if mount == state.visible_root_mount {
            state.visible_root
        } else {
            backend.root()
        };
        let follow = !last || follow_final;
        let node = match backend.lookup_scoped(anchor, dir, component, false) {
            Ok(node) => node,
            Err(errno) => return Err(errno),
        };
        if follow && !node_is_regular_or_dir(backend, node)? {
            // Symlink expansion (§6.4 WalkContext budget).
            if *budget == 0 {
                return Err(Errno::ELoop);
            }
            *budget -= 1;
            let mut scratch = [0u8; MAX_PATH_BYTES];
            let len = backend
                .read_target(node, &mut scratch)
                .map_err(|e| if e == Errno::ENoent { Errno::ENoent } else { e })?;
            let mut target = Vec::from(&scratch[..len]);
            if !last {
                for part in &components[index + 1..] {
                    target.push(b'/');
                    target.extend_from_slice(part);
                }
            }
            let base = if target.first() == Some(&b'/') {
                // Absolute targets restart at the visible root -- never at the
                // current mount -- so chroot bounds hold across expansions.
                (state.visible_root_mount, state.visible_root)
            } else {
                (mount, dir)
            };
            let nested = NsBindingState {
                visible_root_mount: state.visible_root_mount,
                visible_root: state.visible_root,
                current_mount: base.0,
                current: base.1,
                mount_stack: state.mount_stack,
            };
            return resolve_route(
                topology,
                root_fs,
                backends,
                nested,
                &target,
                budget,
                if last { follow_final } else { true },
            );
        }
        if last {
            return Ok(ResolvedTarget {
                mount,
                parent: dir,
                name: Vec::from(component),
                node,
            });
        }
        let meta = backend.metadata_of(node)?;
        if meta.kind != FileKind::Directory {
            return Err(Errno::ENotDir);
        }
        if let Some(child) = topology.child_at(mount, node_key(topology, mount, node)) {
            mount = child;
            dir = backend_of(root_fs, backends, child)
                .ok_or(Errno::ENoent)?
                .root();
        } else {
            dir = node;
        }
    }
    Err(Errno::EInval)
}

/// Classifies the final lookup result: `true` when the node is not a symlink
/// (so no expansion is needed), `false` when it is.
fn node_is_regular_or_dir(backend: &RamFs, node: NodeId) -> Result<bool, Errno> {
    let meta = backend.metadata_of(node)?;
    Ok(meta.kind != FileKind::Symlink)
}

pub(crate) fn binding_references_mount(binding: &Binding, mount: u64) -> bool {
    match binding {
        Binding::Directory { mount: m, .. } | Binding::File { mount: m, .. } => *m == mount,
        Binding::NamespaceBinding(state) => {
            state.visible_root_mount == mount || state.current_mount == mount
        }
        // Control-plane endpoints never count toward busy refs: VfsAdmin is
        // the caller itself, tickets are reconciled via the shared
        // reservation table (invalidate_mount_tickets at detach) and the
        // MountControl end belongs to the worker side of this same process.
        Binding::VfsAdmin
        | Binding::MountTicket { .. }
        | Binding::MutationTicket { .. }
        | Binding::MountControl { .. } => false,
    }
}

pub(crate) fn ns_state_references_mount(state: NsBindingState, mount: u64) -> bool {
    state.visible_root_mount == mount || state.current_mount == mount
}

// ---------------------------------------------------------------------------
// Vfs admin endpoint (scope 16)
// ---------------------------------------------------------------------------

/// Field bundle the dispatch arm hands to [`run_vfs_admin`] while the serve
/// loop holds the state split.
pub struct VfsCtl<'a> {
    pub root_fs: &'a mut RamFs,
    pub backends: &'a mut BTreeMap<u64, RamFs>,
    pub pending_backends: &'a mut BTreeMap<u64, RamFs>,
    pub topology: &'a mut Topology,
    pub entries: &'a mut BTreeMap<sys::Handle, Entry>,
    pub clients: &'a mut BTreeMap<u64, (u64, NodeId, NodeId)>,
    pub binding_clients: &'a mut BTreeMap<u64, NsBindingState>,
    pub root_anchors: &'a mut BTreeMap<u64, OwnedHandle>,
    pub mount_controls: &'a mut BTreeMap<u64, OwnedHandle>,
    pub external_workers: bool,
    pub pending: &'a mut Vec<PendingRegistration>,
    pub reply_wire: &'a mut [u8],
}

/// Serve one Vfs request through the generated dispatcher.
pub fn run_vfs_admin(
    ctx: VfsCtl<'_>,
    incoming: IncomingRequest<'_>,
) -> Result<DispatchOutcome, CallError> {
    let VfsCtl {
        root_fs,
        backends,
        pending_backends,
        topology,
        entries,
        clients,
        binding_clients,
        root_anchors,
        mount_controls,
        external_workers,
        pending,
        reply_wire,
    } = ctx;
    let mut service = VfsAdminService {
        root_fs,
        backends,
        pending_backends,
        topology,
        entries,
        clients,
        binding_clients,
        root_anchors,
        mount_controls,
        external_workers,
        pending,
    };
    vfs::dispatch(&mut service, incoming, reply_wire)
}

struct VfsAdminService<'a> {
    root_fs: &'a mut RamFs,
    backends: &'a mut BTreeMap<u64, RamFs>,
    pending_backends: &'a mut BTreeMap<u64, RamFs>,
    topology: &'a mut Topology,
    entries: &'a mut BTreeMap<sys::Handle, Entry>,
    /// Client ends this server minted; the serve loop joins registrations
    /// after dispatch, so the handler itself never reads this table.
    #[allow(dead_code)]
    clients: &'a mut BTreeMap<u64, (u64, NodeId, NodeId)>,
    binding_clients: &'a mut BTreeMap<u64, NsBindingState>,
    root_anchors: &'a mut BTreeMap<u64, OwnedHandle>,
    mount_controls: &'a mut BTreeMap<u64, OwnedHandle>,
    external_workers: bool,
    pending: &'a mut Vec<PendingRegistration>,
}

impl VfsAdminService<'_> {
    /// Run one worker lifecycle RPC synchronously after the mount enters
    /// DRAINING. The worker is therefore the final admission barrier.
    fn worker_lifecycle<R>(
        &self,
        mount: u64,
        submit: impl FnOnce(&ProtocolClientEndpoint, &mut [u8]) -> Result<Invocation, CallError>,
        take: impl FnOnce(&mut Invocation, &mut [u8]) -> Result<R, CallError>,
    ) -> Result<(), Errno> {
        let ticket = control_mount_ticket_for_mount(mount).ok_or(Errno::ENodev)?;
        let control = self.mount_controls.get(&ticket).ok_or(Errno::ENodev)?;
        // The OwnedHandle retains authority; this temporary typed view must
        // not close it when the call returns.
        let endpoint =
            ManuallyDrop::new(unsafe { ProtocolClientEndpoint::from_raw(control.get()) });
        let mut request_wire = [0u8; 32];
        let mut invocation = submit(&endpoint, &mut request_wire).map_err(lifecycle_error)?;
        if !wait_invocation(&invocation) {
            return Err(Errno::ENodev);
        }
        let mut response_wire = [0u8; 64];
        take(&mut invocation, &mut response_wire)
            .map(|_| ())
            .map_err(lifecycle_error)
    }

    fn worker_sync(&self, mount: u64) -> Result<(), Errno> {
        self.worker_lifecycle(
            mount,
            |endpoint, wire| {
                mc_wire::submit_sync(
                    endpoint,
                    &mc_wire::sync_request {},
                    ResourceTransferBuilder::new(),
                    wire,
                    0,
                )
            },
            mc_wire::take_sync,
        )
    }

    fn worker_prepare_unmount(&self, mount: u64) -> Result<(), Errno> {
        self.worker_lifecycle(
            mount,
            |endpoint, wire| {
                mc_wire::submit_prepare_unmount(
                    endpoint,
                    &mc_wire::prepare_unmount_request {},
                    ResourceTransferBuilder::new(),
                    wire,
                    0,
                )
            },
            mc_wire::take_prepare_unmount,
        )
    }

    fn worker_shutdown(&self, mount: u64) -> Result<(), Errno> {
        self.worker_lifecycle(
            mount,
            |endpoint, wire| {
                mc_wire::submit_shutdown(
                    endpoint,
                    &mc_wire::shutdown_request {},
                    ResourceTransferBuilder::new(),
                    wire,
                    0,
                )
            },
            mc_wire::take_shutdown,
        )
    }

    /// Roll back all state created after a `prepare_mount` reservation.  The
    /// endpoint registrations carry non-owning client raw handles; their
    /// actual owners are the local `OwnedHandle`/`ResourceTransferBuilder`, so dropping
    /// only the server ends here avoids both leaks and double-close races.
    fn rollback_prepare_mount(&mut self, ticket: u64, pending_start: usize) {
        let _ = self.topology.abort_mount(ticket);
        self.pending_backends.remove(&ticket);
        self.mount_controls.remove(&ticket);
        for (server, _, _) in self.pending.drain(pending_start..) {
            drop(server);
        }
    }

    /// Which half of a minted pair travels in the reply.
    fn mint_half(
        &mut self,
        kind: MintKind,
        half: Half,
        binding: Option<Binding>,
    ) -> Result<OwnedHandle, FailInvocation> {
        let endpoints = mint(kind).map_err(|_| sys::STATUS_RESOURCE_EXHAUSTED);
        self.mint_half_pair(endpoints, half, binding)
    }

    fn spawn_directory_binding(
        &mut self,
        binding: Binding,
    ) -> Result<(ResourceSlot, ResourceTransferBuilder<'static>), FailInvocation> {
        let client = self.mint_half(MintKind::Directory, Half::Client, Some(binding))?;
        let mut resources: ResourceTransferBuilder<'static> = ResourceTransferBuilder::new();
        let slot = resources.push_move(client).map_err(|_| fail(Errno::EIo))?;
        Ok((slot, resources))
    }

    /// Client-half mint from an explicit pair (generated MountTicket wire).
    fn mint_client_wire(
        &mut self,
        endpoints: Result<(ProtocolClientEndpoint, ProtocolServerEndpoint), sys::Status>,
        binding: Binding,
    ) -> Result<OwnedHandle, FailInvocation> {
        self.mint_half_pair(endpoints, Half::Client, Some(binding))
    }

    /// Shared half-selection over an explicit pair.
    fn mint_half_pair(
        &mut self,
        endpoints: Result<(ProtocolClientEndpoint, ProtocolServerEndpoint), sys::Status>,
        half: Half,
        binding: Option<Binding>,
    ) -> Result<OwnedHandle, FailInvocation> {
        let (client, server) = endpoints.map_err(|_| fail(Errno::EIo))?;
        match half {
            Half::Client => {
                let client_id = if binding.is_some() {
                    Some(naos_idl::object_id(client.get()).map_err(|_| fail(Errno::EIo))?)
                } else {
                    None
                };
                let client_raw = client.into_raw();
                // SAFETY: uniquely owned freshly minted handle.
                let client_handle = unsafe { OwnedHandle::from_raw(client_raw) };
                if let Some(binding) = binding {
                    self.pending.push((
                        server,
                        binding,
                        Some((client_raw, client_id.expect("binding capability identity"))),
                    ));
                } else {
                    drop(server);
                }
                Ok(client_handle)
            }
            Half::Server => {
                drop(client);
                let server_raw = server.into_raw();
                // SAFETY: uniquely owned freshly minted handle.
                Ok(unsafe { OwnedHandle::from_raw(server_raw) })
            }
        }
    }

    fn system_root_state(&self) -> NsBindingState {
        let root = self.root_fs.root();
        NsBindingState {
            visible_root_mount: ROOT_MOUNT_ID,
            visible_root: root,
            current_mount: ROOT_MOUNT_ID,
            current: root,
            mount_stack: 0,
        }
    }

    /// Resolve a Vfs-instance-absolute target path down the topology.
    fn resolve_target(&mut self, path: &[u8]) -> Result<ResolvedTarget, FailInvocation> {
        if path.is_empty() || path[0] != b'/' {
            return Err(fail(Errno::EInval));
        }
        let mut budget = DEFAULT_SYMLINK_BUDGET;
        resolve_route(
            self.topology,
            self.root_fs,
            self.backends,
            self.system_root_state(),
            path,
            &mut budget,
            // Mount targets never follow their final component (§6.2).
            false,
        )
        .map_err(fail)
    }
}

impl vfs::VfsHandler for VfsAdminService<'_> {
    fn get_root<'s>(
        &'s mut self,
        _request: vfs::get_root_request,
    ) -> Result<MethodReply<'s, vfs::get_root_response>, FailInvocation> {
        let root = self.root_fs.root();
        let (slot, resources) = self.spawn_directory_binding(Binding::Directory {
            mount: ROOT_MOUNT_ID,
            root,
            current: root,
        })?;
        Ok(MethodReply::with_resources(
            vfs::get_root_response { root: slot },
            resources,
        ))
    }

    fn prepare_mount<'s>(
        &'s mut self,
        request: vfs::prepare_mount_request<'_>,
    ) -> Result<MethodReply<'s, vfs::prepare_mount_response>, FailInvocation> {
        // v1 flags accept zero and READ_ONLY only; every other bit is
        // unknown (EINVAL) and never silently ignored (§6.2).
        const READ_ONLY: u64 = 1;
        if request.flags & !READ_ONLY != 0 {
            return Err(fail(Errno::EInval));
        }
        let resolved = self.resolve_target(strip_nul(request.target))?;
        let backend = backend_of(self.root_fs, self.backends, resolved.mount)
            .ok_or_else(|| fail(Errno::ENoent))?;
        let meta = backend.metadata_of(resolved.node).map_err(fail)?;
        if meta.kind != FileKind::Directory {
            return Err(fail(Errno::ENotDir));
        }
        let target = TargetResolution {
            owner_mount: resolved.mount,
            parent_dir: node_key(self.topology, resolved.mount, resolved.parent),
            node: node_key(self.topology, resolved.mount, resolved.node),
            name: resolved.name.clone(),
        };
        let ticket = self
            .topology
            .prepare(target, request.flags, servicekit::monotonic_ticks())
            .map_err(fail)?;
        let pending_start = self.pending.len();
        // Embedded worker: the placeholder backend this same process serves
        // after commit. A real deployment replaces this with child bootstrap.
        if !self.external_workers {
            self.pending_backends
                .insert(ticket, RamFs::new(EMBEDDED_BACKEND_BYTES));
        }

        // The worker receives the MountControl *server* end.  vfsd retains
        // the client half so lifecycle calls remain owned by the namespace
        // manager after the worker is moved to its own process.
        let (ctl_client, ctl_server) = match mint(MintKind::MountControl) {
            Ok((client, server)) => {
                let client_raw = client.into_raw();
                let client = unsafe { OwnedHandle::from_raw(client_raw) };
                let server = unsafe { OwnedHandle::from_raw(server.into_raw()) };
                (client, server)
            }
            Err(_) => {
                self.rollback_prepare_mount(ticket, pending_start);
                return Err(fail(Errno::EIo));
            }
        };
        self.mount_controls.insert(ticket, ctl_client);
        // MountTicket pairs come from the generated binding (scope 19).
        let ticket_client = match self.mint_client_wire(
            mt_wire::create_endpoints(None).map_err(|_| sys::STATUS_RESOURCE_EXHAUSTED),
            Binding::MountTicket { ticket },
        ) {
            Ok(client) => client,
            Err(error) => {
                self.mount_controls.remove(&ticket);
                self.rollback_prepare_mount(ticket, pending_start);
                return Err(error);
            }
        };
        let mut resources: ResourceTransferBuilder<'static> = ResourceTransferBuilder::new();
        let ctl_slot = match resources.push_move(ctl_server) {
            Ok(slot) => slot,
            Err(_) => {
                self.mount_controls.remove(&ticket);
                self.rollback_prepare_mount(ticket, pending_start);
                return Err(fail(Errno::EIo));
            }
        };
        let ticket_slot = match resources.push_move(ticket_client) {
            Ok(slot) => slot,
            Err(_) => {
                self.mount_controls.remove(&ticket);
                self.rollback_prepare_mount(ticket, pending_start);
                return Err(fail(Errno::EIo));
            }
        };
        Ok(MethodReply::with_resources(
            vfs::prepare_mount_response {
                control: ctl_slot,
                ticket: ticket_slot,
            },
            resources,
        ))
    }

    fn unmount<'s>(
        &'s mut self,
        request: vfs::unmount_request<'_>,
    ) -> Result<MethodReply<'s, vfs::unmount_response>, FailInvocation> {
        // Only zero flags are accepted (§6.2).
        if request.flags != 0 {
            return Err(fail(Errno::EInval));
        }
        let resolved = self.resolve_target(strip_nul(request.target))?;
        let mountpoint = node_key(self.topology, resolved.mount, resolved.node);
        let mount = self
            .topology
            .child_at(resolved.mount, mountpoint)
            .ok_or_else(|| fail(Errno::EInval))?;

        // r7 ④: ACTIVE -> DRAINING atomically stops traversal and starts
        // rejecting new routes.  Keep the private anchor until the worker
        // handshake has completed: if prepare/sync/shutdown fails and the
        // mount rolls back to ACTIVE, the existing anchor is still needed to
        // preserve a usable data plane.
        self.topology.begin_drain(mount).map_err(fail)?;

        // Worker quiescence check: any live application endpoint or binding
        // that references the mount keeps it busy; private anchors are not
        // counted per §6.2 r7.
        let busy = self
            .entries
            .values()
            .any(|entry| binding_references_mount(&entry.binding, mount))
            || self
                .binding_clients
                .values()
                .any(|state| ns_state_references_mount(*state, mount));
        if busy {
            self.topology.rollback_drain(mount).map_err(fail)?;
            return Err(fail(Errno::EBusy));
        }

        // DRAINING -> SYNCING. External workers must first enter their own
        // draining state; this is the point at which they reject new opens
        // and wait for already admitted operations. The embedded fallback
        // has no second endpoint and keeps the historical local path.
        if self.external_workers {
            if let Err(errno) = self.worker_prepare_unmount(mount) {
                self.topology.rollback_drain(mount).map_err(fail)?;
                return Err(fail(errno));
            }
        }
        self.topology.mark_syncing(mount).map_err(fail)?;

        // The worker owns the persistence ordering domain.  Do not detach
        // vfsd's topology until its sync barrier has completed successfully.
        if self.external_workers {
            if let Err(errno) = self.worker_sync(mount) {
                self.topology.rollback_drain(mount).map_err(fail)?;
                return Err(fail(errno));
            }
        }

        // Shutdown is deliberately the final worker call.  A failure leaves
        // the mount in ACTIVE so callers may retry; successful shutdown is
        // followed immediately by topology detach and control cleanup.
        if self.external_workers {
            if let Err(errno) = self.worker_shutdown(mount) {
                self.topology.rollback_drain(mount).map_err(fail)?;
                return Err(fail(errno));
            }
        }
        self.topology.detach(mount).map_err(fail)?;
        self.root_anchors.remove(&mount);

        // Release everything the detached mount owned: its backend, every
        // endpoint/binding derived from it, and the MountControl link.
        self.backends.remove(&mount);
        self.entries
            .retain(|_, entry| !binding_references_mount(&entry.binding, mount));
        self.clients.retain(|_, (owner, _, _)| *owner != mount);
        self.binding_clients
            .retain(|_, state| !ns_state_references_mount(*state, mount));
        if let Some(ticket) = control_mount_ticket_for_mount(mount) {
            self.mount_controls.remove(&ticket);
        }
        unlink_control_mount(mount);
        Ok(MethodReply::new(vfs::unmount_response {}))
    }

    fn sync<'s>(
        &'s mut self,
        request: vfs::sync_request,
    ) -> Result<MethodReply<'s, vfs::sync_response>, FailInvocation> {
        if self.topology.mount(request.mount_id).is_none() {
            return Err(fail(Errno::ENoent));
        }
        if request.mount_id != ROOT_MOUNT_ID {
            if self.external_workers {
                self.worker_sync(request.mount_id).map_err(fail)?;
            } else if !self.backends.contains_key(&request.mount_id) {
                return Err(fail(Errno::ENoent));
            }
        }
        Ok(MethodReply::new(vfs::sync_response {}))
    }

    fn get_mount_info<'s>(
        &'s mut self,
        request: vfs::get_mount_info_request,
    ) -> Result<MethodReply<'s, vfs::get_mount_info_response>, FailInvocation> {
        let record = self
            .topology
            .mount(request.mount_id)
            .ok_or_else(|| fail(Errno::ENoent))?;
        Ok(MethodReply::new(vfs::get_mount_info_response {
            value: vfs::MountInfo {
                mount_id: request.mount_id,
                namespace_generation: self.topology.namespace_generation(),
                flags: record.flags,
                backend_generation: record.backend_generation,
                device_id: record.device_id,
            },
        }))
    }
}

// ---------------------------------------------------------------------------
// MountTicket endpoint (scope 19)
// ---------------------------------------------------------------------------

struct MountTicketService<'a> {
    root_fs: &'a RamFs,
    topology: &'a mut Topology,
    pending_backends: &'a mut BTreeMap<u64, RamFs>,
    backends: &'a mut BTreeMap<u64, RamFs>,
    mount_controls: &'a mut BTreeMap<u64, OwnedHandle>,
    pending: &'a mut Vec<PendingRegistration>,
    root_anchors: &'a mut BTreeMap<u64, OwnedHandle>,
    external_workers: bool,
    ticket: u64,
}

fn wait_invocation(invocation: &Invocation) -> bool {
    servicekit::wait_for_completion(invocation.get(), 5_000_000)
}

fn lifecycle_error(error: CallError) -> Errno {
    match error {
        CallError::Outcome { protocol_error, .. } => match protocol_error {
            value if value == -(Errno::EBusy.to_i32() as i64) => Errno::EBusy,
            value if value == -(Errno::ENodev.to_i32() as i64) => Errno::ENodev,
            _ => Errno::EIo,
        },
        CallError::Status(status)
            if status == sys::STATUS_PEER_CLOSED
                || status == sys::STATUS_INVALID_HANDLE
                || status == sys::STATUS_OBJECT_REVOKED =>
        {
            Errno::ENodev
        }
        _ => Errno::EIo,
    }
}

impl MountTicketService<'_> {
    /// Seed the worker's root Directory with a vfsd-issued NamespaceBinding
    /// after commit. This RPC is intentionally impossible while the ticket
    /// is PREPARED, avoiding the frozen pending-worker re-entry cycle.
    fn bind_worker_root(
        &mut self,
        mount_id: u64,
        root_node: NodeKey,
    ) -> Result<(), FailInvocation> {
        let control = self
            .mount_controls
            .get(&self.ticket)
            .ok_or_else(|| fail(Errno::ENodev))?;
        let (binding_client, binding_server) =
            namespace_wire::create_endpoints(None).map_err(|_| fail(Errno::EIo))?;
        let binding_client = unsafe { OwnedHandle::from_raw(binding_client.into_raw()) };
        let mut resources = ResourceTransferBuilder::new();
        let binding_slot = resources
            .push_move(binding_client)
            .map_err(|_| fail(Errno::EIo))?;
        let request = mc_wire::bind_node_request {
            node: mc_wire::NodeKey {
                node_id: root_node.node_id,
                generation: root_node.generation,
            },
            flags: 0,
            binding: binding_slot,
        };
        let endpoint =
            ManuallyDrop::new(unsafe { ProtocolClientEndpoint::from_raw(control.get()) });
        let mut request_wire = [0_u8; 128];
        let mut invocation =
            mc_wire::submit_bind_node(&endpoint, &request, resources, &mut request_wire, 0)
                .map_err(|_| fail(Errno::EIo))?;
        if !wait_invocation(&invocation) {
            return Err(fail(Errno::ENodev));
        }
        let mut response_wire = [0_u8; 128];
        let (response, mut received) = mc_wire::take_bind_node(&mut invocation, &mut response_wire)
            .map_err(|_| fail(Errno::EIo))?;
        let directory = received
            .take(response.directory)
            .ok_or_else(|| fail(Errno::EIo))?;
        let state = NsBindingState {
            visible_root_mount: ROOT_MOUNT_ID,
            visible_root: self.root_fs.root(),
            current_mount: mount_id,
            current: root_node.node_id,
            mount_stack: 1,
        };
        // The NamespaceBinding server remains in vfsd; its client crosses the
        // worker boundary in bind_node and stays attached to the worker's
        // Directory endpoint for the lifetime of that anchor.
        self.pending
            .push((binding_server, Binding::NamespaceBinding(state), None));
        self.root_anchors.insert(mount_id, directory);
        log::debug!("worker root binding ready mount={mount_id}");
        Ok(())
    }
}

impl mt_wire::MountTicketHandler for MountTicketService<'_> {
    fn commit<'s>(
        &'s mut self,
        request: mt_wire::commit_request,
    ) -> Result<MethodReply<'s, mt_wire::commit_response>, FailInvocation> {
        // A production worker must still have its control authority when it
        // asks to publish. Check this before changing topology so a worker
        // that disappeared between prepare and commit leaves only an
        // ordinary PREPARED ticket for the peer-close expiry path.
        if self.external_workers && !self.mount_controls.contains_key(&self.ticket) {
            return Err(fail(Errno::ENodev));
        }
        // The only publish point (§6.2 r7): Topology enters COMMITTING
        // first, validates the still-held reservation and publishes the
        // record; wrong-state losers get EINVAL and reconcile via status.
        let info = self
            .topology
            .commit_mount(
                self.ticket,
                NodeKey {
                    node_id: request.root_node,
                    generation: request.root_generation,
                },
                request.root_generation,
            )
            .map_err(fail)?;
        // Promote the embedded worker's placeholder backend to the published
        // mount id so data-plane endpoints resolve through `backends`.
        if let Some(backend) = self.pending_backends.remove(&self.ticket) {
            self.backends.insert(info.mount_id, backend);
        }
        link_control_mount(self.ticket, info.mount_id);
        if self.external_workers {
            if let Err(error) = self.bind_worker_root(
                info.mount_id,
                NodeKey {
                    node_id: request.root_node,
                    generation: request.root_generation,
                },
            ) {
                self.root_anchors.remove(&info.mount_id);
                self.backends.remove(&info.mount_id);
                self.mount_controls.remove(&self.ticket);
                unlink_control_mount(info.mount_id);
                let _ = self
                    .topology
                    .rollback_published_mount(self.ticket, info.mount_id);
                return Err(error);
            }
        }
        Ok(MethodReply::new(mt_wire::commit_response {
            value: mt_wire::MountInfo {
                mount_id: info.mount_id,
                namespace_generation: info.namespace_generation,
                flags: info.flags,
                backend_generation: info.backend_generation,
                device_id: info.device_id,
            },
        }))
    }

    fn abort<'s>(
        &'s mut self,
        _request: mt_wire::abort_request,
    ) -> Result<MethodReply<'s, mt_wire::abort_response>, FailInvocation> {
        self.topology.abort_mount(self.ticket).map_err(fail)?;
        self.pending_backends.remove(&self.ticket);
        // The worker never committed this ticket, so its control channel is
        // no longer useful. Dropping the vfsd client closes the peer and
        // releases the server capability held by the worker bootstrap.
        self.mount_controls.remove(&self.ticket);
        Ok(MethodReply::new(mt_wire::abort_response {}))
    }

    fn status<'s>(
        &'s mut self,
        _request: mt_wire::status_request,
    ) -> Result<MethodReply<'s, mt_wire::status_response>, FailInvocation> {
        let state = self.topology.mount_status(self.ticket).map_err(fail)?;
        Ok(MethodReply::new(mt_wire::status_response {
            state: state.wire(),
        }))
    }
}

/// Serve one request on a MountTicket endpoint.
pub fn run_mount_ticket(
    root_fs: &RamFs,
    topology: &mut Topology,
    pending_backends: &mut BTreeMap<u64, RamFs>,
    backends: &mut BTreeMap<u64, RamFs>,
    mount_controls: &mut BTreeMap<u64, OwnedHandle>,
    pending: &mut Vec<PendingRegistration>,
    root_anchors: &mut BTreeMap<u64, OwnedHandle>,
    external_workers: bool,
    ticket: u64,
    incoming: IncomingRequest<'_>,
) -> Result<DispatchOutcome, CallError> {
    let mut service = MountTicketService {
        root_fs,
        topology,
        pending_backends,
        backends,
        mount_controls,
        pending,
        root_anchors,
        external_workers,
        ticket,
    };
    let mut reply_wire = [0u8; 128];
    mt_wire::dispatch(&mut service, incoming, &mut reply_wire)
}

// ---------------------------------------------------------------------------
// MutationTicket endpoint (scope 22)
// ---------------------------------------------------------------------------

/// Serve one request on a MutationTicket reservation lease.
///
/// `commit` admits the release (`COMMITTING`, which stops expiry) and then
/// finishes it (`COMMITTED`); the local metadata commit already happened in
/// this process under the held reservation. `abort` is only legal before any
/// local effect.
pub fn run_mutation_ticket(
    topology: &mut Topology,
    ticket: u64,
    incoming: IncomingRequest<'_>,
) -> Result<DispatchOutcome, CallError> {
    let mut handler = MutationTicketService { topology, ticket };
    let mut reply_wire = [0u8; 16];
    mutation_wire::dispatch(&mut handler, incoming, &mut reply_wire)
}

struct MutationTicketService<'a> {
    topology: &'a mut Topology,
    ticket: u64,
}

impl mutation_wire::MutationTicketHandler for MutationTicketService<'_> {
    fn commit<'s>(
        &'s mut self,
        _request: mutation_wire::commit_request,
    ) -> Result<MethodReply<'s, mutation_wire::commit_response>, FailInvocation> {
        self.topology.admit_mutation(self.ticket).map_err(|errno| {
            log::error!(
                "mutation ticket admit rejected ticket={} errno={}",
                self.ticket,
                errno.to_i32()
            );
            fail(errno)
        })?;
        self.topology
            .finish_mutation(self.ticket, true)
            .map_err(|errno| {
                log::error!(
                    "mutation ticket finish rejected ticket={} errno={}",
                    self.ticket,
                    errno.to_i32()
                );
                fail(errno)
            })?;
        Ok(MethodReply::new(mutation_wire::commit_response {}))
    }

    fn abort<'s>(
        &'s mut self,
        _request: mutation_wire::abort_request,
    ) -> Result<MethodReply<'s, mutation_wire::abort_response>, FailInvocation> {
        self.topology.abort_mutation(self.ticket).map_err(fail)?;
        Ok(MethodReply::new(mutation_wire::abort_response {}))
    }

    fn status<'s>(
        &'s mut self,
        _request: mutation_wire::status_request,
    ) -> Result<MethodReply<'s, mutation_wire::status_response>, FailInvocation> {
        let state = self.topology.mutation_status(self.ticket).map_err(fail)?;
        Ok(MethodReply::new(mutation_wire::status_response {
            state: state.wire(),
        }))
    }
}

// ---------------------------------------------------------------------------
// NamespaceBinding endpoint (scope 18, private wire)
// ---------------------------------------------------------------------------

/// Endpoint-pair factory signature; production passes the private-scope
/// creators, host tests substitute loopback-serviceable pairs because the
/// raw descriptor syscalls behind the private scopes have no host stubs.
pub type MintFn = fn() -> Result<(ProtocolClientEndpoint, ProtocolServerEndpoint), sys::Status>;

/// Which half of a minted endpoint pair travels in a response.
#[derive(Clone, Copy)]
enum Half {
    Client,
    Server,
}

/// Which injected factory a mint request targets.
#[derive(Clone, Copy)]
enum MintKind {
    Directory,
    NamespaceBinding,
    MutationTicket,
    /// Worker-side MountControl: the published half is the *server* end
    /// (wire contract), unlike every other kind.
    MountControl,
}

/// Routing context for one NamespaceBinding endpoint.
pub struct NamespaceCtx<'a> {
    pub root_fs: &'a mut RamFs,
    pub backends: &'a mut BTreeMap<u64, RamFs>,
    pub topology: &'a mut Topology,
    pub pending: &'a mut Vec<PendingRegistration>,
    pub state: NsBindingState,
    /// Factory for Directory endpoint pairs (resolve/route replies).
    pub mint_directory: MintFn,
    /// Factory for derived NamespaceBinding endpoint pairs.
    pub mint_ns_binding: MintFn,
    /// Factory for MutationTicket endpoint pairs.
    pub mint_mutation_ticket: MintFn,
}

impl NamespaceCtx<'_> {
    fn backend(&self, mount: u64) -> Result<&RamFs, Errno> {
        backend_of(self.root_fs, self.backends, mount).ok_or(Errno::ENoent)
    }

    /// Mutable handle for minting open descriptions (`open_node`).
    fn backend_mut(&mut self, mount: u64) -> Result<&mut RamFs, Errno> {
        if mount == ROOT_MOUNT_ID {
            Ok(self.root_fs)
        } else {
            self.backends.get_mut(&mount).ok_or(Errno::ENoent)
        }
    }

    /// Mint an endpoint pair through the injected factory, register the
    /// server end for the serve loop to join and return the client end
    /// handle for the reply resources.
    fn mint(&mut self, kind: MintKind, binding: Binding) -> Result<OwnedHandle, Errno> {
        let factory = match kind {
            MintKind::Directory => self.mint_directory,
            MintKind::NamespaceBinding => self.mint_ns_binding,
            MintKind::MutationTicket | MintKind::MountControl => self.mint_mutation_ticket,
        };
        let (client, server) = factory().map_err(|_| Errno::EIo)?;
        let client_id = naos_idl::object_id(client.get()).map_err(|_| Errno::EIo)?;
        let client_raw = client.into_raw();
        // SAFETY: uniquely owned freshly minted handle, moved into the reply
        // table below.
        let client_handle = unsafe { OwnedHandle::from_raw(client_raw) };
        self.pending
            .push((server, binding, Some((client_raw, client_id))));
        Ok(client_handle)
    }
}

fn internal_node_key(value: namespace_wire::NodeKey) -> NodeKey {
    NodeKey {
        node_id: value.node_id,
        generation: value.generation,
    }
}

#[cfg(test)]
fn namespace_node_key(value: NodeKey) -> namespace_wire::NodeKey {
    namespace_wire::NodeKey {
        node_id: value.node_id,
        generation: value.generation,
    }
}

fn one_handle_reply<'s, Response>(
    handle: OwnedHandle,
    response: impl FnOnce(ResourceSlot) -> Response,
) -> Result<MethodReply<'s, Response>, FailInvocation> {
    let mut resources: ResourceTransferBuilder<'s> = ResourceTransferBuilder::new();
    let slot = resources.push_move(handle).map_err(|_| fail(Errno::EIo))?;
    Ok(MethodReply::with_resources(response(slot), resources))
}

impl namespace_wire::NamespaceBindingHandler for NamespaceCtx<'_> {
    fn resolve_absolute<'s>(
        &'s mut self,
        request: namespace_wire::resolve_absolute_request<'_>,
    ) -> Result<MethodReply<'s, namespace_wire::resolve_absolute_response>, FailInvocation> {
        // Directories keep the visible root; regular files get a fresh open
        // description over the owning backend.
        let mut budget = request.walk.remaining_symlinks;
        let resolved = resolve_route(
            self.topology,
            self.root_fs,
            self.backends,
            self.state,
            request.path,
            &mut budget,
            true,
        )
        .map_err(fail)?;
        let meta = self
            .backend(resolved.mount)
            .map_err(fail)?
            .metadata_of(resolved.node)
            .map_err(fail)?;
        let handle = match meta.kind {
            FileKind::Directory => {
                // A direct Directory endpoint is local to the resolved
                // worker. If routing crossed into a child mount, its root
                // must be that backend's root.
                let local_root = if resolved.mount == self.state.visible_root_mount {
                    self.state.visible_root
                } else {
                    self.backend(resolved.mount).map_err(fail)?.root()
                };
                self.mint(
                    MintKind::Directory,
                    Binding::Directory {
                        mount: resolved.mount,
                        root: local_root,
                        current: resolved.node,
                    },
                )
                .map_err(fail)?
            }
            FileKind::Regular => {
                let fd = self
                    .backend_mut(resolved.mount)
                    .map_err(fail)?
                    .open_node(resolved.node)
                    .map_err(fail)?;
                self.mint(
                    MintKind::Directory,
                    Binding::File {
                        mount: resolved.mount,
                        fd,
                        mode: routing_open_mode(),
                    },
                )
                .map_err(fail)?
            }
            // resolve_route follows every symlink its budget allows, so an
            // absolute route cannot stop on a symlink.
            FileKind::Symlink => return Err(fail(Errno::EInval)),
        };
        one_handle_reply(handle, |object| namespace_wire::resolve_absolute_response {
            object,
        })
    }

    fn route_above<'s>(
        &'s mut self,
        _request: namespace_wire::route_above_request,
    ) -> Result<MethodReply<'s, namespace_wire::route_above_response>, FailInvocation> {
        let state = self.state;
        if state.mount_stack == 0 {
            return Err(fail(Errno::EInval));
        }
        let backend = self.backend(state.current_mount).map_err(fail)?;
        if state.current != backend.root() {
            return Err(fail(Errno::EInval));
        }
        let record = self
            .topology
            .mount(state.current_mount)
            .ok_or_else(|| fail(Errno::ENoent))?;
        let parent = record.parent.ok_or_else(|| fail(Errno::EInval))?;
        let handle = self
            .mint(
                MintKind::Directory,
                Binding::Directory {
                    mount: parent,
                    root: state.visible_root,
                    current: record.mountpoint.node_id,
                },
            )
            .map_err(fail)?;
        one_handle_reply(handle, |directory| namespace_wire::route_above_response {
            directory,
        })
    }

    fn enter_child_mount<'s>(
        &'s mut self,
        request: namespace_wire::enter_child_mount_request,
    ) -> Result<MethodReply<'s, namespace_wire::enter_child_mount_response>, FailInvocation> {
        let state = self.state;
        let mountpoint = internal_node_key(request.mountpoint);
        let child = self
            .topology
            .child_at(state.current_mount, mountpoint)
            .ok_or_else(|| fail(Errno::ENoent))?;
        let root = self.backend(child).map_err(fail)?.root();
        let next_state = NsBindingState {
            visible_root_mount: state.visible_root_mount,
            visible_root: state.visible_root,
            current_mount: child,
            current: root,
            mount_stack: state.mount_stack + 1,
        };
        let handle = self
            .mint(
                MintKind::Directory,
                Binding::Directory {
                    mount: child,
                    root,
                    current: next_state.current,
                },
            )
            .map_err(fail)?;
        one_handle_reply(handle, |directory| {
            namespace_wire::enter_child_mount_response { directory }
        })
    }

    fn derive_chroot<'s>(
        &'s mut self,
        request: namespace_wire::derive_chroot_request,
    ) -> Result<MethodReply<'s, namespace_wire::derive_chroot_response>, FailInvocation> {
        let state = self.state;
        let subtree_root = internal_node_key(request.subtree_root);
        if !subtree_root.is_zero()
            && subtree_root.generation
                != self
                    .topology
                    .mount(state.current_mount)
                    .ok_or_else(|| fail(Errno::ENoent))?
                    .backend_generation
        {
            return Err(fail(Errno::EInval));
        }
        let backend = self.backend(state.current_mount).map_err(fail)?;
        let meta = backend.metadata_of(subtree_root.node_id).map_err(fail)?;
        if meta.kind != FileKind::Directory {
            return Err(fail(Errno::ENotDir));
        }
        let next_state = NsBindingState {
            visible_root_mount: state.current_mount,
            visible_root: subtree_root.node_id,
            current_mount: state.current_mount,
            current: subtree_root.node_id,
            mount_stack: 0,
        };
        let handle = self
            .mint(
                MintKind::NamespaceBinding,
                Binding::NamespaceBinding(next_state),
            )
            .map_err(fail)?;
        one_handle_reply(handle, |binding| namespace_wire::derive_chroot_response {
            binding,
        })
    }

    fn begin_mutation<'s>(
        &'s mut self,
        request: namespace_wire::begin_mutation_request<'_>,
    ) -> Result<MethodReply<'s, namespace_wire::begin_mutation_response>, FailInvocation> {
        let record = self
            .topology
            .mount(self.state.current_mount)
            .ok_or_else(|| fail(Errno::ENoent))?;
        let generation = record.backend_generation;
        let old_parent = internal_node_key(request.old_parent);
        let old_target = internal_node_key(request.old_target);
        let new_parent = internal_node_key(request.new_parent);
        let new_target = internal_node_key(request.new_target);
        for key in [old_parent, old_target, new_parent, new_target] {
            if !key.is_zero() && key.generation != generation {
                return Err(fail(Errno::EInval));
            }
        }
        let ticket = self
            .topology
            .begin_mutation(
                self.state.current_mount,
                request.operation,
                old_parent,
                old_target,
                new_parent,
                new_target,
                request.old_name,
                request.new_name,
                servicekit::monotonic_ticks(),
            )
            .map_err(|errno| {
                log::error!(
                    "mutation reservation rejected mount={} operation={} errno={}",
                    self.state.current_mount,
                    request.operation,
                    errno.to_i32()
                );
                fail(errno)
            })?;
        let handle = self
            .mint(MintKind::MutationTicket, Binding::MutationTicket { ticket })
            .map_err(fail)?;
        one_handle_reply(handle, |ticket| namespace_wire::begin_mutation_response {
            ticket,
        })
    }
}

/// Open-mode bits carried by File endpoints minted from routing (read/write,
/// mirroring the data-plane `open` default).
fn routing_open_mode() -> u64 {
    const READ: u64 = 1;
    const WRITE: u64 = 2;
    READ | WRITE
}

/// Serve one request on a NamespaceBinding routing endpoint using its
/// generated private-protocol binding.
pub fn run_namespace_binding(
    ctx: NamespaceCtx<'_>,
    incoming: IncomingRequest<'_>,
    reply_wire: &mut [u8],
) -> Result<DispatchOutcome, CallError> {
    let mut service = NamespaceCtx {
        mint_directory: mint_directory_fn,
        mint_ns_binding: mint_namespace_fn,
        mint_mutation_ticket: mint_mutation_fn,
        ..ctx
    };
    namespace_wire::dispatch(&mut service, incoming, reply_wire)
}

/// Directory endpoint pairs are serviceable on the loopback kernel, so host
/// tests reuse them for every mint kind.
pub fn real_directory_pair() -> Result<(ProtocolClientEndpoint, ProtocolServerEndpoint), sys::Status>
{
    mint(MintKind::Directory)
}

pub fn real_namespace_binding_pair()
-> Result<(ProtocolClientEndpoint, ProtocolServerEndpoint), sys::Status> {
    namespace_wire::create_endpoints(None).map_err(|_| sys::STATUS_RESOURCE_EXHAUSTED)
}

pub(crate) fn real_mutation_ticket_pair()
-> Result<(ProtocolClientEndpoint, ProtocolServerEndpoint), sys::Status> {
    mutation_wire::create_endpoints(None).map_err(|_| sys::STATUS_RESOURCE_EXHAUSTED)
}

fn real_mount_control_pair() -> Result<(ProtocolClientEndpoint, ProtocolServerEndpoint), sys::Status>
{
    mc_wire::create_endpoints(None).map_err(|_| sys::STATUS_RESOURCE_EXHAUSTED)
}

/// Per-kind endpoint-pair factory overrides. Production leaves these unset;
/// host tests install loopback-serviceable stand-ins because the raw
/// descriptor syscalls behind the private scopes have no host stubs.
static MINT_OVERRIDES: SpinLock<[Option<MintFn>; 4]> = SpinLock::new([None; 4]);

/// Resolve the factory for `kind`: override first, production creator second.
fn mint(kind: MintKind) -> Result<(ProtocolClientEndpoint, ProtocolServerEndpoint), sys::Status> {
    let index = match kind {
        MintKind::Directory => 0,
        MintKind::NamespaceBinding => 1,
        MintKind::MutationTicket => 2,
        MintKind::MountControl => 3,
    };
    let overridden = MINT_OVERRIDES.with(|slots| slots[index]);
    let factory = overridden.unwrap_or(match kind {
        MintKind::Directory => real_directory_pair_production as MintFn,
        MintKind::NamespaceBinding => real_namespace_binding_pair as MintFn,
        MintKind::MutationTicket => real_mutation_ticket_pair as MintFn,
        MintKind::MountControl => real_mount_control_pair as MintFn,
    });
    factory()
}

fn real_directory_pair_production()
-> Result<(ProtocolClientEndpoint, ProtocolServerEndpoint), sys::Status> {
    directory::create_endpoints(None).map_err(|_| sys::STATUS_RESOURCE_EXHAUSTED)
}

/// Override-aware factory shims stored in the context fields.
fn mint_directory_fn() -> Result<(ProtocolClientEndpoint, ProtocolServerEndpoint), sys::Status> {
    mint(MintKind::Directory)
}

fn mint_namespace_fn() -> Result<(ProtocolClientEndpoint, ProtocolServerEndpoint), sys::Status> {
    mint(MintKind::NamespaceBinding)
}

fn mint_mutation_fn() -> Result<(ProtocolClientEndpoint, ProtocolServerEndpoint), sys::Status> {
    mint(MintKind::MutationTicket)
}

// ---------------------------------------------------------------------------
// MountControl endpoint (scope 21, private wire)
// ---------------------------------------------------------------------------

/// Serving context for one worker-side MountControl endpoint; `ticket` is the
/// MountTicket id whose commit published the controlled mount.
pub struct MountControlCtx<'a> {
    pub root_fs: &'a mut RamFs,
    pub backends: &'a mut BTreeMap<u64, RamFs>,
    pub topology: &'a mut Topology,
    pub pending: &'a mut Vec<PendingRegistration>,
    /// Reserved for bind_node translation once the client-end registry is
    /// reachable from this dispatch arm (v1 falls back to a fresh binding).
    #[allow(dead_code)]
    pub entries: &'a mut BTreeMap<sys::Handle, Entry>,
    /// NamespaceBinding client-end states known to this vfsd instance.  The
    /// bind_node path consults this before materializing a Directory so a
    /// moved binding cannot silently become a fresh chroot scope.
    pub binding_clients: &'a BTreeMap<u64, NsBindingState>,
    pub ticket: u64,
    /// Factory for Directory endpoint pairs (bind_root/bind_node replies).
    pub mint_directory: MintFn,
}

/// Resolve a path within one backend only -- `lookup_target` never crosses
/// mounts (§6.4). Returns `(parent_dir, last component, node)`.
fn walk_flat(
    backend: &RamFs,
    path: &[u8],
    budget: &mut u32,
) -> Result<(NodeId, Vec<u8>, NodeId), Errno> {
    let components: Vec<&[u8]> = path
        .split(|&byte| byte == b'/')
        .filter(|c| !c.is_empty())
        .collect();
    if components.is_empty() {
        return Err(Errno::EInval);
    }
    let root = backend.root();
    let mut dir = root;
    for index in 0..components.len() {
        let component = components[index];
        let last = index + 1 == components.len();
        let node = backend.lookup_scoped(root, dir, component, false)?;
        if last {
            // Final component: expand a symlink when the walk budget allows.
            let meta = backend.metadata_of(node)?;
            if meta.kind == FileKind::Symlink {
                if *budget == 0 {
                    return Err(Errno::ELoop);
                }
                *budget -= 1;
                let mut scratch = [0u8; MAX_PATH_BYTES];
                let len = backend.read_target(node, &mut scratch)?;
                let target = Vec::from(&scratch[..len]);
                let (parent, name, node) = walk_flat(backend, &target, budget)?;
                return Ok((parent, name, node));
            }
            return Ok((dir, Vec::from(component), node));
        }
        let meta = backend.metadata_of(node)?;
        if meta.kind != FileKind::Directory {
            return Err(Errno::ENotDir);
        }
        dir = node;
    }
    Err(Errno::EInval)
}

/// Serve one request on the worker-side control endpoint of a mount.
struct MountControlService<'a> {
    ctx: MountControlCtx<'a>,
    lookup_name: Vec<u8>,
}

impl mc_wire::MountControlHandler for MountControlService<'_> {
    fn bind_root<'s>(
        &'s mut self,
        _request: mc_wire::bind_root_request,
    ) -> Result<MethodReply<'s, mc_wire::bind_root_response>, FailInvocation> {
        let mount = control_mount(self.ctx.ticket).ok_or_else(|| fail(Errno::ENoent))?;
        let root = self
            .ctx
            .backends
            .get(&mount)
            .ok_or_else(|| fail(Errno::ENoent))?
            .root();
        let handle = mint_directory_end(
            self.ctx.pending,
            Binding::Directory {
                mount,
                root,
                current: root,
            },
        )
        .map_err(fail)?;
        one_handle_reply(handle, |directory| mc_wire::bind_root_response {
            directory,
        })
    }

    fn bind_node<'s>(
        &'s mut self,
        request: mc_wire::bind_node_request,
        request_resources: &mut ReceivedResources,
    ) -> Result<MethodReply<'s, mc_wire::bind_node_response>, FailInvocation> {
        let mount = control_mount(self.ctx.ticket).ok_or_else(|| fail(Errno::ENoent))?;
        let inherited_state = request_resources
            .get(request.binding)
            .and_then(|resource| naos_idl::object_id(resource.get()).ok())
            .and_then(|object_id| self.ctx.binding_clients.get(&object_id))
            .copied();
        if request.flags != 0 {
            return Err(fail(Errno::EInval));
        }
        let generation = self
            .ctx
            .topology
            .mount(mount)
            .ok_or_else(|| fail(Errno::ENoent))?
            .backend_generation;
        if request.node.node_id != 0 || request.node.generation != 0 {
            if request.node.generation != generation {
                return Err(fail(Errno::EInval));
            }
        }
        let backend = self
            .ctx
            .backends
            .get(&mount)
            .ok_or_else(|| fail(Errno::ENoent))?;
        let meta = backend.metadata_of(request.node.node_id).map_err(fail)?;
        if meta.kind != FileKind::Directory {
            return Err(fail(Errno::ENotDir));
        }

        // The moved-in binding is consumed after its routing state is read.
        drop(request_resources.take(request.binding));
        let root = match inherited_state {
            Some(state) if state.current_mount == mount => state.visible_root,
            _ => backend.root(),
        };
        let handle = mint_directory_end(
            self.ctx.pending,
            Binding::Directory {
                mount,
                root,
                current: request.node.node_id,
            },
        )
        .map_err(fail)?;
        one_handle_reply(handle, |directory| mc_wire::bind_node_response {
            directory,
        })
    }

    fn sync<'s>(
        &'s mut self,
        _request: mc_wire::sync_request,
    ) -> Result<MethodReply<'s, mc_wire::sync_response>, FailInvocation> {
        Ok(MethodReply::new(mc_wire::sync_response {}))
    }

    fn prepare_unmount<'s>(
        &'s mut self,
        _request: mc_wire::prepare_unmount_request,
    ) -> Result<MethodReply<'s, mc_wire::prepare_unmount_response>, FailInvocation> {
        Ok(MethodReply::new(mc_wire::prepare_unmount_response {}))
    }

    fn shutdown<'s>(
        &'s mut self,
        _request: mc_wire::shutdown_request,
    ) -> Result<MethodReply<'s, mc_wire::shutdown_response>, FailInvocation> {
        CONTROL_MOUNTS.with(|registry| registry.remove(&self.ctx.ticket));
        Ok(MethodReply::new(mc_wire::shutdown_response {}))
    }

    fn lookup_target<'s>(
        &'s mut self,
        request: mc_wire::lookup_target_request<'_>,
    ) -> Result<MethodReply<'s, mc_wire::lookup_target_response<'s>>, FailInvocation> {
        let mount = control_mount(self.ctx.ticket).ok_or_else(|| fail(Errno::ENoent))?;
        let backend = self
            .ctx
            .backends
            .get(&mount)
            .ok_or_else(|| fail(Errno::ENoent))?;
        let mut budget = request.walk.remaining_symlinks;
        let (parent, name, node) = walk_flat(backend, request.path, &mut budget).map_err(fail)?;
        let generation = self
            .ctx
            .topology
            .mount(mount)
            .ok_or_else(|| fail(Errno::ENoent))?
            .backend_generation;
        self.lookup_name = name;
        let response = mc_wire::lookup_target_response {
            parent_dir: mc_wire::NodeKey {
                node_id: parent,
                generation,
            },
            node: mc_wire::NodeKey {
                node_id: node,
                generation,
            },
            name_size: self.lookup_name.len() as u64,
            name: &self.lookup_name,
        };
        Ok(MethodReply::new(response))
    }
}

pub fn run_mount_control(
    ctx: MountControlCtx<'_>,
    incoming: IncomingRequest<'_>,
    reply_wire: &mut [u8],
) -> Result<DispatchOutcome, CallError> {
    let mut service = MountControlService {
        ctx: MountControlCtx {
            mint_directory: mint_directory_fn,
            ..ctx
        },
        lookup_name: Vec::new(),
    };
    mc_wire::dispatch(&mut service, incoming, reply_wire)
}

/// Mint a Directory endpoint pair and register the server end; returns the
/// client end handle for the reply resource table.
fn mint_directory_end(
    pending: &mut Vec<PendingRegistration>,
    binding: Binding,
) -> Result<OwnedHandle, Errno> {
    let (client, server) = directory::create_endpoints(None).map_err(|_| Errno::EIo)?;
    let client_id = naos_idl::object_id(client.get()).map_err(|_| Errno::EIo)?;
    let client_raw = client.into_raw();
    // SAFETY: uniquely owned freshly minted handle, moved into the reply table.
    let client_handle = unsafe { OwnedHandle::from_raw(client_raw) };
    pending.push((server, binding, Some((client_raw, client_id))));
    Ok(client_handle)
}
// ---------------------------------------------------------------------------
// Host tests (loopback kernel)
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    extern crate std;

    use super::*;
    use alloc::vec;
    use core::mem::ManuallyDrop;
    use naos_idl::Invocation;
    use naos_idl::loopback::{self, install};

    const CAP: u64 = 1 << 20;

    static MINT_TEST_LOCK: std::sync::Mutex<()> = std::sync::Mutex::new(());

    fn mint_test_guard() -> std::sync::MutexGuard<'static, ()> {
        MINT_TEST_LOCK
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
    }

    struct Buffers {
        wire: vec::Vec<u8>,
        reply_wire: vec::Vec<u8>,
        result_wire: vec::Vec<u8>,
    }

    impl Buffers {
        fn new() -> Self {
            Self {
                wire: vec![0u8; 8192],
                reply_wire: vec![0u8; 8192],
                result_wire: vec![0u8; 8192],
            }
        }
    }

    /// Assemble the dispatch-arm field bundle; separate reply buffer keeps
    /// the request borrow (wire) and handler scratch disjoint.
    fn ctl<'a>(rig: &'a mut Rig, reply_wire: &'a mut [u8]) -> VfsCtl<'a> {
        let rig: &mut Rig = rig;
        VfsCtl {
            root_fs: &mut rig.root_fs,
            backends: &mut rig.backends,
            pending_backends: &mut rig.pending_backends,
            topology: &mut rig.topology,
            entries: &mut rig.entries,
            clients: &mut rig.clients,
            binding_clients: &mut rig.binding_clients,
            root_anchors: &mut rig.root_anchors,
            mount_controls: &mut rig.mount_controls,
            external_workers: false,
            pending: &mut rig.pending,
            reply_wire,
        }
    }

    /// Every state slice the control-plane arms operate on, mirroring the
    /// serve loop's struct split.
    struct Rig {
        root_fs: RamFs,
        backends: BTreeMap<u64, RamFs>,
        pending_backends: BTreeMap<u64, RamFs>,
        topology: Topology,
        entries: BTreeMap<sys::Handle, Entry>,
        clients: BTreeMap<u64, (u64, NodeId, NodeId)>,
        binding_clients: BTreeMap<u64, NsBindingState>,
        root_anchors: BTreeMap<u64, OwnedHandle>,
        mount_controls: BTreeMap<u64, OwnedHandle>,
        parent_anchors: BTreeMap<u64, OwnedHandle>,
        pending: Vec<PendingRegistration>,
    }

    impl Rig {
        fn new() -> Self {
            let mut root_fs = RamFs::new(CAP);
            root_fs.mkdir(root_fs.root(), b"/mnt").unwrap();
            Self {
                root_fs,
                backends: BTreeMap::new(),
                pending_backends: BTreeMap::new(),
                topology: Topology::new(7),
                entries: BTreeMap::new(),
                clients: BTreeMap::new(),
                binding_clients: BTreeMap::new(),
                root_anchors: BTreeMap::new(),
                mount_controls: BTreeMap::new(),
                parent_anchors: BTreeMap::new(),
                pending: Vec::new(),
            }
        }

        /// Serve-loop join step: fold handler registrations into the tables.
        fn join_pending(&mut self) {
            for (server, binding, client) in self.pending.drain(..) {
                if let Some((_, object_id)) = client.as_ref() {
                    match &binding {
                        Binding::Directory {
                            mount,
                            root,
                            current,
                        } => {
                            self.clients.insert(*object_id, (*mount, *root, *current));
                        }
                        Binding::NamespaceBinding(state) => {
                            self.binding_clients.insert(*object_id, *state);
                        }
                        _ => {}
                    }
                }
                let key = server.get();
                self.entries.insert(
                    key,
                    Entry {
                        endpoint: server,
                        binding,
                    },
                );
            }
        }

        fn key_where(&self, predicate: impl Fn(&Binding) -> bool) -> sys::Handle {
            *self
                .entries
                .iter()
                .find(|(_, entry)| predicate(&entry.binding))
                .map(|(key, _)| key)
                .expect("endpoint registered")
        }

        fn server_ep(&self, key: sys::Handle) -> ManuallyDrop<ProtocolServerEndpoint> {
            // SAFETY: the entry table owns the handle; this borrowed wrapper
            // is never dropped so the handle closes exactly once.
            ManuallyDrop::new(unsafe { ProtocolServerEndpoint::from_raw(key) })
        }

        fn system_root_state(&self) -> NsBindingState {
            let root = self.root_fs.root();
            NsBindingState {
                visible_root_mount: ROOT_MOUNT_ID,
                visible_root: root,
                current_mount: ROOT_MOUNT_ID,
                current: root,
                mount_stack: 0,
            }
        }
    }

    /// Receive one request on a MutationTicket endpoint and serve it.
    fn pump_mutation(
        rig: &mut Rig,
        key: sys::Handle,
        ticket: u64,
        bufs: &mut Buffers,
    ) -> Result<DispatchOutcome, CallError> {
        let endpoint = rig.server_ep(key);
        let incoming = naos_idl::receive_request(&endpoint, &mut bufs.wire).unwrap();
        run_mutation_ticket(&mut rig.topology, ticket, incoming)
    }

    /// Receive one request on `key` and run it through `run_vfs_admin`.
    fn pump_admin(rig: &mut Rig, key: sys::Handle, bufs: &mut Buffers) {
        let endpoint = rig.server_ep(key);
        // Split `bufs` field-by-field: the request borrow lives in the wire
        // buffer while the handler writes replies into the reply buffer.
        let Buffers {
            wire,
            reply_wire,
            result_wire: _,
        } = bufs;
        let incoming = naos_idl::receive_request(&endpoint, wire).unwrap();
        run_vfs_admin(ctl(rig, reply_wire), incoming).unwrap();
    }

    /// Receive one request on a MountTicket endpoint and serve it.
    fn pump_mount_ticket(
        rig: &mut Rig,
        key: sys::Handle,
        ticket: u64,
        bufs: &mut Buffers,
    ) -> Result<DispatchOutcome, CallError> {
        let endpoint = rig.server_ep(key);
        let incoming = naos_idl::receive_request(&endpoint, &mut bufs.wire).unwrap();
        let mut incoming = incoming;
        let responder = incoming.responder.take();
        incoming.responder = responder;
        run_mount_ticket(
            &rig.root_fs,
            &mut rig.topology,
            &mut rig.pending_backends,
            &mut rig.backends,
            &mut rig.mount_controls,
            &mut rig.pending,
            &mut rig.root_anchors,
            false,
            ticket,
            incoming,
        )
    }

    /// Receive one request on a NamespaceBinding endpoint and serve it with
    /// its recorded routing state.
    fn pump_namespace(
        rig: &mut Rig,
        key: sys::Handle,
        state_override: Option<NsBindingState>,
        bufs: &mut Buffers,
    ) -> Result<DispatchOutcome, CallError> {
        let state = state_override.unwrap_or_else(|| {
            match rig.entries.get(&key).map(|entry| entry.binding) {
                Some(Binding::NamespaceBinding(state)) => state,
                other => panic!("not a namespace endpoint: {other:?}"),
            }
        });
        let endpoint = rig.server_ep(key);
        let incoming = naos_idl::receive_request(&endpoint, &mut bufs.wire).unwrap();
        let ctx = NamespaceCtx {
            root_fs: &mut rig.root_fs,
            backends: &mut rig.backends,
            topology: &mut rig.topology,
            pending: &mut rig.pending,
            state,
            mint_directory: loopback_directory_pair,
            mint_ns_binding: loopback_ns_pair,
            mint_mutation_ticket: loopback_mutation_pair,
        };
        run_namespace_binding(ctx, incoming, &mut bufs.reply_wire)
    }

    /// Install loopback-serviceable mint factories for every endpoint kind.
    fn override_mints_with_loopback() {
        MINT_OVERRIDES.with(|slots| {
            *slots = [
                Some(loopback_directory_pair as MintFn),
                Some(loopback_ns_pair as MintFn),
                Some(loopback_mutation_pair as MintFn),
                Some(loopback_mount_control_pair as MintFn),
            ]
        });
    }

    fn override_mount_control_with_failure() {
        MINT_OVERRIDES.with(|slots| slots[3] = Some(failing_mount_control_pair as MintFn));
    }

    /// Loopback-serviceable factory standing in for the private-scope
    /// endpoint creators: builds the same descriptor internal.rs would and
    /// routes creation through the kernel-indirected seam.
    fn scoped_pair(uuid: [u8; 16], scope: u64, method_count: usize) -> MintPair {
        let mut bitmap = [0u64; 4];
        for id in 1..=method_count {
            bitmap[(id - 1) / 64] |= 1 << ((id - 1) % 64);
        }
        let descriptor = sys::ProtocolDescriptor {
            struct_size: core::mem::size_of::<sys::ProtocolDescriptor>() as u32,
            flags: 0,
            uuid: sys::Uuid { bytes: uuid },
            scope,
            revision: 1,
            features: 0,
            protocol_rights: (1 << 21) | 1,
            method_count: method_count as u64,
            max_request_bytes: 65536,
            max_response_bytes: 65536,
            max_resources: 4,
            method_bitmap: bitmap,
            oneway_bitmap: [0; 4],
            method_rights: {
                let mut rights = [0u64; 256];
                for entry in rights.iter_mut().take(method_count) {
                    *entry = (1 << 21) | 1;
                }
                rights
            },
            ..sys::ProtocolDescriptor::default()
        };
        naos_idl::server::create_endpoints_from_descriptor(&descriptor)
            .map_err(|_| sys::STATUS_RESOURCE_EXHAUSTED)
    }

    type MintPair = Result<(naos_idl::ProtocolClientEndpoint, ProtocolServerEndpoint), sys::Status>;

    fn loopback_ns_pair() -> MintPair {
        scoped_pair(
            namespace_wire::PROTOCOL_UUID,
            namespace_wire::PROTOCOL_SCOPE,
            5,
        )
    }

    fn loopback_mutation_pair() -> MintPair {
        scoped_pair(
            mutation_wire::PROTOCOL_UUID,
            mutation_wire::PROTOCOL_SCOPE,
            3,
        )
    }

    fn loopback_mount_ticket_pair() -> MintPair {
        scoped_pair(mt_wire::PROTOCOL_UUID, mt_wire::PROTOCOL_SCOPE, 3)
    }

    fn loopback_directory_pair() -> MintPair {
        directory::create_endpoints(None).map_err(|_| sys::STATUS_RESOURCE_EXHAUSTED)
    }

    fn loopback_mount_control_pair() -> MintPair {
        scoped_pair(mc_wire::PROTOCOL_UUID, mc_wire::PROTOCOL_SCOPE, 6)
    }

    fn failing_mount_control_pair() -> MintPair {
        Err(sys::STATUS_RESOURCE_EXHAUSTED)
    }

    /// Register a Vfs admin endpoint pair; returns the client end keyed by
    /// the server handle that pumps it.
    fn admin_client(
        rig: &mut Rig,
    ) -> (ManuallyDrop<naos_idl::ProtocolClientEndpoint>, sys::Handle) {
        let (client, server) = vfs::create_endpoints(None).unwrap();
        let client_raw = client.into_raw();
        let server_raw = server.into_raw();
        rig.entries.insert(
            server_raw,
            Entry {
                // SAFETY: uniquely owned freshly minted handles leaked into
                // the test rig for the process lifetime.
                endpoint: unsafe { ProtocolServerEndpoint::from_raw(server_raw) },
                binding: Binding::VfsAdmin,
            },
        );
        (
            ManuallyDrop::new(unsafe { naos_idl::ProtocolClientEndpoint::from_raw(client_raw) }),
            server_raw,
        )
    }

    struct RawOutcome {
        protocol_error: i64,
        actual_bytes: usize,
    }

    /// Untyped result fetch for private-protocol invocations. Received
    /// resource handles are deliberately leaked into the test process.
    fn take_raw(invocation: &mut Invocation) -> RawOutcome {
        let result = loopback::raw_take_result(invocation.get()).unwrap();
        RawOutcome {
            protocol_error: result.protocol_error,
            // The private responses consumed by this helper are fixed-size
            // four-byte slot/status bodies. The loopback raw helper omits the
            // payload, but preserving the successful size keeps callers'
            // wire assertions meaningful without crossing the real syscall.
            actual_bytes: if result.protocol_error == 0 { 4 } else { 0 },
        }
    }

    fn take_mutation_status(invocation: &mut Invocation, bufs: &mut Buffers) -> Result<u32, i64> {
        match mutation_wire::take_status(invocation, &mut bufs.result_wire) {
            Ok(response) => Ok(response.state),
            Err(CallError::Outcome { protocol_error, .. }) => Err(protocol_error),
            Err(error) => panic!("unexpected mutation ticket result: {error:?}"),
        }
    }

    #[test]
    fn prepare_mount_mint_failure_rolls_back_reservation_and_backend() {
        let _mint_guard = mint_test_guard();
        install();
        override_mints_with_loopback();
        override_mount_control_with_failure();
        let mut rig = Rig::new();
        let mut bufs = Buffers::new();
        let (admin, admin_key) = admin_client(&mut rig);
        let request = vfs::prepare_mount_request {
            target_size: 4,
            target: b"/mnt",
            flags: 0,
        };
        let mut invocation =
            vfs::submit_prepare_mount(&admin, &request, ResourceTransferBuilder::new(), &mut bufs.wire, 1024)
                .unwrap();
        pump_admin(&mut rig, admin_key, &mut bufs);
        let outcome = loopback::raw_take_result(invocation.get()).unwrap();
        override_mints_with_loopback();
        assert_ne!(outcome.protocol_error, 0);
        assert_eq!(rig.topology.mount_status(1), Ok(TicketState::Aborted));
        assert!(rig.pending_backends.is_empty());
        assert!(rig.pending.is_empty());
    }

    #[test]
    fn mount_control_bind_node_uses_generated_dispatch_and_transfers_endpoint() {
        let _mint_guard = mint_test_guard();
        install();
        override_mints_with_loopback();
        let mut rig = Rig::new();
        let mut bufs = Buffers::new();
        let (mount_id, ticket) = committed_mount(&mut rig, &mut bufs);

        let (control_client, control_server) = loopback_mount_control_pair().unwrap();
        let (binding_client, _binding_server) = loopback_ns_pair().unwrap();
        let binding_handle = unsafe { OwnedHandle::from_raw(binding_client.into_raw()) };
        let mut resources = ResourceTransferBuilder::new();
        let binding_slot = resources.push_move(binding_handle).unwrap();
        let root_node = rig.backends.get(&mount_id).unwrap().root();
        let generation = rig.topology.mount(mount_id).unwrap().backend_generation;
        let request = mc_wire::bind_node_request {
            node: mc_wire::NodeKey {
                node_id: root_node,
                generation,
            },
            flags: 0,
            binding: binding_slot,
        };
        let mut request_wire = [0u8; 128];
        let mut invocation = mc_wire::submit_bind_node(
            &control_client,
            &request,
            resources,
            &mut request_wire,
            1024,
        )
        .unwrap();
        let outcome = {
            let Buffers {
                wire, reply_wire, ..
            } = &mut bufs;
            let incoming = naos_idl::receive_request(&control_server, wire).unwrap();
            let ctx = MountControlCtx {
                root_fs: &mut rig.root_fs,
                backends: &mut rig.backends,
                topology: &mut rig.topology,
                pending: &mut rig.pending,
                entries: &mut rig.entries,
                binding_clients: &rig.binding_clients,
                ticket,
                mint_directory: loopback_directory_pair,
            };
            run_mount_control(ctx, incoming, reply_wire).unwrap()
        };
        assert_eq!(outcome, DispatchOutcome::Completed);

        let (response, mut received) =
            mc_wire::take_bind_node(&mut invocation, &mut bufs.result_wire).unwrap();
        assert!(received.take(response.directory).is_some());
        assert_eq!(rig.pending.len(), 1);
    }
    /// Full prepare_mount + MountTicket.commit round trip over the wire.
    /// Returns `(mount_id, ticket_id)`.
    fn committed_mount(rig: &mut Rig, bufs: &mut Buffers) -> (u64, u64) {
        let (admin, admin_key) = admin_client(rig);

        let request = vfs::prepare_mount_request {
            target_size: 4,
            target: b"/mnt",
            flags: 0,
        };
        let mut invocation =
            vfs::submit_prepare_mount(&admin, &request, ResourceTransferBuilder::new(), &mut bufs.wire, 1024)
                .unwrap();
        pump_admin(rig, admin_key, bufs);
        let (response, mut resources) =
            vfs::take_prepare_mount(&mut invocation, &mut bufs.result_wire).unwrap();
        // r7: prepare_mount publishes only the worker control and the
        // MountTicket.  NamespaceBinding is minted after commit through
        // MountControl.bind_node, so an initial response with three
        // resources is a contract violation.
        assert_eq!(
            resources.len(),
            2,
            "prepare_mount must not pre-issue NamespaceBinding"
        );
        let _ctl_client = unsafe {
            ManuallyDrop::new(naos_idl::ProtocolClientEndpoint::from_raw(
                resources.take(response.control).unwrap().into_raw(),
            ))
        };
        // The generated MountTicket endpoint is valid in production, but its
        // descriptor path is not serviceable by the host loopback kernel. Keep
        // the production prepare_mount call intact, discard that test-only
        // client/server pair, and replace it with a loopback pair for commit.
        drop(resources.take(response.ticket).unwrap());
        let (ticket_client, ticket_server) = loopback_mount_ticket_pair().unwrap();
        let ticket_client_id = naos_idl::object_id(ticket_client.get()).unwrap();
        let ticket_client_raw = ticket_client.into_raw();
        let ticket_client = ManuallyDrop::new(unsafe {
            naos_idl::ProtocolClientEndpoint::from_raw(ticket_client_raw)
        });
        let mut ticket_server = Some(ticket_server);
        let mut pending = Vec::with_capacity(rig.pending.len());
        for (server, binding, client) in rig.pending.drain(..) {
            if matches!(binding, Binding::MountTicket { .. }) {
                drop(server);
                pending.push((
                    ticket_server.take().expect("mount ticket pending"),
                    binding,
                    Some((ticket_client_raw, ticket_client_id)),
                ));
            } else {
                pending.push((server, binding, client));
            }
        }
        assert!(ticket_server.is_none(), "mount ticket pending");
        rig.pending = pending;
        rig.join_pending();

        let ticket = rig
            .entries
            .values()
            .find_map(|entry| match entry.binding {
                Binding::MountTicket { ticket } => Some(ticket),
                _ => None,
            })
            .expect("mount ticket registered");
        assert_eq!(
            rig.topology.mount_status(ticket).unwrap(),
            TicketState::Prepared
        );
        assert!(rig.pending_backends.contains_key(&ticket));

        // MountTicket.commit {root_node, root_generation} is the only publish
        // point; the embedded placeholder backend is promoted.
        let commit = mt_wire::commit_request {
            root_node: 77,
            root_generation: 42,
        };
        let mut commit_invocation = mt_wire::submit_commit(
            &ticket_client,
            &commit,
            ResourceTransferBuilder::new(),
            &mut bufs.wire,
            1024,
        )
        .unwrap();
        let ticket_key = rig.key_where(|binding| matches!(binding, Binding::MountTicket { .. }));
        pump_mount_ticket(rig, ticket_key, ticket, bufs).unwrap();
        let commit_response =
            mt_wire::take_commit(&mut commit_invocation, &mut bufs.result_wire).unwrap();
        let mount_id = commit_response.value.mount_id;
        assert_eq!(commit_response.value.backend_generation, 42);
        assert_eq!(
            rig.topology.mount_status(ticket).unwrap(),
            TicketState::Committed
        );
        assert!(rig.backends.contains_key(&mount_id));
        assert!(rig.pending_backends.is_empty());
        let root_node = rig.topology.mount(mount_id).unwrap().root_node;
        assert_eq!(root_node.node_id, 77);
        assert_eq!(
            root_node.generation, 42,
            "MountTicket.commit must preserve the worker root generation"
        );

        // Seed a file in the published child backend for later assertions.
        let child_root = rig.backends.get(&mount_id).unwrap().root();
        rig.backends
            .get_mut(&mount_id)
            .unwrap()
            .create_file(child_root, b"/file", false, b"payload")
            .unwrap();

        (mount_id, ticket)
    }

    /// Register an internal NamespaceBinding endpoint served with `state`;
    /// returns the raw client handle for raw_invoke_submit.
    fn namespace_client(
        rig: &mut Rig,
        state: NsBindingState,
    ) -> (ManuallyDrop<naos_idl::ProtocolClientEndpoint>, sys::Handle) {
        let (client, server) = loopback_ns_pair().unwrap();
        let client_raw = client.into_raw();
        let server_raw = server.into_raw();
        rig.entries.insert(
            server_raw,
            Entry {
                // SAFETY: uniquely owned freshly minted handles leaked into
                // the test rig for the process lifetime.
                endpoint: unsafe { ProtocolServerEndpoint::from_raw(server_raw) },
                binding: Binding::NamespaceBinding(state),
            },
        );
        (
            ManuallyDrop::new(unsafe { naos_idl::ProtocolClientEndpoint::from_raw(client_raw) }),
            server_raw,
        )
    }

    fn ok_raw(invocation: &mut Invocation) {
        assert_eq!(
            loopback::raw_take_result(invocation.get())
                .unwrap()
                .protocol_error,
            0
        );
    }

    fn submit_resolve(
        ns_client: &naos_idl::ProtocolClientEndpoint,
        path: &[u8],
        budget: u32,
        bufs: &mut Buffers,
    ) -> Invocation {
        let request = namespace_wire::resolve_absolute_request {
            walk: namespace_wire::WalkContext {
                open_flags: 0,
                remaining_symlinks: budget,
                reserved: 0,
            },
            path_size: path.len() as u64,
            path,
        };
        namespace_wire::submit_resolve_absolute(
            ns_client,
            &request,
            ResourceTransferBuilder::new(),
            &mut bufs.wire,
            1024,
        )
        .unwrap()
    }

    #[test]
    fn prepare_commit_and_cross_mount_lookup_publish_the_topology() {
        let _mint_guard = mint_test_guard();
        install();
        override_mints_with_loopback();
        let mut rig = Rig::new();
        let mut bufs = Buffers::new();
        let (mount_id, _ticket) = committed_mount(&mut rig, &mut bufs);
        assert_eq!(mount_id, 1);

        // resolve_absolute from a system-root binding crosses into the child
        // mount at /mnt and materializes a File endpoint over its backend.
        let root_state = rig.system_root_state();
        let (ns_client, ns_key) = namespace_client(&mut rig, root_state);
        let mut invocation = submit_resolve(&ns_client, b"/mnt/file", 4, &mut bufs);
        pump_namespace(&mut rig, ns_key, None, &mut bufs).unwrap();
        let outcome = take_raw(&mut invocation);
        assert_eq!(outcome.protocol_error, 0);
        assert!(outcome.actual_bytes > 0);
        rig.join_pending();

        let file_mount = rig
            .entries
            .values()
            .find_map(|entry| match entry.binding {
                Binding::File { mount, .. } => Some(mount),
                _ => None,
            })
            .expect("file endpoint minted from routing");
        assert_eq!(file_mount, mount_id);

        // get_mount_info reports the committed record; sync accepts it.
        let (admin, admin_key) = admin_client(&mut rig);
        let info_request = vfs::get_mount_info_request { mount_id };
        let mut info_invocation = vfs::submit_get_mount_info(
            &admin,
            &info_request,
            ResourceTransferBuilder::new(),
            &mut bufs.wire,
            1024,
        )
        .unwrap();
        pump_admin(&mut rig, admin_key, &mut bufs);
        let info = vfs::take_get_mount_info(&mut info_invocation, &mut bufs.result_wire).unwrap();
        assert_eq!(info.value.backend_generation, 42);
        assert_ne!(info.value.device_id, 0);

        // Unknown mounts report ENOENT.
        let missing = vfs::get_mount_info_request { mount_id: 999 };
        let mut missing_invocation = vfs::submit_get_mount_info(
            &admin,
            &missing,
            ResourceTransferBuilder::new(),
            &mut bufs.wire,
            1024,
        )
        .unwrap();
        pump_admin(&mut rig, admin_key, &mut bufs);
        assert_eq!(
            loopback::raw_take_result(missing_invocation.get())
                .unwrap()
                .protocol_error,
            -(Errno::ENoent.to_i32() as i64)
        );
    }

    #[test]
    fn routed_directory_endpoint_uses_the_child_backend_root() {
        let _mint_guard = mint_test_guard();
        install();
        override_mints_with_loopback();
        let mut rig = Rig::new();
        let mut bufs = Buffers::new();
        let (mount_id, _) = committed_mount(&mut rig, &mut bufs);
        let child_root = rig.backends.get(&mount_id).unwrap().root();
        let child_dir = rig
            .backends
            .get_mut(&mount_id)
            .unwrap()
            .mkdir(child_root, b"dir")
            .unwrap()
            .node_id;

        let root_state = rig.system_root_state();
        let (ns_client, ns_key) = namespace_client(&mut rig, root_state);
        let mut invocation = submit_resolve(&ns_client, b"/mnt/dir", 4, &mut bufs);
        pump_namespace(&mut rig, ns_key, None, &mut bufs).unwrap();
        assert_eq!(take_raw(&mut invocation).protocol_error, 0);

        let routed = rig
            .pending
            .iter()
            .find_map(|(_, binding, _)| match binding {
                Binding::Directory {
                    mount,
                    root,
                    current,
                } if *mount == mount_id && *current == child_dir => Some((*root, *current)),
                _ => None,
            })
            .expect("resolved child directory endpoint");
        assert_eq!(routed.0, child_root);
    }

    #[test]
    fn unmount_drains_only_after_bindings_release() {
        let _mint_guard = mint_test_guard();
        install();
        override_mints_with_loopback();
        let mut rig = Rig::new();
        let mut bufs = Buffers::new();
        let (mount_id, _) = committed_mount(&mut rig, &mut bufs);

        // An active application Directory endpoint on the mount keeps it busy.
        let (busy_client, busy_server) = directory::create_endpoints(None).unwrap();
        core::mem::forget(busy_client);
        let busy_root = rig.backends.get(&mount_id).unwrap().root();
        rig.entries.insert(
            busy_server.get(),
            Entry {
                endpoint: busy_server,
                binding: Binding::Directory {
                    mount: mount_id,
                    root: busy_root,
                    current: busy_root,
                },
            },
        );

        let (admin, admin_key) = admin_client(&mut rig);
        let request = vfs::unmount_request {
            target_size: 4,
            target: b"/mnt",
            flags: 0,
        };
        let mut invocation =
            vfs::submit_unmount(&admin, &request, ResourceTransferBuilder::new(), &mut bufs.wire, 1024)
                .unwrap();
        pump_admin(&mut rig, admin_key, &mut bufs);
        assert_eq!(
            loopback::raw_take_result(invocation.get())
                .unwrap()
                .protocol_error,
            -(Errno::EBusy.to_i32() as i64)
        );
        // r7 (4): a failed drain rolls back to ACTIVE and stays usable.
        assert_eq!(
            rig.topology.mount(mount_id).unwrap().state,
            crate::mount::MountState::Active
        );

        // Release the binding; the same unmount now drains, syncs, detaches.
        let busy_key = rig.key_where(|binding| match binding {
            Binding::Directory { mount, .. } => *mount == mount_id,
            _ => false,
        });
        drop(rig.entries.remove(&busy_key).expect("busy endpoint"));
        let mut retry =
            vfs::submit_unmount(&admin, &request, ResourceTransferBuilder::new(), &mut bufs.wire, 1024)
                .unwrap();
        pump_admin(&mut rig, admin_key, &mut bufs);
        ok_raw(&mut retry);
        assert!(rig.topology.mount(mount_id).is_none());
        assert!(!rig.backends.contains_key(&mount_id));
        assert!(
            rig.entries
                .values()
                .all(|entry| !binding_references_mount(&entry.binding, mount_id))
        );
        assert_eq!(control_mount_ticket_for(mount_id), None);
    }

    /// Registry lookup inverted for test assertions.
    fn control_mount_ticket_for(mount: u64) -> Option<u64> {
        CONTROL_MOUNTS.with(|registry| {
            registry
                .iter()
                .find_map(|(ticket, m)| if *m == mount { Some(*ticket) } else { None })
        })
    }

    #[test]
    fn rename_across_mounts_fails_exdev_without_side_effects() {
        let _mint_guard = mint_test_guard();
        install();
        override_mints_with_loopback();
        let mut rig = Rig::new();
        let mut bufs = Buffers::new();
        let (mount_id, _) = committed_mount(&mut rig, &mut bufs);

        // A Directory endpoint inside the child mount issues rename_at with a
        // new_parent that lives on the root mount: identity comparison alone
        // must fail EXDEV before any metadata work (section 6.4).
        let child_root = rig.backends.get(&mount_id).unwrap().root();
        let (child_client, child_server) = directory::create_endpoints(None).unwrap();
        rig.entries.insert(
            child_server.get(),
            Entry {
                endpoint: child_server,
                binding: Binding::Directory {
                    mount: mount_id,
                    root: child_root,
                    current: child_root,
                },
            },
        );

        let (peer_client, peer_server) = loopback_directory_pair().unwrap();
        let root_of_root = rig.root_fs.root();
        let peer_client_raw = peer_client.into_raw();
        rig.entries.insert(
            peer_server.get(),
            Entry {
                endpoint: peer_server,
                binding: Binding::Directory {
                    mount: ROOT_MOUNT_ID,
                    root: root_of_root,
                    current: root_of_root,
                },
            },
        );
        let peer_handle = unsafe { OwnedHandle::from_raw(peer_client_raw) };
        let mut table: ResourceTransferBuilder<'static> = ResourceTransferBuilder::new();
        let slot = table.push_move(peer_handle).unwrap();

        let request = directory::rename_at_request {
            flags: 0,
            new_parent: slot,
            first_size: 2,
            second_size: 2,
            first: b"/f",
            second: b"/g",
        };
        let mut wire = vec![0u8; 512];
        let mut invocation =
            directory::submit_rename_at(&child_client, &request, table, &mut wire, 1024).unwrap();

        let child_key = rig.key_where(|binding| match binding {
            Binding::Directory { mount, .. } => *mount == mount_id,
            _ => false,
        });
        let endpoint = rig.server_ep(child_key);
        let incoming = naos_idl::receive_request(&endpoint, &mut bufs.wire).unwrap();
        let moved_peer_raw = incoming.resources.get(slot).unwrap().get();
        let moved_peer_id = naos_idl::object_id(moved_peer_raw).unwrap();
        rig.clients
            .insert(moved_peer_id, (ROOT_MOUNT_ID, root_of_root, root_of_root));
        let outcome = crate::server::dispatch_directory_for_test(
            rig.backends.get_mut(&mount_id).unwrap(),
            &mut rig.topology,
            &rig.entries,
            &rig.clients,
            (mount_id, child_root, child_root),
            incoming,
            &mut bufs.reply_wire,
        )
        .unwrap();
        assert_eq!(outcome, naos_idl::DispatchOutcome::Completed);
        assert_eq!(
            loopback::raw_take_result(invocation.get())
                .unwrap()
                .protocol_error,
            -(Errno::EXdev.to_i32() as i64)
        );
    }

    #[test]
    fn link_across_mounts_fails_exdev_without_side_effects() {
        let _mint_guard = mint_test_guard();
        install();
        override_mints_with_loopback();
        let mut rig = Rig::new();
        let mut bufs = Buffers::new();
        let (mount_id, _) = committed_mount(&mut rig, &mut bufs);

        let child_root = rig.backends.get(&mount_id).unwrap().root();
        let (child_client, child_server) = directory::create_endpoints(None).unwrap();
        rig.entries.insert(
            child_server.get(),
            Entry {
                endpoint: child_server,
                binding: Binding::Directory {
                    mount: mount_id,
                    root: child_root,
                    current: child_root,
                },
            },
        );

        let (peer_client, peer_server) = loopback_directory_pair().unwrap();
        let root_of_root = rig.root_fs.root();
        let peer_client_raw = peer_client.into_raw();
        rig.entries.insert(
            peer_server.get(),
            Entry {
                endpoint: peer_server,
                binding: Binding::Directory {
                    mount: ROOT_MOUNT_ID,
                    root: root_of_root,
                    current: root_of_root,
                },
            },
        );
        let peer_handle = unsafe { OwnedHandle::from_raw(peer_client_raw) };
        let mut table: ResourceTransferBuilder<'static> = ResourceTransferBuilder::new();
        let slot = table.push_move(peer_handle).unwrap();

        let request = directory::link_at_request {
            flags: 0,
            new_parent: slot,
            first_size: 2,
            second_size: 2,
            first: b"/f",
            second: b"/g",
        };
        let mut wire = vec![0u8; 512];
        let mut invocation =
            directory::submit_link_at(&child_client, &request, table, &mut wire, 1024).unwrap();

        let child_key = rig.key_where(|binding| match binding {
            Binding::Directory { mount, .. } => *mount == mount_id,
            _ => false,
        });
        let endpoint = rig.server_ep(child_key);
        let incoming = naos_idl::receive_request(&endpoint, &mut bufs.wire).unwrap();
        let moved_peer_raw = incoming.resources.get(slot).unwrap().get();
        let moved_peer_id = naos_idl::object_id(moved_peer_raw).unwrap();
        rig.clients
            .insert(moved_peer_id, (ROOT_MOUNT_ID, root_of_root, root_of_root));
        let outcome = crate::server::dispatch_directory_for_test(
            rig.backends.get_mut(&mount_id).unwrap(),
            &mut rig.topology,
            &rig.entries,
            &rig.clients,
            (mount_id, child_root, child_root),
            incoming,
            &mut bufs.reply_wire,
        )
        .unwrap();
        assert_eq!(outcome, naos_idl::DispatchOutcome::Completed);
        assert_eq!(
            loopback::raw_take_result(invocation.get())
                .unwrap()
                .protocol_error,
            -(Errno::EXdev.to_i32() as i64)
        );
    }

    #[test]
    fn symlink_loop_across_mounts_exhausts_the_global_budget() {
        let _mint_guard = mint_test_guard();
        install();
        override_mints_with_loopback();
        let mut rig = Rig::new();
        let mut bufs = Buffers::new();
        let (mount_id, _) = committed_mount(&mut rig, &mut bufs);

        // Loop spanning two mounts: /loop-a -> /mnt/loop-b in the root
        // backend, /loop-b -> /loop-a in the child backend. The absolute
        // target restarts at the visible root (section 6.3), so the chain
        // crosses mounts and must terminate through the global budget.
        let rr = rig.root_fs.root();
        rig.root_fs
            .symlink_scoped(rr, rr, b"/mnt/loop-b", b"/loop-a")
            .unwrap();
        let cr = rig.backends.get(&mount_id).unwrap().root();
        rig.backends
            .get_mut(&mount_id)
            .unwrap()
            .symlink_scoped(cr, cr, b"/loop-a", b"/loop-b")
            .unwrap();

        let root_state = rig.system_root_state();
        let (ns_client, ns_key) = namespace_client(&mut rig, root_state);
        let looped = submit_resolve(&ns_client, b"/loop-a", 5, &mut bufs);
        let root_state = rig.system_root_state();
        pump_namespace(&mut rig, ns_key, Some(root_state), &mut bufs).unwrap();
        assert_eq!(
            loopback::raw_take_result(looped.get())
                .unwrap()
                .protocol_error,
            -(Errno::ELoop.to_i32() as i64)
        );

        // Positive control: a real file below the mountpoint resolves with
        // budget to spare.
        let mut resolved = submit_resolve(&ns_client, b"/mnt/file", 5, &mut bufs);
        let root_state = rig.system_root_state();
        pump_namespace(&mut rig, ns_key, Some(root_state), &mut bufs).unwrap();
        ok_raw(&mut resolved);

        // A tight budget fails before any expansion completes.
        let mut starved = submit_resolve(&ns_client, b"/mnt/file", 0, &mut bufs);
        let root_state = rig.system_root_state();
        pump_namespace(&mut rig, ns_key, Some(root_state), &mut bufs).unwrap();
        ok_raw(&mut starved); // no symlink involved: zero budget still works
    }
    #[test]
    fn mutation_tickets_admit_blocks_expiry_and_status_reconciles() {
        let _mint_guard = mint_test_guard();
        install();
        override_mints_with_loopback();
        let mut rig = Rig::new();
        let mut bufs = Buffers::new();

        let rr = rig.root_fs.root();
        let dir_meta = rig.root_fs.mkdir(rr, b"/d").unwrap();
        let file_meta = rig
            .root_fs
            .create_file(dir_meta.node_id, b"/a", false, b"x")
            .unwrap();
        let generation = rig
            .topology
            .mount(ROOT_MOUNT_ID)
            .unwrap()
            .backend_generation;
        let dir_key = NodeKey {
            node_id: dir_meta.node_id,
            generation,
        };
        let file_key = NodeKey {
            node_id: file_meta.node_id,
            generation,
        };

        // begin_mutation over the NamespaceBinding endpoint issues the lease.
        let root_state = rig.system_root_state();
        let (ns_client, ns_key) = namespace_client(&mut rig, root_state);
        let begin = namespace_wire::begin_mutation_request {
            operation: internal::namespace_binding::OP_RENAME,
            old_parent: namespace_node_key(dir_key),
            old_target: namespace_node_key(file_key),
            new_parent: namespace_node_key(dir_key),
            new_target: namespace_node_key(NodeKey::ZERO),
            old_name_size: 1,
            old_name: b"a",
            new_name_size: 1,
            new_name: b"b",
        };
        let mut invocation = namespace_wire::submit_begin_mutation(
            &ns_client,
            &begin,
            ResourceTransferBuilder::new(),
            &mut bufs.wire,
            1024,
        )
        .unwrap();
        pump_namespace(&mut rig, ns_key, None, &mut bufs).unwrap();
        ok_raw(&mut invocation);
        rig.join_pending();

        let ticket = rig
            .entries
            .values()
            .find_map(|entry| match entry.binding {
                Binding::MutationTicket { ticket } => Some(ticket),
                _ => None,
            })
            .expect("mutation ticket registered");

        // Throwaway ticket endpoint pair for driving status/commit/abort.
        let (mt_client, mt_server) = loopback_mutation_pair().unwrap();
        let mt_client = ManuallyDrop::new(mt_client);
        let mt_key = mt_server.get();
        rig.entries.insert(
            mt_key,
            Entry {
                endpoint: mt_server,
                binding: Binding::MutationTicket { ticket },
            },
        );

        let raw_status = |rig: &mut Rig, bufs: &mut Buffers| -> Result<u32, i64> {
            let request = mutation_wire::status_request {};
            let mut request_wire = [0u8; 16];
            let mut invocation = mutation_wire::submit_status(
                &mt_client,
                &request,
                ResourceTransferBuilder::new(),
                &mut request_wire,
                1024,
            )
            .unwrap();
            pump_mutation(rig, mt_key, ticket, bufs).unwrap();
            take_mutation_status(&mut invocation, bufs)
        };

        // PREPARED while the worker works under the reservation.
        assert_eq!(
            raw_status(&mut rig, &mut bufs),
            Ok(crate::mount::ticket_state::PREPARED)
        );

        // Admitting the commit enters COMMITTING and freezes expiry: even a
        // clock far past TICKET_TIMEOUT_TICKS cannot reach EXPIRED now (r7).
        rig.topology.admit_mutation(ticket).unwrap();
        assert_eq!(rig.topology.poll_expiry(u64::MAX / 2), 0);
        assert_eq!(
            raw_status(&mut rig, &mut bufs),
            Ok(crate::mount::ticket_state::COMMITTING)
        );

        // Finishing publishes COMMITTED and releases the reservation.
        rig.topology.finish_mutation(ticket, true).unwrap();
        assert_eq!(
            raw_status(&mut rig, &mut bufs),
            Ok(crate::mount::ticket_state::COMMITTED)
        );

        // A second commit on the terminal ticket fails EINVAL.
        let request = mutation_wire::commit_request {};
        let mut request_wire = [0u8; 16];
        let mut second_commit = mutation_wire::submit_commit(
            &mt_client,
            &request,
            ResourceTransferBuilder::new(),
            &mut request_wire,
            1024,
        )
        .unwrap();
        pump_mutation(&mut rig, mt_key, ticket, &mut bufs).unwrap();
        assert_eq!(
            loopback::raw_take_result(second_commit.get())
                .unwrap()
                .protocol_error,
            -(Errno::EInval.to_i32() as i64)
        );

        // Expiry still fires for tickets left PREPARED past the deadline:
        // a fresh reservation times out and reports EXPIRED idempotently.
        let stale = rig
            .topology
            .begin_mutation(
                ROOT_MOUNT_ID,
                internal::namespace_binding::OP_RENAME,
                dir_key,
                file_key,
                dir_key,
                NodeKey::ZERO,
                b"a",
                b"c",
                0,
            )
            .unwrap();
        assert_eq!(
            rig.topology.poll_expiry(crate::mount::TICKET_TIMEOUT_TICKS),
            1
        );
        let request = mutation_wire::status_request {};
        let mut request_wire = [0u8; 16];
        let mut stale_status = mutation_wire::submit_status(
            &mt_client,
            &request,
            ResourceTransferBuilder::new(),
            &mut request_wire,
            1024,
        )
        .unwrap();
        pump_mutation(&mut rig, mt_key, stale, &mut bufs).unwrap();
        assert_eq!(
            take_mutation_status(&mut stale_status, &mut bufs),
            Ok(crate::mount::ticket_state::EXPIRED)
        );
    }
}
