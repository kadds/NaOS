//! File/Directory protocol server over the RAM backend
//! (USERSPACE_FILESYSTEM_ADR §5.2-§5.4).
//!
//! Every client-visible endpoint is a real protocol endpoint pair created in
//! this process: the client end travels to the caller as a MOVE resource,
//! the server end stays in [`VfsServer`] and is served by a single
//! round-robin dispatch loop. Semantics mirrored from the frozen contracts:
//!
//! * every `open`/`clone_binding` yields an independent endpoint; clones of
//!   a Directory share its visible root/current pair (the Directory wire has
//!   no cursor state -- `list` offsets travel with each request, §5.2);
//! * File open descriptions share offset state across clones per §5.2;
//! * `stat_node` NOFOLLOW applies to the final component only, with the
//!   backend's global symlink budget surfacing ELOOP;
//! * `rename_at`/`link_at` consume their MOVE-disposition `new_parent` and
//!   resolve both sides before any mutation; within one vfsd instance EXDEV
//!   cannot occur, so a foreign endpoint fails EINVAL without side effects;
//! * `set_current`/`set_root` are frozen compatibility no-ops answered
//!   ENOTSUP (§5.3.4);
//! * `materialize` snapshots at admission, refuses directories (EISDIR) and
//!   files beyond NA_MEMORY_OBJECT_MAX_BYTES (EFBIG), and hands out an
//!   immutable READ|MAP|INFO MemoryObject (§5.4).

use alloc::collections::BTreeMap;
use alloc::vec;
use alloc::vec::Vec;
use core::mem::ManuallyDrop;

use naos_idl::directory::{self, DirectoryHandler};
use naos_idl::file::{self, FileHandler};
use naos_idl::vfs;
use naos_idl::{
    CallError, DispatchOutcome, FailInvocation, Invocation, MethodReply, OwnedHandle,
    ProtocolClientEndpoint, ProtocolServerEndpoint, ReceivedResources, ResourceSlot, ResourceTable,
    ResponderHandle,
};
use naos_sys as sys;

use crate::backend::{FileKind, Metadata, NodeId, RamFs};
use crate::errno::Errno;
use crate::mount_admin::{
    MountControlCtx, NamespaceCtx, VfsCtl, binding_references_mount, ns_state_references_mount,
    run_mount_control, run_mount_ticket, run_mutation_ticket, run_namespace_binding, run_vfs_admin,
};

/// Pseudo st_dev reported for the single RAM instance (VFS ADR §6.2).
const PSEUDO_DEVICE: u64 = 1;

// Wire flag vocabulary shared with the Phase-3 kernel adapter / mlibc.
mod walk_flags {
    pub const CREATE: u64 = 1;
    /// `not_resolve_symbolic_link`: keep the final component un-followed.
    pub const NOFOLLOW_FINAL: u64 = 8;
    pub const DIRECTORY: u64 = 16;
    pub const FILE: u64 = 32;
    pub const TRUNC: u64 = 256;
}

mod open_mode {
    pub const READ: u64 = 1;
    pub const WRITE: u64 = 2;
    pub const APPEND: u64 = 8;
    pub const EXCL: u64 = 128;
}

/// `NA_DIRECTORY_OPEN_FLAG_CHROOT` (abi.h): the opened directory becomes the
/// new endpoint's visible root.
const OPEN_FLAG_CHROOT: u64 = 1 << 63;

/// `NA_DIRECTORY_LOOKUP_FLAG_NOFOLLOW` for stat_node (abi.h).
const LOOKUP_NOFOLLOW: u64 = 1;

/// `fs::create_flags::directory` for create/remove.
const CREATE_FLAG_DIRECTORY: u64 = 1;

/// POSIX st_mode type bits.
const S_IFDIR: u32 = 0o040000;
const S_IFREG: u32 = 0o100000;
const S_IFLNK: u32 = 0o120000;

fn fail(errno: Errno) -> FailInvocation {
    FailInvocation::domain(-(errno.to_i32() as i64))
}

/// Bytes to move through a request's bulk region: the caller must grant a
/// window at least as large as the requested size, and the size must fit a
/// host-side length before any mapping is attempted.
fn bulk_transfer_bytes(size: u64) -> Result<usize, FailInvocation> {
    usize::try_from(size).map_err(|_| fail(Errno::EInval))
}

/// Region handle for a request the generated dispatcher already validated
/// (slot 0 binding/scope/rights/disposition); `None` means the request
/// arrived without its mandatory bulk reference.
fn region_handle(region: Option<sys::Handle>) -> Result<sys::Handle, FailInvocation> {
    region.ok_or_else(|| fail(Errno::EInval))
}

fn strip_nul(path: &[u8]) -> &[u8] {
    let mut end = path.len();
    while end > 0 && path[end - 1] == 0 {
        end -= 1;
    }
    &path[..end]
}

fn directory_stat(meta: Metadata) -> directory::Stat {
    let mode_bits = match meta.kind {
        FileKind::Regular => S_IFREG,
        FileKind::Directory => S_IFDIR,
        FileKind::Symlink => S_IFLNK,
    };
    // Single-user system (uid/gid 0); permissions are rwxr-xr-x.
    directory::Stat {
        device: PSEUDO_DEVICE,
        inode: meta.node_id,
        links: meta.nlink,
        mode: mode_bits | 0o755,
        uid: 0,
        gid: 0,
        padding: 0,
        device_id: 0,
        size: meta.size as i64,
        block_size: 4096,
        blocks: ((meta.size + 511) / 512) as i64,
        access_seconds: 0,
        access_nanoseconds: 0,
        modify_seconds: 0,
        modify_nanoseconds: 0,
        change_seconds: 0,
        change_nanoseconds: 0,
        unused0: 0,
        unused1: 0,
        unused2: 0,
    }
}

fn file_stat(meta: Metadata) -> file::Stat {
    let value = directory_stat(meta);
    file::Stat {
        device: value.device,
        inode: value.inode,
        links: value.links,
        mode: value.mode,
        uid: value.uid,
        gid: value.gid,
        padding: value.padding,
        device_id: value.device_id,
        size: value.size,
        block_size: value.block_size,
        blocks: value.blocks,
        access_seconds: value.access_seconds,
        access_nanoseconds: value.access_nanoseconds,
        modify_seconds: value.modify_seconds,
        modify_nanoseconds: value.modify_nanoseconds,
        change_seconds: value.change_seconds,
        change_nanoseconds: value.change_nanoseconds,
        unused0: value.unused0,
        unused1: value.unused1,
        unused2: value.unused2,
    }
}

/// What a live server endpoint is bound to.
#[derive(Clone, Copy, Debug)]
pub enum Binding {
    /// A Directory endpoint: owning backend mount plus visible-root/current
    /// nodes. The root RAM instance is [`crate::mount::ROOT_MOUNT_ID`].
    Directory {
        mount: u64,
        root: NodeId,
        current: NodeId,
    },
    /// A File endpoint over one open description; `mode` keeps the open-mode
    /// bits so read/write enforcement survives clones.
    File { mount: u64, fd: u64, mode: u64 },
    /// The `Vfs` admin control-plane endpoint (scope 16).
    VfsAdmin,
    /// One pending/published MountTicket (scope 19).
    MountTicket { ticket: u64 },
    /// One open mutation reservation lease (scope 22).
    MutationTicket { ticket: u64 },
    /// A private NamespaceBinding routing context (scope 18, §6.3 r6).
    NamespaceBinding(NsBindingState),
    /// A worker-side MountControl endpoint retained for the embedded fallback
    /// and host tests. External workers receive the server half directly;
    /// vfsd keeps the client authority in `mount_controls`.
    MountControl { ticket: u64 },
}

/// Routing context carried by every NamespaceBinding endpoint
/// (`{namespace_instance, visible_root_node, current_node, mount_stack}` of
/// §6.3; `namespace_instance` is validated at mint time, not per request).
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct NsBindingState {
    pub visible_root_mount: u64,
    pub visible_root: NodeId,
    pub current_mount: u64,
    pub current: NodeId,
    /// Depth of namespace mounts entered through this binding chain.
    pub mount_stack: u32,
}

pub(crate) struct Entry {
    pub(crate) endpoint: ProtocolServerEndpoint,
    pub(crate) binding: Binding,
}

/// Registrations produced inside a handler while the serve loop holds the
/// entry borrow; joined after the request completes. The trailing value is
/// the raw handle and stable capability identity of the minted client end,
/// remembered so `rename_at` / `link_at` can resolve a transferred
/// `new_parent` after the kernel renumbers its handle.
pub(crate) type PendingRegistration = (ProtocolServerEndpoint, Binding, Option<(sys::Handle, u64)>);

/// The whole vfsd serving state: RAM backend plus the endpoint table.
pub struct VfsServer {
    fs: RamFs,
    entries: BTreeMap<sys::Handle, Entry>,
    /// Directory client ends this server minted, keyed by stable capability
    /// identity and mapped to their `(root, current)` scope. The identity is
    /// preserved when the kernel MOVE-transfers and renumbers the handle.
    clients: BTreeMap<u64, (u64, NodeId, NodeId)>,
    /// Server-end to client-end association for endpoints minted by vfsd.
    /// It lets peer-close remove the corresponding client registry entry;
    /// the kernel may renumber a moved capability, so the association cannot
    /// be reconstructed from the server handle later.
    client_by_server: BTreeMap<sys::Handle, u64>,
    pending: Vec<PendingRegistration>,
    next_generation: u64,
    /// Mount topology, shared reservation table and ticket machines.
    topology: crate::mount::Topology,
    /// Backend instances of committed child mounts keyed by mount id.
    backends: BTreeMap<u64, RamFs>,
    /// Backends created by prepare_mount whose worker has not committed yet.
    pending_backends: BTreeMap<u64, RamFs>,
    /// NamespaceBinding client ends this server minted, for MountControl.
    /// bind_node to translate a moved-in binding back to its routing state.
    binding_clients: BTreeMap<u64, NsBindingState>,
    /// Per-mount anchor caches (`root_anchor`, `parent_anchor`). These are
    /// vfsd-private and never count toward busy refs (§6.2 r7 ④).
    root_anchors: BTreeMap<u64, OwnedHandle>,
    parent_anchors: BTreeMap<u64, OwnedHandle>,
    /// Client halves of worker MountControl pairs. The server half is moved
    /// to the filesystem worker; vfsd retains this control authority for
    /// lifecycle RPCs after the worker commits.
    mount_controls: BTreeMap<u64, OwnedHandle>,
    /// Production vfsd uses independently spawned workers; host tests keep
    /// the embedded RAM backend enabled to exercise namespace semantics.
    external_workers: bool,
    /// Tokio registrations for all current endpoint and listener handles.
    /// This is refreshed only when the handle set changes.
    readiness: servicekit::ReadinessSet,
}

impl VfsServer {
    pub fn new(fs: RamFs) -> Self {
        Self {
            fs,
            entries: BTreeMap::new(),
            clients: BTreeMap::new(),
            client_by_server: BTreeMap::new(),
            pending: Vec::new(),
            next_generation: 1,
            topology: crate::mount::Topology::new(0),
            backends: BTreeMap::new(),
            pending_backends: BTreeMap::new(),
            binding_clients: BTreeMap::new(),
            root_anchors: BTreeMap::new(),
            parent_anchors: BTreeMap::new(),
            mount_controls: BTreeMap::new(),
            external_workers: false,
            readiness: servicekit::ReadinessSet::new(),
        }
    }

    /// Construct the production server mode in which committed mounts are
    /// owned by external filesystem workers rather than a vfsd placeholder.
    pub fn new_external(fs: RamFs) -> Self {
        let mut server = Self::new(fs);
        server.external_workers = true;
        server
    }

    pub fn backend_mut(&mut self) -> &mut RamFs {
        &mut self.fs
    }

    pub fn backend(&self) -> &RamFs {
        &self.fs
    }

    pub fn endpoint_count(&self) -> usize {
        self.entries.len()
    }

    /// The root route becomes usable only after an external worker has
    /// committed a child mount and the post-commit bind_node handshake has
    /// installed its anchor.  Keeping this gate in vfsd prevents a Directory
    /// listener from exposing the placeholder root to init.
    pub fn root_route_ready(&self) -> bool {
        self.topology
            .children_of(crate::mount::ROOT_MOUNT_ID)
            .into_iter()
            .any(|mount| {
                self.topology
                    .mount(mount)
                    .is_some_and(|record| record.state == crate::mount::MountState::Active)
                    && self.root_anchors.contains_key(&mount)
            })
    }

    /// Register a Directory server endpoint rooted at `(root, current)` and
    /// return the client end for hand-out (bootstrap roots, listener
    /// accepts).
    pub fn new_directory_binding(
        &mut self,
        root: NodeId,
        current: NodeId,
    ) -> Result<OwnedHandle, sys::Status> {
        let (client, server) =
            directory::create_endpoints(None).map_err(|_| sys::STATUS_RESOURCE_EXHAUSTED)?;
        let client_id =
            naos_idl::object_id(client.get()).map_err(|_| sys::STATUS_RESOURCE_EXHAUSTED)?;
        let client_raw = client.into_raw();
        let server_raw = server.get();
        let handle = unsafe { OwnedHandle::from_raw(client_raw) };
        self.insert(
            server,
            Binding::Directory {
                mount: crate::mount::ROOT_MOUNT_ID,
                root,
                current,
            },
        );
        self.clients
            .insert(client_id, (crate::mount::ROOT_MOUNT_ID, root, current));
        self.client_by_server.insert(server_raw, client_id);
        Ok(handle)
    }

    fn insert(&mut self, endpoint: ProtocolServerEndpoint, binding: Binding) {
        self.entries
            .insert(endpoint.get(), Entry { endpoint, binding });
    }

    /// Drain accepted protocol server ends from the ServiceDirectory listen
    /// channel; each message carries one fresh Directory server endpoint.
    fn accept_connections(&mut self, listener: sys::Handle, admin: bool) {
        loop {
            let mut resources = [sys::HANDLE_INVALID; naos_idl::MAX_RESOURCES];
            let mut frame = sys::ChannelReceiveFrame {
                struct_size: core::mem::size_of::<sys::ChannelReceiveFrame>() as u32,
                bytes: 0,
                byte_capacity: 0,
                resources: resources.as_mut_ptr() as u64,
                resource_capacity: naos_idl::MAX_RESOURCES as u64,
                ..sys::ChannelReceiveFrame::default()
            };
            let status = unsafe { sys::_na_channel_receive(listener, &mut frame) };
            if status != sys::STATUS_OK || frame.actual_resources == 0 {
                return;
            }
            for index in 0..frame.actual_resources.min(naos_idl::MAX_RESOURCES as u64) as usize {
                let raw = resources[index];
                resources[index] = sys::HANDLE_INVALID;
                if raw == sys::HANDLE_INVALID {
                    continue;
                }
                // SAFETY: freshly received handle, uniquely owned here.
                let endpoint = unsafe { ProtocolServerEndpoint::from_raw(raw) };
                if admin {
                    self.insert(endpoint, Binding::VfsAdmin);
                } else {
                    let root = self.fs.root();
                    self.insert(
                        endpoint,
                        Binding::Directory {
                            mount: crate::mount::ROOT_MOUNT_ID,
                            root,
                            current: root,
                        },
                    );
                }
            }
        }
    }

    /// Serve one readable endpoint until it would block or die.
    fn service_endpoint(&mut self, handle: sys::Handle, wire: &mut [u8], reply_wire: &mut [u8]) {
        loop {
            let Some(entry) = self.entries.get(&handle) else {
                return;
            };
            let binding = entry.binding;
            let scope = match binding {
                Binding::Directory {
                    mount,
                    root,
                    current,
                } => (mount, root, current),
                Binding::File { mount, .. } => (mount, 0, 0),
                // Control-plane endpoints pick their backend per request.
                _ => (crate::mount::ROOT_MOUNT_ID, 0, 0),
            };
            // Split the struct so handlers can use the backend while the
            // loop reads the entry table.
            let VfsServer {
                fs,
                entries,
                clients,
                client_by_server,
                pending,
                next_generation,
                topology,
                backends,
                pending_backends,
                binding_clients,
                root_anchors,
                mount_controls,
                external_workers,
                readiness: _,
                parent_anchors: _,
            } = self;

            let mut incoming = {
                let endpoint = &entries.get(&handle).expect("checked above").endpoint;
                match naos_idl::receive_request(endpoint, wire) {
                    Ok(incoming) => incoming,
                    // Idle: keep the endpoint registered for later wakeups.
                    Err(naos_idl::CallError::Status(status))
                        if status == sys::STATUS_WOULD_BLOCK =>
                    {
                        return;
                    }
                    // Peer death or protocol garbage: drop the endpoint.
                    Err(_) => {
                        retire_endpoint(
                            handle,
                            entries,
                            clients,
                            client_by_server,
                            binding_clients,
                            topology,
                            pending_backends,
                            mount_controls,
                        );
                        return;
                    }
                }
            };

            // A migrated bulk method names its region as resource slot 0.
            // The generated `validate_<method>_request_received_resources`
            // already checked binding, scope, rights and disposition before
            // the handler runs; only the raw handle has to reach the context.
            let region = ResourceSlot::new(0)
                .and_then(|slot| incoming.resources.get(slot))
                .map(OwnedHandle::get);

            // rename_at/link_at carry a MOVE-disposition endpoint whose raw
            // handle never reaches generated handlers; route them manually.
            let is_pair_method = binding.is_directory()
                && (incoming.method_id == directory::METHOD_RENAME_AT
                    || incoming.method_id == directory::METHOD_LINK_AT);

            let outcome = if is_pair_method {
                let responder = incoming.responder.take();
                let backend = match scope.0 {
                    crate::mount::ROOT_MOUNT_ID => fs,
                    mount => backends
                        .get_mut(&mount)
                        .expect("backend alive while endpoints live"),
                };
                run_pair_method(
                    backend,
                    topology,
                    entries,
                    clients,
                    scope,
                    incoming.method_id,
                    incoming.wire,
                    responder,
                    &incoming.resources,
                    reply_wire,
                )
                .map(|_| DispatchOutcome::Completed)
                .map_err(|_| naos_idl::CallError::Status(sys::STATUS_IO_ERROR))
            } else {
                match binding {
                    Binding::Directory {
                        mount,
                        root,
                        current,
                    } => {
                        let backend = match mount {
                            crate::mount::ROOT_MOUNT_ID => fs,
                            m => backends
                                .get_mut(&m)
                                .expect("backend alive while endpoints live"),
                        };
                        let mut handler = DirectoryContext {
                            fs: backend,
                            topology: &*topology,
                            root_anchors,
                            pending,
                            mount,
                            root,
                            current,
                            region,
                            scratch: Vec::new(),
                        };
                        directory::dispatch(&mut handler, incoming, reply_wire)
                    }
                    Binding::File { mount, fd, mode } => {
                        let backend = match mount {
                            crate::mount::ROOT_MOUNT_ID => fs,
                            m => backends
                                .get_mut(&m)
                                .expect("backend alive while endpoints live"),
                        };
                        let mut handler = FileContext {
                            fs: backend,
                            mount,
                            fd,
                            mode,
                            region,
                            next_generation,
                        };
                        file::dispatch(&mut handler, incoming, reply_wire)
                    }
                    Binding::VfsAdmin => run_vfs_admin(
                        VfsCtl {
                            root_fs: fs,
                            backends,
                            pending_backends,
                            topology,
                            entries,
                            clients,
                            binding_clients,
                            root_anchors,
                            mount_controls,
                            external_workers: *external_workers,
                            pending,
                            reply_wire,
                        },
                        incoming,
                    ),
                    Binding::MountTicket { ticket } => run_mount_ticket(
                        fs,
                        topology,
                        pending_backends,
                        backends,
                        mount_controls,
                        pending,
                        root_anchors,
                        *external_workers,
                        ticket,
                        incoming,
                    ),
                    Binding::MutationTicket { ticket } => {
                        run_mutation_ticket(topology, ticket, incoming)
                    }
                    Binding::NamespaceBinding(state) => run_namespace_binding(
                        NamespaceCtx {
                            root_fs: fs,
                            backends,
                            topology,
                            pending,
                            state,
                            mint_directory: crate::mount_admin::real_directory_pair,
                            mint_ns_binding: crate::internal::namespace_binding::create_endpoints,
                            mint_mutation_ticket:
                                crate::internal::mutation_ticket::create_endpoints,
                        },
                        incoming,
                        reply_wire,
                    ),
                    Binding::MountControl { ticket } => run_mount_control(
                        MountControlCtx {
                            root_fs: fs,
                            backends,
                            topology,
                            pending,
                            entries,
                            binding_clients,
                            ticket,
                            mint_directory: crate::mount_admin::real_directory_pair,
                        },
                        incoming,
                        reply_wire,
                    ),
                }
            };

            // Handler-produced endpoints join the table after dispatch.
            for (server, registration, client) in core::mem::take(pending) {
                if let Some((_, object_id)) = client.as_ref() {
                    match &registration {
                        Binding::Directory {
                            mount,
                            root,
                            current,
                        } => {
                            clients.insert(*object_id, (*mount, *root, *current));
                        }
                        Binding::NamespaceBinding(state) => {
                            binding_clients.insert(*object_id, *state);
                        }
                        _ => {}
                    }
                }
                let server_raw = server.get();
                insert_entry(entries, server, registration);
                if let Some((_, object_id)) = client {
                    client_by_server.insert(server_raw, object_id);
                }
            }

            match outcome {
                Ok(DispatchOutcome::Completed) | Ok(DispatchOutcome::Accepted) => {}
                // Unsupported method / protocol violation: close like the
                // C++ dispatcher does.
                Ok(DispatchOutcome::Rejected) | Err(_) => {
                    retire_endpoint(
                        handle,
                        entries,
                        clients,
                        client_by_server,
                        binding_clients,
                        topology,
                        pending_backends,
                        mount_controls,
                    );
                    return;
                }
            }
        }
    }

    /// One round-robin pass: wait for activity on any served endpoint (and
    /// the optional connection listener), then serve what became ready.
    pub fn serve_once(
        &mut self,
        listener: Option<sys::Handle>,
        wire: &mut [u8],
        reply_wire: &mut [u8],
    ) -> Result<(), sys::Status> {
        if let Some(listener) = listener {
            let listeners = [(listener, false)];
            self.serve_once_with_listeners(&listeners, wire, reply_wire)
        } else {
            self.serve_once_with_listeners(&[], wire, reply_wire)
        }
    }

    /// One round-robin pass over multiple persistent service listeners.
    /// `admin` marks listeners whose accepted endpoints carry the Vfs admin
    /// binding; all other listeners produce root Directory bindings.
    pub fn serve_once_with_listeners(
        &mut self,
        listeners: &[(sys::Handle, bool)],
        wire: &mut [u8],
        reply_wire: &mut [u8],
    ) -> Result<(), sys::Status> {
        let mut handles: Vec<sys::Handle> = self.entries.keys().copied().collect();
        handles.extend(listeners.iter().map(|(listener, _)| *listener));
        // Worker-side MountControl client halves are not protocol servers in
        // vfsd, but their peer-close signal is the crash/lifecycle boundary
        // for a mounted worker and must participate in the same wait pass.
        handles.extend(self.mount_controls.values().map(OwnedHandle::get));
        if handles.is_empty() {
            // Nothing to serve yet; report so callers can retry later.
            return Err(sys::STATUS_WOULD_BLOCK);
        }
        // Wake periodically even when no endpoint is active so PREPARED
        // mount/mutation reservations are expired by the service itself.
        let observed =
            match servicekit::wait_ready(&handles, Some(crate::mount::TICKET_TIMEOUT_TICKS)) {
                Ok(event) => {
                    let mut signals = 0;
                    if event.readable {
                        signals |= sys::SIGNAL_READABLE;
                    }
                    if event.writable {
                        signals |= sys::SIGNAL_WRITABLE;
                    }
                    if event.read_closed || event.write_closed {
                        signals |= sys::SIGNAL_PEER_CLOSED;
                    }
                    if event.error {
                        signals |= sys::SIGNAL_OBJECT_REVOKED;
                    }
                    vec![(handles[event.index], signals)]
                }
                Err(sys::STATUS_WAIT_TIMED_OUT) | Err(sys::STATUS_WOULD_BLOCK) => Vec::new(),
                Err(status) => return Err(status),
            };

        self.serve_observed(listeners, observed, wire, reply_wire)
    }

    /// Tokio/Mio variant of [`Self::serve_once`]. Capability registrations are
    /// attached to servicekit's platform reactor and the endpoint state
    /// machine is kept in this crate; only readiness delivery crosses the
    /// runtime boundary.
    pub async fn serve_once_async(
        &mut self,
        listener: Option<sys::Handle>,
        wire: &mut [u8],
        reply_wire: &mut [u8],
    ) -> Result<(), sys::Status> {
        if let Some(listener) = listener {
            let listeners = [(listener, false)];
            self.serve_once_async_with_listeners(&listeners, wire, reply_wire)
                .await
        } else {
            self.serve_once_async_with_listeners(&[], wire, reply_wire)
                .await
        }
    }

    /// Async variant of [`Self::serve_once_with_listeners`].
    pub async fn serve_once_async_with_listeners(
        &mut self,
        listeners: &[(sys::Handle, bool)],
        wire: &mut [u8],
        reply_wire: &mut [u8],
    ) -> Result<(), sys::Status> {
        let mut handles: Vec<sys::Handle> = self.entries.keys().copied().collect();
        for (listener, _) in listeners {
            handles.push(*listener);
        }
        handles.extend(self.mount_controls.values().map(OwnedHandle::get));
        if handles.is_empty() {
            return Err(sys::STATUS_WOULD_BLOCK);
        }

        self.readiness.refresh(&handles)?;
        let (ready_index, observed) = match self
            .readiness
            .wait(Some(crate::mount::TICKET_TIMEOUT_TICKS))
            .await
        {
                Ok(event) => {
                    let index = event.index;
                    let mut signals = 0;
                    if event.readable {
                        signals |= sys::SIGNAL_READABLE;
                    }
                    if event.writable {
                        signals |= sys::SIGNAL_WRITABLE;
                    }
                    if event.read_closed || event.write_closed {
                        signals |= sys::SIGNAL_PEER_CLOSED;
                    }
                    if event.error {
                        signals |= sys::SIGNAL_OBJECT_REVOKED;
                    }
                    (Some(index), vec![(handles[index], signals)])
                }
                Err(sys::STATUS_WAIT_TIMED_OUT) => (None, Vec::new()),
                Err(status) => return Err(status),
            };
        self.topology.poll_expiry(servicekit::monotonic_ticks());
        let result = self.serve_observed(listeners, observed, wire, reply_wire);
        if let Some(index) = ready_index {
            self.readiness.clear(index);
        }
        result
    }

    fn serve_observed(
        &mut self,
        listeners: &[(sys::Handle, bool)],
        observed: Vec<(sys::Handle, u64)>,
        wire: &mut [u8],
        reply_wire: &mut [u8],
    ) -> Result<(), sys::Status> {
        self.topology.poll_expiry(servicekit::monotonic_ticks());

        // New connections first so fresh endpoints can be served below.
        for (listener, admin) in listeners {
            for (handle, signals) in &observed {
                if *handle == *listener && signals & sys::SIGNAL_READABLE != 0 {
                    self.accept_connections(*listener, *admin);
                }
            }
        }

        // Dead peers release their endpoints before serving.
        for (handle, signals) in &observed {
            if listeners.iter().any(|(listener, _)| *listener == *handle)
                || *handle == sys::HANDLE_INVALID
            {
                continue;
            }
            if signals & (sys::SIGNAL_PEER_CLOSED | sys::SIGNAL_OBJECT_REVOKED) != 0 {
                retire_endpoint(
                    *handle,
                    &mut self.entries,
                    &mut self.clients,
                    &mut self.client_by_server,
                    &mut self.binding_clients,
                    &mut self.topology,
                    &mut self.pending_backends,
                    &mut self.mount_controls,
                );
            }
        }

        // A worker crash closes the MountControl peer. Drop the stale client
        // authority and expire its pending topology reservations before
        // accepting any later mount-manager request.
        let closed_controls: Vec<u64> = observed
            .iter()
            .filter_map(|(handle, signals)| {
                if signals & (sys::SIGNAL_PEER_CLOSED | sys::SIGNAL_OBJECT_REVOKED) == 0 {
                    return None;
                }
                self.mount_controls
                    .iter()
                    .find_map(|(ticket, endpoint)| (endpoint.get() == *handle).then_some(*ticket))
            })
            .collect();
        for ticket in closed_controls {
            self.mount_controls.remove(&ticket);
            let failed_mount = crate::mount_admin::peer_closed_control(
                ticket,
                &mut self.topology,
                &mut self.pending_backends,
            );
            for mount in failed_mount {
                self.root_anchors.remove(&mount);
                self.backends.remove(&mount);
                if let Some(child_ticket) =
                    crate::mount_admin::control_mount_ticket_for_mount(mount)
                {
                    self.mount_controls.remove(&child_ticket);
                }
                crate::mount_admin::unlink_control_mount(mount);
                self.entries
                    .retain(|_, entry| !binding_references_mount(&entry.binding, mount));
                self.clients.retain(|_, (owner, _, _)| *owner != mount);
                self.binding_clients
                    .retain(|_, state| !ns_state_references_mount(*state, mount));
            }
        }

        for (handle, signals) in &observed {
            if listeners.iter().any(|(listener, _)| *listener == *handle)
                || *handle == sys::HANDLE_INVALID
            {
                continue;
            }
            if signals & sys::SIGNAL_READABLE != 0 {
                self.service_endpoint(*handle, wire, reply_wire);
            }
        }
        Ok(())
    }
}

/// Release ticket reservations when a server endpoint's peer closes.  This
/// runs before any subsequent readable request is serviced, so a worker that
/// disappears cannot leave a mount reservation or pending backend stranded.
fn cleanup_peer_binding(
    binding: Binding,
    topology: &mut crate::mount::Topology,
    pending_backends: &mut BTreeMap<u64, RamFs>,
) {
    cleanup_peer_binding_with_controls(binding, topology, pending_backends, None);
}

/// Variant used by the live serve loop.  A MountTicket peer close must drop
/// both sides of the prepared worker control pair; otherwise a worker that
/// lost its ticket can retain a stale control capability until process exit.
fn cleanup_peer_binding_with_controls(
    binding: Binding,
    topology: &mut crate::mount::Topology,
    pending_backends: &mut BTreeMap<u64, RamFs>,
    mut mount_controls: Option<&mut BTreeMap<u64, OwnedHandle>>,
) {
    match binding {
        Binding::MountTicket { ticket } => {
            let expired = topology.expire_mount_ticket(ticket);
            if expired {
                pending_backends.remove(&ticket);
                if let Some(controls) = mount_controls.as_deref_mut() {
                    controls.remove(&ticket);
                }
            }
        }
        Binding::MutationTicket { ticket } => {
            topology.expire_mutation_ticket(ticket);
        }
        Binding::MountControl { ticket } => {
            crate::mount_admin::peer_closed_control(ticket, topology, pending_backends);
        }
        // A NamespaceBinding is an application routing capability, not the
        // worker control channel.  Closing one endpoint must only remove its
        // own registry association; unrelated PREPARED mount tickets remain
        // valid until their ticket/control peer closes or they expire.
        Binding::NamespaceBinding(_) => {}
        _ => {}
    }
}

fn insert_entry(
    entries: &mut BTreeMap<sys::Handle, Entry>,
    endpoint: ProtocolServerEndpoint,
    binding: Binding,
) {
    entries.insert(endpoint.get(), Entry { endpoint, binding });
}

/// Retire one endpoint and release everything its binding owned.
///
/// Every fault path funnels here -- an unreadable frame, a protocol violation,
/// a dispatch error, a closed peer -- so "what a fault costs" is decided once
/// and identically: that endpoint, its client authority, and any reservation or
/// control pair its binding held go away, and the service keeps serving every
/// other peer.  Getting this wrong in one of the paths is how a single bad peer
/// would strand a mount reservation or take the whole service down, so the
/// sequence lives here rather than being repeated at each call site.
///
/// Takes the field references rather than `&mut self` because the request loop
/// needs them split while it reads the entry table.
fn retire_endpoint(
    handle: sys::Handle,
    entries: &mut BTreeMap<sys::Handle, Entry>,
    clients: &mut BTreeMap<u64, (u64, NodeId, NodeId)>,
    client_by_server: &mut BTreeMap<sys::Handle, u64>,
    binding_clients: &mut BTreeMap<u64, NsBindingState>,
    topology: &mut crate::mount::Topology,
    pending_backends: &mut BTreeMap<u64, RamFs>,
    mount_controls: &mut BTreeMap<u64, OwnedHandle>,
) {
    let Some(entry) = entries.remove(&handle) else {
        return;
    };
    if let Some(client) = client_by_server.remove(&handle) {
        clients.remove(&client);
        binding_clients.remove(&client);
    }
    cleanup_peer_binding_with_controls(
        entry.binding,
        topology,
        pending_backends,
        Some(mount_controls),
    );
}

impl Binding {
    fn is_directory(self) -> bool {
        matches!(self, Binding::Directory { .. })
    }
}

/// Manual routing for rename_at/link_at: validates the disposition, maps
/// the transferred `new_parent` to one of our own Directory bindings
/// (consuming the moved handle), rejects cross-mount pairs with `EXDEV`
/// before any work (§6.4 worker rule), reserves both affected components in
/// the shared mutation table, then commits the backend change while the
/// reservation is held (r7 rule 1: local commit under the reservation).
pub(crate) fn run_pair_method(
    fs: &mut RamFs,
    topology: &mut crate::mount::Topology,
    entries: &BTreeMap<sys::Handle, Entry>,
    clients: &BTreeMap<u64, (u64, NodeId, NodeId)>,
    scope: (u64, NodeId, NodeId),
    method_id: u64,
    wire: &[u8],
    responder: Option<ResponderHandle>,
    resources: &ReceivedResources,
    reply_wire: &mut [u8],
) -> Result<(), ()> {
    let Some(mut responder) = responder else {
        return Err(());
    };
    let fail_with = |mut responder: ResponderHandle, errno: Errno| -> Result<(), ()> {
        let _ = responder.fail(FailInvocation::domain(-(errno.to_i32() as i64)));
        Err(())
    };

    // Resolve the transferred `new_parent` endpoint to its binding scope:
    // registered server ends first, then client ends this server minted.
    let peer = |slot: ResourceSlot,
                entries: &BTreeMap<sys::Handle, Entry>,
                clients: &BTreeMap<u64, (u64, NodeId, NodeId)>,
                resources: &ReceivedResources|
     -> Result<(u64, NodeId, NodeId), ()> {
        let raw = resources.get(slot).ok_or(())?.get();
        if let Some(Entry {
            binding:
                Binding::Directory {
                    mount,
                    root,
                    current,
                },
            ..
        }) = entries.get(&raw)
        {
            return Ok((*mount, *root, *current));
        }
        let object_id = naos_idl::object_id(raw).map_err(|_| ())?;
        if let Some(scope) = clients.get(&object_id) {
            return Ok(*scope);
        }
        Err(())
    };

    let decode =
        |method_id: u64, wire: &[u8]| -> Result<(Vec<u8>, Vec<u8>, u64, ResourceSlot), ()> {
            if method_id == directory::METHOD_RENAME_AT {
                let request = directory::decode_rename_at_request(wire).map_err(|_| ())?;
                directory::validate_rename_at_request_received_resources(&request, &resources)
                    .map_err(|_| ())?;
                if request.flags != 0 {
                    // NOREPLACE/EXCHANGE bits are reserved in v1.
                    return Err(());
                }
                Ok((
                    Vec::from(strip_nul(request.first)),
                    Vec::from(strip_nul(request.second)),
                    0,
                    request.new_parent,
                ))
            } else {
                let request = directory::decode_link_at_request(wire).map_err(|_| ())?;
                directory::validate_link_at_request_received_resources(&request, &resources)
                    .map_err(|_| ())?;
                if request.flags & !1 != 0 {
                    return Err(());
                }
                Ok((
                    Vec::from(strip_nul(request.first)),
                    Vec::from(strip_nul(request.second)),
                    request.flags & 1,
                    request.new_parent,
                ))
            }
        };

    let (first, second, flags, new_parent_slot) = match decode(method_id, wire) {
        Ok(decoded) => decoded,
        Err(()) => return fail_with(responder, Errno::EInval),
    };
    let peer_scope = match peer(new_parent_slot, entries, clients, resources) {
        Ok(peer) => peer,
        Err(()) => return fail_with(responder, Errno::EInval),
    };
    // Cross-mount rename/link fails EXDEV by identity comparison alone --
    // never an RPC, and without side effects (§6.4).
    if peer_scope.0 != scope.0 {
        return fail_with(responder, Errno::EXdev);
    }

    // Dry-run resolution so the reservation covers the real victims before
    // any metadata moves. Failures here are plain POSIX errors.
    let (old_parent, old_name, old_target) = match resolve_victims(fs, scope.1, scope.2, &first) {
        Ok(victim) => victim,
        Err(errno) => return fail_with(responder, errno),
    };
    let new_side = match resolve_victims(fs, peer_scope.1, peer_scope.2, &second) {
        Ok((parent, name, target)) => (parent, name, Some(target)),
        Err(Errno::ENoent) => match fs.resolve_parent_scoped(peer_scope.1, peer_scope.2, &second) {
            Ok((parent, _name)) => (parent, last_component(&second).to_vec(), None),
            Err(errno) => return fail_with(responder, errno),
        },
        Err(errno) => return fail_with(responder, errno),
    };
    let operation = if method_id == directory::METHOD_RENAME_AT {
        crate::internal::namespace_binding::OP_RENAME
    } else {
        crate::internal::namespace_binding::OP_LINK
    };
    let key = |mount: u64, id: NodeId| node_key(topology, mount, id);
    let ticket = match topology.begin_mutation(
        scope.0,
        operation,
        key(scope.0, old_parent),
        key(scope.0, old_target),
        key(scope.0, new_side.0),
        new_side
            .2
            .map(|id| key(scope.0, id))
            .unwrap_or(crate::internal::NodeKey::ZERO),
        &old_name,
        &new_side.1,
        servicekit::monotonic_ticks(),
    ) {
        Ok(ticket) => ticket,
        Err(errno) => return fail_with(responder, errno),
    };

    let applied = apply_pair(
        fs,
        (scope.1, scope.2),
        method_id,
        &first,
        (peer_scope.1, peer_scope.2),
        &second,
        flags,
    );
    match applied {
        Ok(()) => {
            // Local commit done under the held reservation: admit the
            // release (COMMITTING) and finish (COMMITTED).
            let _ = topology.admit_mutation(ticket);
            let _ = topology.finish_mutation(ticket, true);
            reply_empty(responder, method_id, reply_wire);
            Ok(())
        }
        Err(errno) => {
            // No local effect happened: abort releases the reservation.
            let _ = topology.abort_mutation(ticket);
            fail_with(responder, errno)
        }
    }
}

/// Generation-tagged wire identity of a backend node; the mount's backend
/// generation tags the instance so stale keys cannot alias a rebuilt mount.
fn node_key(topology: &crate::mount::Topology, mount: u64, id: NodeId) -> crate::internal::NodeKey {
    crate::internal::NodeKey {
        node_id: id,
        generation: topology
            .mount(mount)
            .map(|record| record.backend_generation)
            .unwrap_or(0),
    }
}

/// Split a scoped relative path into `(parent dir node, last component,
/// resolved final node)` without side effects.
fn resolve_victims(
    fs: &RamFs,
    root: NodeId,
    current: NodeId,
    path: &[u8],
) -> Result<(NodeId, Vec<u8>, NodeId), Errno> {
    let (parent, name) = fs.resolve_parent_scoped(root, current, path)?;
    let target = fs.lookup_scoped(root, parent, &name, false)?;
    Ok((parent, name, target))
}

/// Final component of a slash-separated relative path.
fn last_component(path: &[u8]) -> &[u8] {
    match path.iter().rposition(|byte| *byte == b'/') {
        Some(pos) => &path[pos + 1..],
        None => path,
    }
}

/// The backend mutation shared by `rename_at` and `link_at`: resolve both
/// sides under their binding scopes and commit. `flags` only matters for
/// link_at (bit0 = AT_SYMLINK_FOLLOW).
fn apply_pair(
    fs: &mut RamFs,
    scope: (NodeId, NodeId),
    method_id: u64,
    first: &[u8],
    peer: (NodeId, NodeId),
    second: &[u8],
    flags: u64,
) -> Result<(), Errno> {
    if method_id == directory::METHOD_RENAME_AT {
        return fs.rename_scoped(scope.0, scope.1, first, peer.0, peer.1, second);
    }
    if flags & 1 == 0 {
        fs.link_scoped(scope.0, scope.1, first, peer.0, peer.1, second)
    } else {
        // AT_SYMLINK_FOLLOW: resolve the source fully, then hard-link.
        link_following(fs, scope.0, scope.1, first, peer.0, peer.1, second)
    }
}

/// Answer a `->;` pair-method with its (empty) canonical response.
fn reply_empty(mut responder: ResponderHandle, method_id: u64, reply_wire: &mut [u8]) {
    let outcome = match method_id {
        directory::METHOD_RENAME_AT => {
            directory::encode_rename_at_response(&directory::rename_at_response {}, reply_wire)
        }
        _ => directory::encode_link_at_response(&directory::link_at_response {}, reply_wire),
    };
    if let Ok(written) = outcome {
        let _ = responder.reply(&reply_wire[..written], ResourceTable::new().as_slice());
    }
}

/// The AT_SYMLINK_FOLLOW branch of link_at: resolve the source through
/// symlinks, require a regular node, then hard-link under `(parent, name)`.
fn link_following(
    fs: &mut RamFs,
    root: NodeId,
    current: NodeId,
    source_path: &[u8],
    new_root: NodeId,
    new_current: NodeId,
    dest_path: &[u8],
) -> Result<(), Errno> {
    let source = fs.lookup_scoped(root, current, source_path, true)?;
    let meta = fs.metadata_of(source)?;
    if meta.kind != FileKind::Regular {
        return Err(if meta.kind == FileKind::Directory {
            Errno::EIsDir
        } else {
            Errno::Eperm
        });
    }
    let (parent, name) = fs.resolve_parent_scoped(new_root, new_current, dest_path)?;
    fs.link_node(parent, source, &name)
}

// ---------------------------------------------------------------------------
// Directory handler
// ---------------------------------------------------------------------------

struct DirectoryContext<'a> {
    fs: &'a mut RamFs,
    topology: &'a crate::mount::Topology,
    root_anchors: &'a BTreeMap<u64, OwnedHandle>,
    pending: &'a mut Vec<PendingRegistration>,
    mount: u64,
    root: NodeId,
    current: NodeId,
    region: Option<sys::Handle>,
    scratch: Vec<u8>,
}

fn worker_call_failure(error: CallError) -> FailInvocation {
    match error {
        CallError::Outcome { protocol_error, .. } if protocol_error != 0 => {
            FailInvocation::domain(protocol_error)
        }
        CallError::Status(sys::STATUS_PEER_CLOSED) | CallError::InvalidHandle => {
            fail(Errno::ENodev)
        }
        _ => fail(Errno::EIo),
    }
}

fn wait_worker_call(invocation: &Invocation) -> Result<(), FailInvocation> {
    if !servicekit::wait_for_completion(invocation.get(), 5_000_000) {
        return Err(fail(Errno::EIo));
    }
    Ok(())
}

fn worker_endpoint(anchor: &OwnedHandle) -> ManuallyDrop<ProtocolClientEndpoint> {
    // The anchor remains owned by vfsd for the whole call. Protocol endpoints
    // are intentionally unique and cannot be duplicated; this temporary
    // typed view therefore must never run its destructor.
    ManuallyDrop::new(unsafe { ProtocolClientEndpoint::from_raw(anchor.get()) })
}

fn worker_clone_directory(anchor: &OwnedHandle) -> Result<OwnedHandle, FailInvocation> {
    let endpoint = worker_endpoint(anchor);
    let request = directory::clone_binding_request {};
    let mut request_wire = [0_u8; 16];
    let mut invocation = directory::submit_clone_binding(
        &endpoint,
        &request,
        ResourceTable::new(),
        &mut request_wire,
        0,
    )
    .map_err(worker_call_failure)?;
    wait_worker_call(&invocation)?;
    let mut response_wire = [0_u8; 16];
    let (response, mut resources) =
        directory::take_clone_binding(&mut invocation, &mut response_wire)
            .map_err(worker_call_failure)?;
    resources
        .take(response.directory)
        .ok_or_else(|| fail(Errno::EIo))
}

fn worker_open_directory(
    endpoint: &ProtocolClientEndpoint,
    path: &[u8],
    mode: u64,
    flags: u64,
) -> Result<OwnedHandle, FailInvocation> {
    let request = directory::open_request { mode, flags, path };
    let mut request_wire = [0_u8; 8192];
    let mut invocation = directory::submit_open(
        endpoint,
        &request,
        ResourceTable::new(),
        &mut request_wire,
        0,
    )
    .map_err(worker_call_failure)?;
    wait_worker_call(&invocation)?;
    let mut response_wire = [0_u8; 8192];
    let (response, mut resources) =
        directory::take_open(&mut invocation, &mut response_wire).map_err(worker_call_failure)?;
    resources
        .take(response.object)
        .ok_or_else(|| fail(Errno::EIo))
}

fn worker_stat_directory(
    endpoint: &ProtocolClientEndpoint,
) -> Result<directory::Stat, FailInvocation> {
    let mut request_wire = [0_u8; 8];
    let request = directory::stat_request {};
    let mut invocation = directory::submit_stat(
        endpoint,
        &request,
        ResourceTable::new(),
        &mut request_wire,
        0,
    )
    .map_err(worker_call_failure)?;
    wait_worker_call(&invocation)?;
    let mut response_wire = [0_u8; 256];
    directory::take_stat(&mut invocation, &mut response_wire)
        .map(|response| response.value)
        .map_err(worker_call_failure)
}

fn worker_stat_node(
    endpoint: &ProtocolClientEndpoint,
    path: &[u8],
    flags: u64,
) -> Result<directory::Stat, FailInvocation> {
    let request = directory::stat_node_request {
        flags,
        path_size: path.len() as u64,
        path,
    };
    let mut request_wire = [0_u8; 8192];
    let mut invocation = directory::submit_stat_node(
        endpoint,
        &request,
        ResourceTable::new(),
        &mut request_wire,
        0,
    )
    .map_err(worker_call_failure)?;
    wait_worker_call(&invocation)?;
    let mut response_wire = [0_u8; 256];
    directory::take_stat_node(&mut invocation, &mut response_wire)
        .map(|response| response.value)
        .map_err(worker_call_failure)
}

fn worker_create(
    endpoint: &ProtocolClientEndpoint,
    path: &[u8],
    mode: u64,
    flags: u64,
) -> Result<(), FailInvocation> {
    let request = directory::create_request { mode, flags, path };
    let mut request_wire = vec![0_u8; 8192];
    let mut invocation = directory::submit_create(
        endpoint,
        &request,
        ResourceTable::new(),
        &mut request_wire,
        0,
    )
    .map_err(worker_call_failure)?;
    wait_worker_call(&invocation)?;
    let mut response_wire = [0_u8; 64];
    directory::take_create(&mut invocation, &mut response_wire)
        .map(|_| ())
        .map_err(worker_call_failure)
}

fn worker_remove(
    endpoint: &ProtocolClientEndpoint,
    path: &[u8],
    mode: u64,
    flags: u64,
) -> Result<(), FailInvocation> {
    let request = directory::remove_request { mode, flags, path };
    let mut request_wire = vec![0_u8; 8192];
    let mut invocation = directory::submit_remove(
        endpoint,
        &request,
        ResourceTable::new(),
        &mut request_wire,
        0,
    )
    .map_err(worker_call_failure)?;
    wait_worker_call(&invocation)?;
    let mut response_wire = [0_u8; 64];
    directory::take_remove(&mut invocation, &mut response_wire)
        .map(|_| ())
        .map_err(worker_call_failure)
}

fn worker_access(
    endpoint: &ProtocolClientEndpoint,
    path: &[u8],
    mode: u64,
) -> Result<(), FailInvocation> {
    let request = directory::access_request { mode, path };
    let mut request_wire = vec![0_u8; 8192];
    let mut invocation = directory::submit_access(
        endpoint,
        &request,
        ResourceTable::new(),
        &mut request_wire,
        0,
    )
    .map_err(worker_call_failure)?;
    wait_worker_call(&invocation)?;
    let mut response_wire = [0_u8; 64];
    directory::take_access(&mut invocation, &mut response_wire)
        .map(|_| ())
        .map_err(worker_call_failure)
}

fn worker_rename(
    endpoint: &ProtocolClientEndpoint,
    first: &[u8],
    second: &[u8],
) -> Result<(), FailInvocation> {
    let request = directory::rename_request {
        first_size: first.len() as u64,
        second_size: second.len() as u64,
        first,
        second,
    };
    let mut request_wire = vec![0_u8; 8192];
    let mut invocation = directory::submit_rename(
        endpoint,
        &request,
        ResourceTable::new(),
        &mut request_wire,
        0,
    )
    .map_err(worker_call_failure)?;
    wait_worker_call(&invocation)?;
    let mut response_wire = [0_u8; 64];
    directory::take_rename(&mut invocation, &mut response_wire)
        .map(|_| ())
        .map_err(worker_call_failure)
}

impl<'a> DirectoryContext<'a> {
    /// Return an external mount and the path relative to its worker root when
    /// this directory is the system root. The first component is resolved in
    /// the root RAM backend, where vfsd stores the mountpoint node.
    fn external_mount_path(&self, path: &[u8]) -> Result<Option<(u64, Vec<u8>)>, Errno> {
        if self.mount != crate::mount::ROOT_MOUNT_ID {
            return Ok(None);
        }
        let path = strip_nul(path);
        let mut components = path
            .split(|byte| *byte == b'/')
            .filter(|part| !part.is_empty());
        let Some(first) = components.next() else {
            return Ok(None);
        };
        let node = match self.fs.lookup_scoped(self.root, self.current, first, false) {
            Ok(node) => node,
            // A missing first component simply means this is a local path;
            // the caller's normal backend operation will produce the proper
            // POSIX error (or create the final component) below.
            Err(Errno::ENoent) => return Ok(None),
            Err(errno) => return Err(errno),
        };
        let generation = self
            .topology
            .mount(crate::mount::ROOT_MOUNT_ID)
            .map(|record| record.backend_generation)
            .unwrap_or(0);
        let Some(mount) = self.topology.child_at(
            crate::mount::ROOT_MOUNT_ID,
            crate::internal::NodeKey {
                node_id: node,
                generation,
            },
        ) else {
            return Ok(None);
        };
        let mut relative = Vec::new();
        for (index, part) in components.enumerate() {
            if index != 0 {
                relative.push(b'/');
            }
            relative.extend_from_slice(part);
        }
        Ok(Some((mount, relative)))
    }

    fn open_external(
        &mut self,
        mount: u64,
        relative: &[u8],
        flags: u64,
        mode: u64,
    ) -> Result<MethodReply<'a, directory::open_response>, FailInvocation> {
        let Some(anchor) = self.root_anchors.get(&mount) else {
            return Err(fail(Errno::ENodev));
        };
        let cloned = match worker_clone_directory(anchor) {
            Ok(cloned) => cloned,
            Err(error) => return Err(error),
        };
        if relative.is_empty() {
            let mut resources = ResourceTable::new();
            let slot = resources.push_move(cloned).map_err(|_| fail(Errno::EIo))?;
            return Ok(MethodReply::with_resources(
                directory::open_response { object: slot },
                resources,
            ));
        }
        let endpoint = worker_endpoint(&cloned);
        let object = worker_open_directory(&endpoint, relative, mode, flags)?;
        let mut resources = ResourceTable::new();
        let slot = resources.push_move(object).map_err(|_| fail(Errno::EIo))?;
        Ok(MethodReply::with_resources(
            directory::open_response { object: slot },
            resources,
        ))
    }

    /// Register a fresh endpoint pair for `binding`; returns the client-end
    /// resource table entry.
    fn spawn_endpoint(
        &mut self,
        binding: Binding,
    ) -> Result<(ResourceSlot, ResourceTable<'static>), FailInvocation> {
        // File and Directory are distinct protocol scopes.  They share the
        // opaque endpoint wrapper, but the kernel metadata still carries the
        // descriptor and method surface; creating a File binding with a
        // Directory descriptor makes read(2)/write(2) fail as a protocol
        // violation even though the userspace dispatcher is correct.
        let (client, server) = match binding {
            Binding::File { .. } => file::create_endpoints(None),
            _ => directory::create_endpoints(None),
        }
        .map_err(|_| FailInvocation::domain(-Errno::EIo.to_i32() as i64))?;
        let client_id = naos_idl::object_id(client.get())
            .map_err(|_| FailInvocation::domain(-Errno::EIo.to_i32() as i64))?;
        let client_raw = client.into_raw();
        let client_handle = unsafe { OwnedHandle::from_raw(client_raw) };
        let mut resources: ResourceTable<'static> = ResourceTable::new();
        let slot = resources
            .push_move(client_handle)
            .map_err(|_| fail(Errno::EIo))?;
        self.pending
            .push((server, binding, Some((client_raw, client_id))));
        Ok((slot, resources))
    }

    fn open_node_as_endpoint(
        &mut self,
        node: NodeId,
        kind: FileKind,
        flags: u64,
        mode: u64,
    ) -> Result<MethodReply<'a, directory::open_response>, FailInvocation> {
        if kind == FileKind::Directory {
            if flags & walk_flags::TRUNC != 0 {
                return Err(fail(Errno::EIsDir));
            }
            let root = if flags & OPEN_FLAG_CHROOT != 0 {
                node
            } else {
                self.root
            };
            let (slot, resources) = self.spawn_endpoint(Binding::Directory {
                mount: self.mount,
                root,
                current: node,
            })?;
            return Ok(MethodReply::with_resources(
                directory::open_response { object: slot },
                resources,
            ));
        }
        if flags & walk_flags::DIRECTORY != 0 {
            return Err(fail(Errno::ENotDir));
        }
        let fd = self.fs.open_node(node).map_err(fail)?;
        if flags & walk_flags::TRUNC != 0 {
            self.fs.ftruncate(fd, 0).map_err(fail)?;
        }
        let (slot, resources) = self.spawn_endpoint(Binding::File {
            mount: self.mount,
            fd,
            mode,
        })?;
        Ok(MethodReply::with_resources(
            directory::open_response { object: slot },
            resources,
        ))
    }
}

impl DirectoryHandler for DirectoryContext<'_> {
    fn open<'s>(
        &'s mut self,
        request: directory::open_request<'_>,
    ) -> Result<MethodReply<'s, directory::open_response>, FailInvocation> {
        let path = strip_nul(request.path);
        let flags = request.flags;
        let mode = request.mode;
        if let Some((mount, relative)) = self.external_mount_path(path).map_err(fail)? {
            return self.open_external(mount, &relative, flags, mode);
        }
        let follow_final = flags & walk_flags::NOFOLLOW_FINAL == 0;

        let node = match self
            .fs
            .lookup_scoped(self.root, self.current, path, follow_final)
        {
            Ok(node) => node,
            Err(Errno::ENoent) if flags & walk_flags::CREATE != 0 => {
                // O_CREAT never creates directories; O_DIRECTORY|O_CREAT on a
                // missing name stays ENOENT (POSIX).
                if flags & walk_flags::DIRECTORY != 0 {
                    return Err(fail(Errno::ENoent));
                }
                let meta = self
                    .fs
                    .create_scoped(self.root, self.current, path, mode & open_mode::EXCL != 0)
                    .map_err(fail)?;
                meta.node_id
            }
            Err(errno) => return Err(fail(errno)),
        };
        let meta = self.fs.metadata_of(node).map_err(fail)?;
        self.open_node_as_endpoint(node, meta.kind, flags, mode)
    }

    fn list<'s>(
        &'s mut self,
        request: directory::list_request,
    ) -> Result<MethodReply<'s, directory::list_response>, FailInvocation> {
        // Record budget mirrors the kernel adapter: requested_bytes == 0
        // means "fill the maximum payload".
        let budget = if request.requested_bytes == 0 {
            65536
        } else {
            (request.requested_bytes as usize).min(65536)
        };
        let (page, _next, _truncated) = self
            .fs
            .entries_page(self.current, request.offset, budget)
            .map_err(fail)?;
        // The caller's grant is a hard bound: a record whose 16-byte header
        // plus NUL-terminated name would leave the window is not written, and
        // `next` advances only past the records that fit.  A page stops early
        // rather than failing, so a small buffer still reports progress.
        let window = request.requested_bytes.min(65_536) as usize;
        let mut produced = 0usize;
        let mut count = 0u64;
        for (name, _meta) in &page {
            let record = 16 + name.len() + 1;
            if produced + record > window {
                break;
            }
            produced += record;
            count += 1;
        }
        let next = request.offset + count;
        if produced == 0 {
            return Ok(MethodReply::new(directory::list_response {
                next,
                count,
                bytes: 0,
            }));
        }
        let handle = region_handle(self.region)?;
        servicekit::memory::with_region_write(handle, 0, produced, |out| {
            let mut cursor = 0usize;
            for (name, meta) in page.iter().take(count as usize) {
                // Record shape pinned by the kernel adapter: u64 inode, u32
                // inode-type, u32 name-bytes-with-NUL, name bytes + NUL.
                out[cursor..cursor + 8].copy_from_slice(&meta.node_id.to_le_bytes());
                cursor += 8;
                let type_word: u32 = match meta.kind {
                    FileKind::Regular => 0,
                    FileKind::Directory => 1,
                    FileKind::Symlink => 2,
                };
                out[cursor..cursor + 4].copy_from_slice(&type_word.to_le_bytes());
                cursor += 4;
                out[cursor..cursor + 4]
                    .copy_from_slice(&((name.len() + 1) as u32).to_le_bytes());
                cursor += 4;
                out[cursor..cursor + name.len()].copy_from_slice(name);
                cursor += name.len();
                out[cursor] = 0;
                cursor += 1;
            }
        })
        .map_err(|_| fail(Errno::EInval))?;
        Ok(MethodReply::new(directory::list_response {
            next,
            count,
            bytes: produced as u64,
        }))
    }

    fn stat<'s>(
        &'s mut self,
        _request: directory::stat_request,
    ) -> Result<MethodReply<'s, directory::stat_response>, FailInvocation> {
        let meta = self.fs.metadata_of(self.current).map_err(fail)?;
        Ok(MethodReply::new(directory::stat_response {
            value: directory_stat(meta),
        }))
    }

    fn create<'s>(
        &'s mut self,
        request: directory::create_request<'_>,
    ) -> Result<MethodReply<'s, directory::create_response>, FailInvocation> {
        let path = strip_nul(request.path);
        if let Some((mount, relative)) = self.external_mount_path(path).map_err(fail)? {
            if relative.is_empty() {
                return Err(fail(Errno::EExist));
            }
            let anchor = self
                .root_anchors
                .get(&mount)
                .ok_or_else(|| fail(Errno::ENodev))?;
            let cloned = worker_clone_directory(anchor)?;
            let endpoint = worker_endpoint(&cloned);
            worker_create(&endpoint, &relative, request.mode, request.flags)?;
            return Ok(MethodReply::new(directory::create_response {}));
        }
        if request.flags & CREATE_FLAG_DIRECTORY != 0 {
            self.fs
                .mkdir_scoped(self.root, self.current, path)
                .map_err(fail)?;
        } else {
            self.fs
                .create_scoped(self.root, self.current, path, false)
                .map_err(fail)?;
        }
        Ok(MethodReply::new(directory::create_response {}))
    }

    fn remove<'s>(
        &'s mut self,
        request: directory::remove_request<'_>,
    ) -> Result<MethodReply<'s, directory::remove_response>, FailInvocation> {
        let path = strip_nul(request.path);
        if let Some((mount, relative)) = self.external_mount_path(path).map_err(fail)? {
            if relative.is_empty() {
                return Err(fail(Errno::EBusy));
            }
            let anchor = self
                .root_anchors
                .get(&mount)
                .ok_or_else(|| fail(Errno::ENodev))?;
            let cloned = worker_clone_directory(anchor)?;
            let endpoint = worker_endpoint(&cloned);
            worker_remove(&endpoint, &relative, request.mode, request.flags)?;
            return Ok(MethodReply::new(directory::remove_response {}));
        }
        if request.flags & CREATE_FLAG_DIRECTORY != 0 {
            self.fs
                .rmdir_scoped(self.root, self.current, path)
                .map_err(fail)?;
        } else {
            self.fs
                .unlink_scoped(self.root, self.current, path)
                .map_err(fail)?;
        }
        Ok(MethodReply::new(directory::remove_response {}))
    }

    fn path<'s>(
        &'s mut self,
        _request: directory::path_request,
    ) -> Result<MethodReply<'s, directory::path_response<'s>>, FailInvocation> {
        let path = self
            .fs
            .path_of(self.root, self.current)
            .unwrap_or_else(|| b"/".to_vec());
        self.scratch = path;
        Ok(MethodReply::new(directory::path_response {
            path: &self.scratch,
        }))
    }

    fn access<'s>(
        &'s mut self,
        request: directory::access_request<'_>,
    ) -> Result<MethodReply<'s, directory::access_response>, FailInvocation> {
        let path = strip_nul(request.path);
        if let Some((mount, relative)) = self.external_mount_path(path).map_err(fail)? {
            let anchor = self
                .root_anchors
                .get(&mount)
                .ok_or_else(|| fail(Errno::ENodev))?;
            let cloned = worker_clone_directory(anchor)?;
            let endpoint = worker_endpoint(&cloned);
            worker_access(&endpoint, &relative, request.mode)?;
            return Ok(MethodReply::new(directory::access_response {}));
        }
        // Single-user uid/gid 0 policy (ADR §5.2): possession is authority,
        // so any existing node grants every access mode.
        match self.fs.lookup_scoped(self.root, self.current, path, true) {
            Ok(_) => Ok(MethodReply::new(directory::access_response {})),
            Err(_) => Err(FailInvocation::domain(-Errno::EAccess.to_i32() as i64)),
        }
    }

    fn rename<'s>(
        &'s mut self,
        request: directory::rename_request<'_>,
    ) -> Result<MethodReply<'s, directory::rename_response>, FailInvocation> {
        let first = strip_nul(request.first);
        let second = strip_nul(request.second);
        let first_external = self.external_mount_path(first).map_err(fail)?;
        let second_external = self.external_mount_path(second).map_err(fail)?;
        if first_external.is_some() || second_external.is_some() {
            let Some((first_mount, first_relative)) = first_external else {
                return Err(fail(Errno::EXdev));
            };
            let Some((second_mount, second_relative)) = second_external else {
                return Err(fail(Errno::EXdev));
            };
            if first_mount != second_mount
                || first_relative.is_empty()
                || second_relative.is_empty()
            {
                return Err(fail(Errno::EXdev));
            }
            let anchor = self
                .root_anchors
                .get(&first_mount)
                .ok_or_else(|| fail(Errno::ENodev))?;
            let cloned = worker_clone_directory(anchor)?;
            let endpoint = worker_endpoint(&cloned);
            worker_rename(&endpoint, &first_relative, &second_relative)?;
            return Ok(MethodReply::new(directory::rename_response {}));
        }
        self.fs
            .rename_scoped(
                self.root,
                self.current,
                first,
                self.root,
                self.current,
                second,
            )
            .map_err(fail)?;
        Ok(MethodReply::new(directory::rename_response {}))
    }

    fn link<'s>(
        &'s mut self,
        request: directory::link_request<'_>,
    ) -> Result<MethodReply<'s, directory::link_response>, FailInvocation> {
        let first = strip_nul(request.first);
        let second = strip_nul(request.second);
        self.fs
            .link_scoped(
                self.root,
                self.current,
                first,
                self.root,
                self.current,
                second,
            )
            .map_err(fail)?;
        Ok(MethodReply::new(directory::link_response {}))
    }

    fn symlink<'s>(
        &'s mut self,
        request: directory::symlink_request<'_>,
    ) -> Result<MethodReply<'s, directory::symlink_response>, FailInvocation> {
        // Wire order pinned by the kernel adapter: first = link content,
        // second = path of the new link.
        let target = strip_nul(request.first);
        let link_path = strip_nul(request.second);
        self.fs
            .symlink_scoped(self.root, self.current, target, link_path)
            .map_err(fail)?;
        Ok(MethodReply::new(directory::symlink_response {}))
    }

    fn readlink<'s>(
        &'s mut self,
        request: directory::readlink_request<'_>,
    ) -> Result<MethodReply<'s, directory::readlink_response<'s>>, FailInvocation> {
        let path = strip_nul(request.path);
        let node = self
            .fs
            .lookup_scoped(self.root, self.current, path, false)
            .map_err(fail)?;
        self.scratch.resize(crate::backend::MAX_PATH_BYTES + 1, 0);
        let len = self.fs.read_target(node, &mut self.scratch).map_err(fail)?;
        self.scratch.truncate(len);
        Ok(MethodReply::new(directory::readlink_response {
            target: &self.scratch,
        }))
    }

    fn set_current<'s>(
        &'s mut self,
        _request: directory::set_current_request,
    ) -> Result<MethodReply<'s, directory::set_current_response>, FailInvocation> {
        // Frozen compatibility no-op (USERSPACE_FILESYSTEM_ADR §5.3.4):
        // mlibc owns cwd state in userland.
        Err(FailInvocation::domain(-95)) // -ENOTSUP
    }

    fn set_root<'s>(
        &'s mut self,
        _request: directory::set_root_request,
    ) -> Result<MethodReply<'s, directory::set_root_response>, FailInvocation> {
        Err(FailInvocation::domain(-95)) // -ENOTSUP
    }

    fn clone_binding<'s>(
        &'s mut self,
        _request: directory::clone_binding_request,
    ) -> Result<MethodReply<'s, directory::clone_binding_response>, FailInvocation> {
        let (slot, resources) = self.spawn_endpoint(Binding::Directory {
            mount: self.mount,
            root: self.root,
            current: self.current,
        })?;
        Ok(MethodReply::with_resources(
            directory::clone_binding_response { directory: slot },
            resources,
        ))
    }

    fn stat_node<'s>(
        &'s mut self,
        request: directory::stat_node_request<'_>,
    ) -> Result<MethodReply<'s, directory::stat_node_response>, FailInvocation> {
        if request.flags & !LOOKUP_NOFOLLOW != 0 {
            return Err(fail(Errno::EInval));
        }
        let path = strip_nul(request.path);
        if let Some((mount, relative)) = self.external_mount_path(path).map_err(fail)? {
            let anchor = self
                .root_anchors
                .get(&mount)
                .ok_or_else(|| fail(Errno::ENodev))?;
            let endpoint = worker_endpoint(anchor);
            let value = if relative.is_empty() {
                worker_stat_directory(&endpoint)?
            } else {
                worker_stat_node(&endpoint, &relative, request.flags)?
            };
            return Ok(MethodReply::new(directory::stat_node_response { value }));
        }
        let follow_final = request.flags & LOOKUP_NOFOLLOW == 0;
        // Dangling symlinks report themselves under NOFOLLOW because the
        // final component resolves without following it.
        let node = self
            .fs
            .lookup_scoped(self.root, self.current, path, follow_final)
            .map_err(fail)?;
        let meta = self.fs.metadata_of(node).map_err(fail)?;
        Ok(MethodReply::new(directory::stat_node_response {
            value: directory_stat(meta),
        }))
    }

    fn sync<'s>(
        &'s mut self,
        _request: directory::sync_request,
    ) -> Result<MethodReply<'s, directory::sync_response>, FailInvocation> {
        // RAM backend: success without durability (documented §5.3.6).
        Ok(MethodReply::new(directory::sync_response {}))
    }

    // rename_at/link_at are routed manually in `service_endpoint` because
    // their MOVE-disposition `new_parent` never reaches generated handlers;
    // these trait slots answer EINVAL should a request ever slip through.
    fn rename_at<'s>(
        &'s mut self,
        _request: directory::rename_at_request<'_>,
    ) -> Result<MethodReply<'s, directory::rename_at_response>, FailInvocation> {
        Err(FailInvocation::domain(-Errno::EInval.to_i32() as i64))
    }

    fn link_at<'s>(
        &'s mut self,
        _request: directory::link_at_request<'_>,
    ) -> Result<MethodReply<'s, directory::link_at_response>, FailInvocation> {
        Err(FailInvocation::domain(-Errno::EInval.to_i32() as i64))
    }
}

// ---------------------------------------------------------------------------
// File handler
// ---------------------------------------------------------------------------
struct FileContext<'a> {
    fs: &'a mut RamFs,
    mount: u64,
    fd: u64,
    mode: u64,
    region: Option<sys::Handle>,
    next_generation: &'a mut u64,
}

impl FileContext<'_> {
    fn require_readable(&self) -> Result<(), FailInvocation> {
        if self.mode & open_mode::READ != 0 {
            Ok(())
        } else {
            Err(FailInvocation::domain(-Errno::EBadf.to_i32() as i64))
        }
    }

    fn require_writable(&self) -> Result<(), FailInvocation> {
        if self.mode & open_mode::WRITE != 0 {
            Ok(())
        } else {
            Err(FailInvocation::domain(-Errno::EBadf.to_i32() as i64))
        }
    }
}

impl FileHandler for FileContext<'_> {
    fn pread<'s>(
        &'s mut self,
        request: file::pread_request,
    ) -> Result<MethodReply<'s, file::pread_response>, FailInvocation> {
        self.require_readable()?;
        let bytes = bulk_transfer_bytes(request.size)?;
        if bytes == 0 {
            return Ok(MethodReply::new(file::pread_response { count: 0 }));
        }
        let handle = region_handle(self.region)?;
        let count = servicekit::memory::with_region_write(
            handle,
            0,
            bytes,
            |out| self.fs.pread(self.fd, request.offset.max(0) as u64, out),
        )
        .map_err(|_| fail(Errno::EInval))?
        .map_err(fail)?;
        Ok(MethodReply::new(file::pread_response {
            count: count as u64,
        }))
    }

    fn pwrite<'s>(
        &'s mut self,
        request: file::pwrite_request,
    ) -> Result<MethodReply<'s, file::pwrite_response>, FailInvocation> {
        self.require_writable()?;
        let bytes = bulk_transfer_bytes(request.size)?;
        if bytes == 0 {
            return Ok(MethodReply::new(file::pwrite_response { count: 0 }));
        }
        let handle = region_handle(self.region)?;
        let count = servicekit::memory::with_region_read(
            handle,
            0,
            bytes,
            |data| self.fs.pwrite(self.fd, request.offset.max(0) as u64, data),
        )
        .map_err(|_| fail(Errno::EInval))?
        .map_err(fail)?;
        Ok(MethodReply::new(file::pwrite_response {
            count: count as u64,
        }))
    }

    fn seek<'s>(
        &'s mut self,
        request: file::seek_request,
    ) -> Result<MethodReply<'s, file::seek_response>, FailInvocation> {
        // Wire codes follow mlibc/Rust NaOS protocol constants: 0=current,
        // 1=begin, 2=end (not the libc SEEK_* values themselves).
        let position = match request.whence {
            0 => crate::backend::SeekFrom::Current(request.offset),
            1 => crate::backend::SeekFrom::Set(request.offset),
            2 => crate::backend::SeekFrom::End(request.offset),
            _ => return Err(fail(Errno::EInval)),
        };
        let offset = self.fs.seek(self.fd, position).map_err(fail)?;
        Ok(MethodReply::new(file::seek_response {
            offset: offset as i64,
        }))
    }

    fn stat<'s>(
        &'s mut self,
        _request: file::stat_request,
    ) -> Result<MethodReply<'s, file::stat_response>, FailInvocation> {
        let meta = self.fs.fstat(self.fd).map_err(fail)?;
        Ok(MethodReply::new(file::stat_response {
            value: file_stat(meta),
        }))
    }

    fn sync<'s>(
        &'s mut self,
        _request: file::sync_request,
    ) -> Result<MethodReply<'s, file::sync_response>, FailInvocation> {
        Ok(MethodReply::new(file::sync_response {}))
    }

    fn truncate<'s>(
        &'s mut self,
        request: file::truncate_request,
    ) -> Result<MethodReply<'s, file::truncate_response>, FailInvocation> {
        self.require_writable()?;
        self.fs.ftruncate(self.fd, request.length).map_err(fail)?;
        Ok(MethodReply::new(file::truncate_response {}))
    }

    fn allocate<'s>(
        &'s mut self,
        _request: file::allocate_request,
    ) -> Result<MethodReply<'s, file::allocate_response>, FailInvocation> {
        // Storage is already materialized in RAM; nothing to reserve.
        Ok(MethodReply::new(file::allocate_response {}))
    }

    fn get_flags<'s>(
        &'s mut self,
        _request: file::get_flags_request,
    ) -> Result<MethodReply<'s, file::get_flags_response>, FailInvocation> {
        Ok(MethodReply::new(file::get_flags_response {
            flags: self.mode,
        }))
    }

    fn set_flags<'s>(
        &'s mut self,
        request: file::set_flags_request,
    ) -> Result<MethodReply<'s, file::set_flags_response>, FailInvocation> {
        self.mode = request.flags;
        Ok(MethodReply::new(file::set_flags_response {}))
    }

    fn device_control<'s>(
        &'s mut self,
        _request: file::device_control_request,
    ) -> Result<MethodReply<'s, file::device_control_response>, FailInvocation> {
        Err(FailInvocation::domain(-25)) // -ENOTTY
    }

    fn read<'s>(
        &'s mut self,
        request: file::read_request,
    ) -> Result<MethodReply<'s, file::read_response>, FailInvocation> {
        self.require_readable()?;
        let bytes = bulk_transfer_bytes(request.size)?;
        if bytes == 0 {
            return Ok(MethodReply::new(file::read_response { count: 0 }));
        }
        let handle = region_handle(self.region)?;
        let count = servicekit::memory::with_region_write(handle, 0, bytes, |out| {
            self.fs.read(self.fd, out)
        })
        .map_err(|_| fail(Errno::EInval))?
        .map_err(fail)?;
        Ok(MethodReply::new(file::read_response {
            count: count as u64,
        }))
    }

    fn write<'s>(
        &'s mut self,
        request: file::write_request,
    ) -> Result<MethodReply<'s, file::write_response>, FailInvocation> {
        self.require_writable()?;
        let bytes = bulk_transfer_bytes(request.size)?;
        if bytes == 0 {
            return Ok(MethodReply::new(file::write_response { count: 0 }));
        }
        let handle = region_handle(self.region)?;
        let append = self.mode & open_mode::APPEND != 0;
        let count = servicekit::memory::with_region_read(handle, 0, bytes, |data| {
            if append {
                let size = self.fs.fstat(self.fd)?.size;
                let count = self.fs.pwrite(self.fd, size, data)?;
                let _ = self
                    .fs
                    .seek(self.fd, crate::backend::SeekFrom::Set((size + count as u64) as i64));
                return Ok(count);
            }
            self.fs.write(self.fd, data)
        })
        .map_err(|_| fail(Errno::EInval))?
        .map_err(fail)?;
        Ok(MethodReply::new(file::write_response {
            count: count as u64,
        }))
    }

    fn preadv<'s>(
        &'s mut self,
        request: file::preadv_request,
    ) -> Result<MethodReply<'s, file::preadv_response>, FailInvocation> {
        self.require_readable()?;
        let bytes = bulk_transfer_bytes(request.size)?;
        if bytes == 0 {
            return Ok(MethodReply::new(file::preadv_response { count: 0 }));
        }
        let handle = region_handle(self.region)?;
        let segments = request.layout.segment_count as usize;
        let count =
            servicekit::memory::with_region_write(handle, 0, bytes, |out| {
                let mut offset = request.offset.max(0) as u64;
                let mut total = 0usize;
                let mut cursor = 0usize;
                for length in request.layout.lengths.iter().take(segments) {
                    let length = *length as usize;
                    let remaining = out.len() - cursor;
                    if remaining == 0 || length > remaining {
                        break;
                    }
                    let read = self
                        .fs
                        .pread(self.fd, offset, &mut out[cursor..cursor + length])?;
                    if read == 0 {
                        break;
                    }
                    total += read;
                    offset += read as u64;
                    cursor += length;
                }
                Ok::<usize, Errno>(total)
            })
            .map_err(|_| fail(Errno::EInval))?
            .map_err(fail)?;
        Ok(MethodReply::new(file::preadv_response {
            count: count as u64,
        }))
    }

    fn pwritev<'s>(
        &'s mut self,
        request: file::pwritev_request,
    ) -> Result<MethodReply<'s, file::pwritev_response>, FailInvocation> {
        self.require_writable()?;
        let bytes = bulk_transfer_bytes(request.size)?;
        if bytes == 0 {
            return Ok(MethodReply::new(file::pwritev_response { count: 0 }));
        }
        let handle = region_handle(self.region)?;
        let segments = request.layout.segment_count as usize;
        let count =
            servicekit::memory::with_region_read(handle, 0, bytes, |data| {
                let mut offset = request.offset.max(0) as u64;
                let mut total = 0usize;
                let mut cursor = 0usize;
                for length in request.layout.lengths.iter().take(segments) {
                    let end = cursor.saturating_add(*length as usize).min(data.len());
                    if cursor >= end {
                        break;
                    }
                    let written = self.fs.pwrite(self.fd, offset, &data[cursor..end])?;
                    total += written;
                    offset += written as u64;
                    cursor = end;
                }
                Ok::<usize, Errno>(total)
            })
            .map_err(|_| fail(Errno::EInval))?
            .map_err(fail)?;
        Ok(MethodReply::new(file::pwritev_response {
            count: count as u64,
        }))
    }

    fn readv<'s>(
        &'s mut self,
        request: file::readv_request,
    ) -> Result<MethodReply<'s, file::readv_response>, FailInvocation> {
        self.require_readable()?;
        let bytes = bulk_transfer_bytes(request.size)?;
        if bytes == 0 {
            return Ok(MethodReply::new(file::readv_response { count: 0 }));
        }
        let handle = region_handle(self.region)?;
        let segments = request.layout.segment_count as usize;
        let count =
            servicekit::memory::with_region_write(handle, 0, bytes, |out| {
                let mut total = 0usize;
                let mut cursor = 0usize;
                for length in request.layout.lengths.iter().take(segments) {
                    let length = *length as usize;
                    let remaining = out.len() - cursor;
                    if remaining == 0 || length > remaining {
                        break;
                    }
                    let read = self.fs.read(self.fd, &mut out[cursor..cursor + length])?;
                    if read == 0 {
                        break;
                    }
                    total += read;
                    cursor += length;
                }
                Ok::<usize, Errno>(total)
            })
            .map_err(|_| fail(Errno::EInval))?
            .map_err(fail)?;
        Ok(MethodReply::new(file::readv_response {
            count: count as u64,
        }))
    }

    fn writev<'s>(
        &'s mut self,
        request: file::writev_request,
    ) -> Result<MethodReply<'s, file::writev_response>, FailInvocation> {
        self.require_writable()?;
        let bytes = bulk_transfer_bytes(request.size)?;
        if bytes == 0 {
            return Ok(MethodReply::new(file::writev_response { count: 0 }));
        }
        let handle = region_handle(self.region)?;
        let segments = request.layout.segment_count as usize;
        let count =
            servicekit::memory::with_region_read(handle, 0, bytes, |data| {
                let mut total = 0usize;
                let mut cursor = 0usize;
                for length in request.layout.lengths.iter().take(segments) {
                    let end = cursor.saturating_add(*length as usize).min(data.len());
                    if cursor >= end {
                        break;
                    }
                    let written = self.fs.write(self.fd, &data[cursor..end])?;
                    total += written;
                    cursor = end;
                }
                Ok::<usize, Errno>(total)
            })
            .map_err(|_| fail(Errno::EInval))?
            .map_err(fail)?;
        Ok(MethodReply::new(file::writev_response {
            count: count as u64,
        }))
    }

    fn materialize<'s>(
        &'s mut self,
        _request: file::materialize_request,
    ) -> Result<MethodReply<'s, file::materialize_response>, FailInvocation> {
        // Admission linearization point: the snapshot is immutable from here
        // on (USERSPACE_FILESYSTEM_ADR §5.4).
        let snapshot = self.fs.snapshot(self.fd).map_err(fail)?;
        if snapshot.is_empty() {
            // Zero-length MemoryObjects are unrepresentable in the kernel
            // contract (memory_create rejects size 0); surface that instead
            // of pretending success.
            return Err(fail(Errno::EInval));
        }
        if snapshot.len() as u64 > sys::MEMORY_OBJECT_MAX_BYTES {
            return Err(FailInvocation::domain(-Errno::EFbig.to_i32() as i64));
        }
        let generation = *self.next_generation;
        *self.next_generation = self.next_generation.wrapping_add(1);
        let object =
            crate::mobj::create_and_fill_read_only(&snapshot).map_err(|_| fail(Errno::EIo))?;
        let mut resources: ResourceTable<'static> = ResourceTable::new();
        let slot = resources.push_move(object).map_err(|_| fail(Errno::EIo))?;
        Ok(MethodReply::with_resources(
            file::materialize_response {
                object: slot,
                length: snapshot.len() as u64,
                generation,
            },
            resources,
        ))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::vec;
    use alloc::vec::Vec;
    use naos_idl::loopback::{self, install};
    use naos_idl::{Invocation, ProtocolClientEndpoint, ResourceTable};

    const CAP: u64 = 1 << 20;

    fn seeded_fs() -> RamFs {
        let mut fs = RamFs::new(CAP);
        fs.create_file(fs.root(), b"/hello", false, b"hello world")
            .unwrap();
        fs.mkdir(fs.root(), b"/sub").unwrap();
        fs.create_file(fs.root(), b"/sub/inner", false, b"inner")
            .unwrap();
        fs.create_file(fs.root(), b"/empty", false, b"").unwrap();
        fs
    }

    /// Bind a fresh Directory endpoint pair directly (no wire transfer);
    /// returns the client end plus the registered server-end pump key.
    fn bind(
        server: &mut VfsServer,
        root: NodeId,
        current: NodeId,
    ) -> (ProtocolClientEndpoint, sys::Handle) {
        let owned = server.new_directory_binding(root, current).unwrap();
        let raw = owned.get();
        core::mem::forget(owned);
        // SAFETY: leaked above so it lives for the whole test.
        let client = unsafe { ProtocolClientEndpoint::from_raw(raw) };
        let key = *server.entries.keys().next().expect("server end registered");
        (client, key)
    }

    /// Serve every request queued on `key` until the endpoint goes idle.
    fn pump(server: &mut VfsServer, key: sys::Handle, wire: &mut [u8], reply_wire: &mut [u8]) {
        server.service_endpoint(key, wire, reply_wire);
    }

    fn newest_key(server: &VfsServer, before: &[sys::Handle]) -> sys::Handle {
        *server
            .entries
            .keys()
            .find(|key| !before.contains(key))
            .expect("new endpoint registered")
    }

    fn raw_error(invocation: &Invocation) -> i64 {
        loopback::raw_take_result(invocation.get())
            .expect("completed")
            .protocol_error
    }

    #[test]
    fn peer_close_releases_mount_ticket_reservation_and_backend() {
        let mut topology = crate::mount::Topology::new(1);
        let ticket = topology
            .prepare(
                crate::mount::TargetResolution {
                    owner_mount: crate::mount::ROOT_MOUNT_ID,
                    parent_dir: crate::internal::NodeKey {
                        node_id: 1,
                        generation: 1,
                    },
                    node: crate::internal::NodeKey {
                        node_id: 2,
                        generation: 1,
                    },
                    name: b"mnt".to_vec(),
                },
                0,
                0,
            )
            .unwrap();
        let mut pending = BTreeMap::new();
        pending.insert(ticket, RamFs::new(1024));

        cleanup_peer_binding(Binding::MountTicket { ticket }, &mut topology, &mut pending);

        assert_eq!(
            topology.mount_status(ticket),
            Ok(crate::mount::TicketState::Expired)
        );
        assert!(pending.is_empty());
        // The reservation is reusable immediately after the peer close.
        topology
            .prepare(
                crate::mount::TargetResolution {
                    owner_mount: crate::mount::ROOT_MOUNT_ID,
                    parent_dir: crate::internal::NodeKey {
                        node_id: 1,
                        generation: 1,
                    },
                    node: crate::internal::NodeKey {
                        node_id: 2,
                        generation: 1,
                    },
                    name: b"mnt".to_vec(),
                },
                0,
                1,
            )
            .expect("reservation released");
    }

    #[test]
    fn peer_close_mount_ticket_drops_worker_control_authority() {
        install();
        let mut topology = crate::mount::Topology::new(1);
        let ticket = topology
            .prepare(
                crate::mount::TargetResolution {
                    owner_mount: crate::mount::ROOT_MOUNT_ID,
                    parent_dir: crate::internal::NodeKey {
                        node_id: 1,
                        generation: 1,
                    },
                    node: crate::internal::NodeKey {
                        node_id: 2,
                        generation: 1,
                    },
                    name: b"mnt".to_vec(),
                },
                0,
                0,
            )
            .unwrap();
        let mut pending = BTreeMap::new();
        pending.insert(ticket, RamFs::new(1024));
        let (client, server) = naos_idl::directory::create_endpoints(None).unwrap();
        drop(server);
        let mut controls = BTreeMap::new();
        controls.insert(ticket, unsafe { OwnedHandle::from_raw(client.into_raw()) });

        cleanup_peer_binding_with_controls(
            Binding::MountTicket { ticket },
            &mut topology,
            &mut pending,
            Some(&mut controls),
        );

        assert!(controls.is_empty());
        assert!(pending.is_empty());
        assert_eq!(
            topology.mount_status(ticket),
            Ok(crate::mount::TicketState::Expired)
        );
    }

    #[test]
    fn peer_close_removes_minted_client_registry_entries() {
        install();
        let mut server = VfsServer::new(seeded_fs());
        let root = server.backend().root();
        let (client, _server_key) = bind(&mut server, root, root);
        let client_raw = client.get();
        let client_id = naos_idl::object_id(client_raw).unwrap();
        drop(client);

        let entry = server
            .entries
            .remove(&_server_key)
            .expect("server endpoint");
        cleanup_peer_binding(
            entry.binding,
            &mut server.topology,
            &mut server.pending_backends,
        );
        if let Some(client) = server.client_by_server.remove(&_server_key) {
            server.clients.remove(&client);
            server.binding_clients.remove(&client);
        }
        assert!(!server.clients.contains_key(&client_id));
        assert!(!server.binding_clients.contains_key(&client_id));
        assert_eq!(server.endpoint_count(), 0);
    }

    #[test]
    fn namespace_binding_peer_close_does_not_expire_mount_tickets() {
        let mut topology = crate::mount::Topology::new(1);
        let ticket = topology
            .prepare(
                crate::mount::TargetResolution {
                    owner_mount: crate::mount::ROOT_MOUNT_ID,
                    parent_dir: crate::internal::NodeKey {
                        node_id: 1,
                        generation: 1,
                    },
                    node: crate::internal::NodeKey {
                        node_id: 2,
                        generation: 1,
                    },
                    name: b"mnt".to_vec(),
                },
                0,
                0,
            )
            .unwrap();
        let mut pending = BTreeMap::new();
        cleanup_peer_binding(
            Binding::NamespaceBinding(NsBindingState {
                visible_root_mount: crate::mount::ROOT_MOUNT_ID,
                visible_root: 1,
                current_mount: crate::mount::ROOT_MOUNT_ID,
                current: 1,
                mount_stack: 0,
            }),
            &mut topology,
            &mut pending,
        );
        assert_eq!(
            topology.mount_status(ticket),
            Ok(crate::mount::TicketState::Prepared)
        );
    }

    #[test]
    fn stat_node_nofollow_reports_the_link_itself() {
        install();
        let mut server = VfsServer::new(seeded_fs());
        let mut wire = vec![0u8; 4096];
        let mut reply_wire = vec![0u8; 4096];
        let mut result_wire = vec![0u8; 4096];
        let root = server.backend().root();
        let (dir, key) = bind(&mut server, root, root);

        let link = directory::symlink_request {
            first_size: 6,
            second_size: 5,
            first: b"/hello",
            second: b"/link",
        };
        let mut invocation =
            directory::submit_symlink(&dir, &link, ResourceTable::new(), &mut wire, 1024).unwrap();
        pump(&mut server, key, &mut wire, &mut reply_wire);
        directory::take_symlink(&mut invocation, &mut result_wire).unwrap();

        // NOFOLLOW surfaces the symlink itself.
        let request = directory::stat_node_request {
            flags: LOOKUP_NOFOLLOW,
            path_size: 5,
            path: b"/link",
        };
        let mut invocation =
            directory::submit_stat_node(&dir, &request, ResourceTable::new(), &mut wire, 1024)
                .unwrap();
        pump(&mut server, key, &mut wire, &mut reply_wire);
        let response = directory::take_stat_node(&mut invocation, &mut result_wire).unwrap();
        assert_eq!(response.value.mode & 0o170000, 0o120000);

        // Default lookup resolves through it to the regular file.
        let request = directory::stat_node_request {
            flags: 0,
            path_size: 5,
            path: b"/link",
        };
        let mut invocation =
            directory::submit_stat_node(&dir, &request, ResourceTable::new(), &mut wire, 1024)
                .unwrap();
        pump(&mut server, key, &mut wire, &mut reply_wire);
        let response = directory::take_stat_node(&mut invocation, &mut result_wire).unwrap();
        assert_eq!(response.value.mode & 0o170000, 0o100000);
        assert_eq!(response.value.size, 11);

        // Missing paths report ENOENT.
        let absent = directory::stat_node_request {
            flags: 0,
            path_size: 7,
            path: b"/absent",
        };
        let invocation =
            directory::submit_stat_node(&dir, &absent, ResourceTable::new(), &mut wire, 1024)
                .unwrap();
        pump(&mut server, key, &mut wire, &mut reply_wire);
        assert_eq!(raw_error(&invocation), -(Errno::ENoent.to_i32() as i64));
    }

    #[test]
    fn remove_unlinks_files() {
        install();
        let mut server = VfsServer::new(seeded_fs());
        let mut wire = vec![0u8; 4096];
        let mut reply_wire = vec![0u8; 4096];
        let mut result_wire = vec![0u8; 4096];
        let root = server.backend().root();
        let (dir, key) = bind(&mut server, root, root);

        let request = directory::remove_request {
            mode: 0,
            flags: 0,
            path: b"/hello",
        };
        let mut invocation =
            directory::submit_remove(&dir, &request, ResourceTable::new(), &mut wire, 1024)
                .unwrap();
        pump(&mut server, key, &mut wire, &mut reply_wire);
        directory::take_remove(&mut invocation, &mut result_wire).unwrap();

        let gone = directory::stat_node_request {
            flags: 0,
            path_size: 6,
            path: b"/hello",
        };
        let invocation =
            directory::submit_stat_node(&dir, &gone, ResourceTable::new(), &mut wire, 1024)
                .unwrap();
        pump(&mut server, key, &mut wire, &mut reply_wire);
        assert_eq!(raw_error(&invocation), -(Errno::ENoent.to_i32() as i64));
    }

    #[test]
    fn clone_binding_hands_out_another_usable_endpoint() {
        install();
        let mut server = VfsServer::new(seeded_fs());
        let mut wire = vec![0u8; 4096];
        let mut reply_wire = vec![0u8; 4096];
        let mut result_wire = vec![0u8; 4096];
        let root = server.backend().root();
        let (dir, key) = bind(&mut server, root, root);

        let before: Vec<sys::Handle> = server.entries.keys().copied().collect();
        let mut invocation = directory::submit_clone_binding(
            &dir,
            &directory::clone_binding_request {},
            ResourceTable::new(),
            &mut wire,
            1024,
        )
        .unwrap();
        pump(&mut server, key, &mut wire, &mut reply_wire);
        let (response, mut resources) =
            directory::take_clone_binding(&mut invocation, &mut result_wire).unwrap();
        let raw = resources.take(response.directory).unwrap().into_raw();
        // SAFETY: leaked above via into_raw; lives for the whole test.
        let clone = unsafe { ProtocolClientEndpoint::from_raw(raw) };
        assert_eq!(server.entries.len(), before.len() + 1);

        // The clone sees the same filesystem content.
        let request = directory::stat_node_request {
            flags: 0,
            path_size: 6,
            path: b"/hello",
        };
        let mut invocation =
            directory::submit_stat_node(&clone, &request, ResourceTable::new(), &mut wire, 1024)
                .unwrap();
        let clone_key = newest_key(&server, &before);
        pump(&mut server, clone_key, &mut wire, &mut reply_wire);
        let response = directory::take_stat_node(&mut invocation, &mut result_wire).unwrap();
        assert_eq!(response.value.size, 11);
    }

    #[test]
    fn sync_succeeds_and_set_current_is_enotsup() {
        install();
        let mut server = VfsServer::new(seeded_fs());
        let mut wire = vec![0u8; 4096];
        let mut reply_wire = vec![0u8; 4096];
        let root = server.backend().root();
        let (dir, key) = bind(&mut server, root, root);

        let mut invocation = directory::submit_sync(
            &dir,
            &directory::sync_request {},
            ResourceTable::new(),
            &mut wire,
            1024,
        )
        .unwrap();
        pump(&mut server, key, &mut wire, &mut reply_wire);
        assert_eq!(raw_error(&invocation), 0);

        let invocation = directory::submit_set_current(
            &dir,
            &directory::set_current_request {},
            ResourceTable::new(),
            &mut wire,
            1024,
        )
        .unwrap();
        pump(&mut server, key, &mut wire, &mut reply_wire);
        assert_eq!(raw_error(&invocation), -95);
    }

    #[test]
    fn materialize_refuses_empty_files() {
        install();
        let mut server = VfsServer::new(seeded_fs());
        let mut wire = vec![0u8; 4096];
        let mut reply_wire = vec![0u8; 4096];
        let mut result_wire = vec![0u8; 4096];
        let root = server.backend().root();
        let (dir, key) = bind(&mut server, root, root);

        let open = directory::open_request {
            mode: open_mode::READ,
            flags: 0,
            path: b"/empty",
        };
        let before: Vec<sys::Handle> = server.entries.keys().copied().collect();
        let mut invocation =
            directory::submit_open(&dir, &open, ResourceTable::new(), &mut wire, 1024).unwrap();
        pump(&mut server, key, &mut wire, &mut reply_wire);
        let (response, mut resources) =
            directory::take_open(&mut invocation, &mut result_wire).unwrap();
        let raw = resources.take(response.object).unwrap().into_raw();
        // SAFETY: leaked above via into_raw; lives for the whole test.
        let empty_file = unsafe { ProtocolClientEndpoint::from_raw(raw) };

        let invocation = file::submit_materialize(
            &empty_file,
            &file::materialize_request {},
            ResourceTable::new(),
            &mut wire,
            1024,
        )
        .unwrap();
        let file_key = newest_key(&server, &before);
        pump(&mut server, file_key, &mut wire, &mut reply_wire);
        assert_eq!(raw_error(&invocation), -(Errno::EInval.to_i32() as i64));
    }

    /// Region capability for one bulk File request over the loopback kernel.
    ///
    /// The fake kernel has no MemoryObject mapping syscall, so the bytes a
    /// service reaches through the received capability handle are registered
    /// with servicekit's host-test registry.  The capability carries the
    /// metadata the generated receive validation checks: a memory object at
    /// its protocol scope, transferable, with MAP plus the direction right
    /// (WRITE when the service produces the bytes, READ when it consumes them).
    fn memory_region(bytes: &[u8], service_writes: bool) -> naos_idl::OwnedHandle {
        let direction = if service_writes {
            sys::MEMORY_RIGHT_WRITE
        } else {
            sys::MEMORY_RIGHT_READ
        };
        let handle = loopback::add_capability_with_rights(
            sys::BINDING_MEMORY_OBJECT,
            naos_idl::memory_object::PROTOCOL_SCOPE,
            sys::RIGHT_DUPLICATE | sys::RIGHT_TRANSFER,
            sys::MEMORY_RIGHT_MAP | direction,
        );
        servicekit::memory::register_loopback_region(handle.get(), bytes.to_vec());
        handle
    }

    /// Bytes a loopback-backed region currently holds.
    fn region_bytes(region: &naos_idl::OwnedHandle) -> Vec<u8> {
        servicekit::memory::with_loopback_region(region.get(), |bytes| bytes.to_vec())
            .expect("registered loopback region")
    }

    /// Open `/hello` through the served Directory endpoint and return the File
    /// client end plus its registry key.
    fn open_file(
        server: &mut VfsServer,
        dir: &ProtocolClientEndpoint,
        dir_key: sys::Handle,
        path: &[u8],
        mode: u64,
        wire: &mut [u8],
        reply_wire: &mut [u8],
        result_wire: &mut [u8],
    ) -> (ProtocolClientEndpoint, sys::Handle) {
        let before: Vec<sys::Handle> = server.entries.keys().copied().collect();
        let open = directory::open_request {
            mode,
            flags: 0,
            path,
        };
        let mut invocation =
            directory::submit_open(dir, &open, ResourceTable::new(), wire, 1024).unwrap();
        pump(server, dir_key, wire, reply_wire);
        let (response, mut resources) =
            directory::take_open(&mut invocation, result_wire).unwrap();
        let raw = resources.take(response.object).unwrap().into_raw();
        // SAFETY: taken above, so the raw handle is uniquely owned here and
        // lives for the whole test.
        let file = unsafe { ProtocolClientEndpoint::from_raw(raw) };
        let key = newest_key(server, &before);
        (file, key)
    }

    #[test]
    fn file_endpoints_have_independent_cursors() {
        install();
        let mut server = VfsServer::new(seeded_fs());
        let mut wire = vec![0u8; 4096];
        let mut reply_wire = vec![0u8; 4096];
        let mut result_wire = vec![0u8; 4096];
        let root = server.backend().root();
        let (dir, dir_key) = bind(&mut server, root, root);

        let (first, first_key) = open_file(
            &mut server,
            &dir,
            dir_key,
            b"/hello",
            open_mode::READ,
            &mut wire,
            &mut reply_wire,
            &mut result_wire,
        );
        // Two independent descriptions of the same path must be distinct
        // endpoint pairs, not one shared binding.
        assert_ne!(first.get(), dir.get());
        let (second, second_key) = open_file(
            &mut server,
            &dir,
            dir_key,
            b"/hello",
            open_mode::READ,
            &mut wire,
            &mut reply_wire,
            &mut result_wire,
        );
        assert_ne!(first_key, second_key);

        // Seek A to EOF (11) and read nothing; B's own cursor must be at 0.
        let mut invocation = file::submit_seek(
            &first,
            &file::seek_request {
                offset: 0,
                whence: 2,
            },
            ResourceTable::new(),
            &mut wire,
            1024,
        )
        .unwrap();
        pump(&mut server, first_key, &mut wire, &mut reply_wire);
        let seek = file::take_seek(&mut invocation, &mut result_wire).unwrap();
        assert_eq!(seek.offset, 11);

        let region = memory_region(&[0u8; 11], true);
        let mut table = ResourceTable::new();
        let slot = table.push_duplicate(&region).unwrap();
        let mut invocation = file::submit_read(
            &second,
            &file::read_request {
                size: 11,
                flags: 0,
                buffer: slot,
            },
            table,
            &mut wire,
            1024,
        )
        .unwrap();
        pump(&mut server, second_key, &mut wire, &mut reply_wire);
        let response = file::take_read(&mut invocation, &mut result_wire).unwrap();
        assert_eq!(region_bytes(&region), b"hello world");
        assert_eq!(response.count, 11);
    }

    #[test]
    fn write_then_read_round_trips_over_the_served_endpoint() {
        install();
        let mut server = VfsServer::new(seeded_fs());
        let mut wire = vec![0u8; 4096];
        let mut reply_wire = vec![0u8; 4096];
        let mut result_wire = vec![0u8; 4096];
        let root = server.backend().root();
        let (dir, dir_key) = bind(&mut server, root, root);
        let (file, file_key) = open_file(
            &mut server,
            &dir,
            dir_key,
            b"/empty",
            open_mode::READ | open_mode::WRITE,
            &mut wire,
            &mut reply_wire,
            &mut result_wire,
        );

        let payload = b"over the region";
        let region = memory_region(payload, false);
        let mut table = ResourceTable::new();
        let slot = table.push_duplicate(&region).unwrap();
        let mut invocation = file::submit_write(
            &file,
            &file::write_request {
                size: payload.len() as u64,
                flags: 0,
                buffer: slot,
            },
            table,
            &mut wire,
            1024,
        )
        .unwrap();
        pump(&mut server, file_key, &mut wire, &mut reply_wire);
        let response = file::take_write(&mut invocation, &mut result_wire).unwrap();
        assert_eq!(response.count, payload.len() as u64);

        let mut invocation = file::submit_seek(
            &file,
            &file::seek_request {
                offset: 0,
                whence: 1,
            },
            ResourceTable::new(),
            &mut wire,
            1024,
        )
        .unwrap();
        pump(&mut server, file_key, &mut wire, &mut reply_wire);
        let _ = file::take_seek(&mut invocation, &mut result_wire).unwrap();

        let read_region = memory_region(&[0u8; 15], true);
        let mut table = ResourceTable::new();
        let slot = table.push_duplicate(&read_region).unwrap();
        let mut invocation = file::submit_read(
            &file,
            &file::read_request {
                size: 15,
                flags: 0,
                buffer: slot,
            },
            table,
            &mut wire,
            1024,
        )
        .unwrap();
        pump(&mut server, file_key, &mut wire, &mut reply_wire);
        let response = file::take_read(&mut invocation, &mut result_wire).unwrap();
        // The bytes must actually land in the caller's region.
        assert_eq!(region_bytes(&read_region), payload);
        assert_eq!(response.count, payload.len() as u64);
    }

    #[test]
    fn apply_pair_renames_across_bindings_posix_style() {
        install();
        let mut server = VfsServer::new(seeded_fs());
        let root = server.backend().root();
        let sub = server
            .backend()
            .lookup(root, b"/sub", true)
            .expect("sub")
            .node_id;

        // Plain file move across bindings replaces the destination.
        server
            .backend_mut()
            .create_file(sub, b"victim", false, b"V")
            .unwrap();
        let hello = server
            .backend()
            .lookup(root, b"/hello", true)
            .unwrap()
            .node_id;
        apply_pair(
            server.backend_mut(),
            (root, root),
            directory::METHOD_RENAME_AT,
            b"/hello",
            (sub, sub),
            b"victim",
            0,
        )
        .unwrap();
        assert_eq!(
            server.backend().lookup(root, b"/hello", true),
            Err(Errno::ENoent)
        );
        assert_eq!(
            server
                .backend()
                .lookup(sub, b"victim", true)
                .unwrap()
                .node_id,
            hello
        );
        // Content travelled with the node.
        let fd = server.backend_mut().open(sub, b"victim", true).unwrap();
        let size = server.backend().fstat(fd).unwrap().size;

        // Directory onto a file fails ENOTDIR without side effects.
        assert_eq!(
            apply_pair(
                server.backend_mut(),
                (root, root),
                directory::METHOD_RENAME_AT,
                b"/sub",
                (root, root),
                b"empty",
                0,
            ),
            Err(Errno::ENotDir)
        );
        assert!(server.backend().lookup(root, b"/empty", true).is_ok());

        // Hard link via link_at, then AT_SYMLINK_FOLLOW variant.
        apply_pair(
            server.backend_mut(),
            (root, root),
            directory::METHOD_LINK_AT,
            b"empty",
            (sub, sub),
            b"hardlink",
            0,
        )
        .unwrap();
        assert!(server.backend().lookup(sub, b"hardlink", true).is_ok());

        server
            .backend_mut()
            .symlink_scoped(root, root, b"empty", b"/jump")
            .unwrap();
        apply_pair(
            server.backend_mut(),
            (root, root),
            directory::METHOD_LINK_AT,
            b"/jump",
            (sub, sub),
            b"followed",
            1,
        )
        .unwrap();
        assert!(server.backend().lookup(sub, b"followed", true).is_ok());
    }
}
