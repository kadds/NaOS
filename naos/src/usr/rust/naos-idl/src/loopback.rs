//! In-process fake kernel for host-side round-trip tests.
//!
//! Enabled by the `test-loopback` feature.  [`install`] swaps a table of
//! deterministic, single-threaded syscall implementations over the
//! [`crate::kernel_ops`] seam so generated client bindings and generated
//! server dispatchers can run a full `submit -> receive -> dispatch ->
//! reply -> take_result` cycle on any host without a running NaOS kernel.
//!
//! Semantics modelled after the kernel contract:
//!
//! * `MOVE` dispositions transfer ownership: the source handle dies and the
//!   receiver observes a fresh handle carrying the same capability metadata.
//! * `DUPLICATE` dispositions mint an alias; the source stays valid.
//! * A reply/fail installs its result on the pending invocation; a second
//!   `_na_invocation_take_result` reports `STATUS_ALREADY_CONSUMED`.
//! * Dropping an unanswered responder surfaces `RESPONDER_ABANDONED` to the
//!   waiting client, mirroring kernel behavior for a crashed server.
//! * Receiving without pending data reports `STATUS_WOULD_BLOCK`.
//!
//! State lives in a thread-local, so each test thread gets an isolated fake
//! kernel; [`install`] returns a guard restoring the previous ops table on
use alloc::collections::{BTreeMap, VecDeque};

use alloc::vec::Vec;
use core::cell::RefCell;

use naos_sys as sys;

use crate::OwnedHandle;
use crate::kernel_ops::{self, KernelOps};

/// Fixed caller pid reported to servers; tests only assert it is stable.
pub const CALLER_PID: u64 = 4242;

#[derive(Clone, Copy, Default)]
struct CapabilityMeta {
    binding: u32,
    scope: u64,
    revision: u64,
    features: u64,
    meta_rights: u64,
    protocol_rights: u64,
    object_id: u64,
    view_offset: u64,
    view_length: u64,
}

enum Kind {
    /// One direction of a raw `channel_create` pair.  The queue holds
    /// messages waiting to be received on THIS end.
    ChannelEnd {
        peer: sys::Handle,
        peer_closed: bool,
        queue: VecDeque<Message>,
    },
    Descriptor {
        /// Protocol scope recorded from the descriptor at creation time;
        /// endpoint halves inherit it into their capability metadata just
        /// like the kernel's `kernel_view_metadata` does.
        scope: u64,
    },
    ClientEnd {
        connection: usize,
    },
    ServerEnd {
        connection: usize,
    },
    Responder {
        invocation: sys::Handle,
        method_id: u64,
    },
    Invocation {
        result: Option<ResultRecord>,
        consumed: bool,
    },
    Capability(CapabilityMeta),
}

struct Object {
    kind: Kind,
}

struct Message {
    method_id: u64,
    bytes: Vec<u8>,
    handles: Vec<sys::Handle>,
    responder: Option<sys::Handle>,
}

struct ResultRecord {
    method_id: u64,
    bytes: Vec<u8>,
    handles: Vec<sys::Handle>,
    execution: u32,
    reason: u32,
    protocol_error: i64,
}

#[derive(Default)]
struct Kernel {
    /// Slot `i` is reachable as handle `i + 1` (`HANDLE_INVALID` is 0).
    objects: Vec<Option<Object>>,
    /// Per protocol-endpoint pair: requests awaiting the server end.
    connections: Vec<VecDeque<Message>>,
    /// Capability metadata for minted protocol endpoint halves; consulted
    /// by [`Kernel::meta`] before the kind-derived defaults.
    endpoint_meta: BTreeMap<sys::Handle, CapabilityMeta>,
    next_object_id: u64,
}

impl Kernel {
    fn allocate_object_id(&mut self) -> u64 {
        self.next_object_id = self.next_object_id.saturating_add(1);
        self.next_object_id
    }

    fn mint(&mut self, kind: Kind) -> sys::Handle {
        self.objects.push(Some(Object { kind }));
        self.objects.len() as sys::Handle
    }

    fn object(&self, handle: sys::Handle) -> Option<&Object> {
        if handle == sys::HANDLE_INVALID {
            return None;
        }
        self.objects
            .get(handle as usize - 1)
            .and_then(Option::as_ref)
    }

    fn object_mut(&mut self, handle: sys::Handle) -> Option<&mut Object> {
        if handle == sys::HANDLE_INVALID {
            return None;
        }
        self.objects
            .get_mut(handle as usize - 1)
            .and_then(Option::as_mut)
    }

    /// Capability metadata observable through `_na_handle_get_info`.
    fn meta(&self, handle: sys::Handle) -> Option<CapabilityMeta> {
        if let Some(meta) = self.endpoint_meta.get(&handle) {
            return Some(*meta);
        }
        match &self.object(handle)?.kind {
            Kind::ChannelEnd { .. } => Some(CapabilityMeta {
                binding: sys::BINDING_RAW_CHANNEL_END,
                ..CapabilityMeta::default()
            }),
            Kind::Descriptor { .. } => Some(CapabilityMeta::default()),
            Kind::ClientEnd { .. } => Some(CapabilityMeta {
                binding: sys::BINDING_CLIENT_END,
                ..CapabilityMeta::default()
            }),
            Kind::ServerEnd { .. } => Some(CapabilityMeta {
                binding: sys::BINDING_SERVER_END,
                ..CapabilityMeta::default()
            }),
            Kind::Responder { .. } => Some(CapabilityMeta {
                binding: sys::BINDING_RESPONDER,
                ..CapabilityMeta::default()
            }),
            Kind::Invocation { .. } => Some(CapabilityMeta {
                binding: sys::BINDING_INVOCATION,
                ..CapabilityMeta::default()
            }),
            Kind::Capability(meta) => Some(*meta),
        }
    }

    fn add_capability(
        &mut self,
        binding: u32,
        scope: u64,
        meta_rights: u64,
        protocol_rights: u64,
    ) -> sys::Handle {
        let object_id = self.allocate_object_id();
        self.mint(Kind::Capability(CapabilityMeta {
            binding,
            scope,
            meta_rights,
            protocol_rights,
            object_id,
            ..CapabilityMeta::default()
        }))
    }

    /// Apply a disposition list, returning the receiver-side handles.
    ///
    /// MOVE kills the source handle; DUPLICATE leaves it alive.  Plain
    /// capabilities and protocol endpoint halves are transferable; endpoint
    /// halves keep their kind (and thus their connection) across the move,
    /// mirroring kernel capability transfer.
    fn transfer(
        &mut self,
        dispositions: &[sys::ResourceDisposition],
    ) -> Result<Vec<sys::Handle>, sys::Status> {
        let mut out = Vec::new();
        for disposition in dispositions {
            let moved_kind = match self.object(disposition.handle).map(|object| &object.kind) {
                Some(Kind::Capability(_)) => None,
                Some(Kind::ClientEnd { connection }) => Some(Kind::ClientEnd {
                    connection: *connection,
                }),
                Some(Kind::ServerEnd { connection }) => Some(Kind::ServerEnd {
                    connection: *connection,
                }),
                _ => return Err(sys::STATUS_INVALID_HANDLE),
            };
            let meta = self.meta(disposition.handle).unwrap_or_default();
            let handle = match disposition.operation {
                sys::RESOURCE_MOVE => {
                    self.objects[disposition.handle as usize - 1] = None;
                    let is_endpoint = moved_kind.is_some();
                    let kind = moved_kind.unwrap_or(Kind::Capability(meta));
                    let minted = self.mint(kind);
                    if is_endpoint {
                        // Endpoint metadata is keyed per handle; re-key it.
                        self.endpoint_meta.insert(minted, meta);
                    }
                    minted
                }
                sys::RESOURCE_DUPLICATE => {
                    let is_endpoint = moved_kind.is_some();
                    let kind = moved_kind.unwrap_or(Kind::Capability(meta));
                    let minted = self.mint(kind);
                    if is_endpoint {
                        self.endpoint_meta.insert(minted, meta);
                    }
                    minted
                }
                _ => return Err(sys::STATUS_INVALID_ARGUMENT),
            };
            out.push(handle);
        }
        Ok(out)
    }
}

std::thread_local! {
    static KERNEL: RefCell<Kernel> = RefCell::new(Kernel::default());
}

fn with_kernel<R>(f: impl FnOnce(&mut Kernel) -> R) -> R {
    KERNEL.with(|kernel| f(&mut kernel.borrow_mut()))
}

/// Mint a fake capability handle with the given metadata.
///
/// Tests use this to build client-side resource tables (for example a
/// memory-object buffer handed to a block-device read).
pub fn add_capability(binding: u32, scope: u64, meta_rights: u64) -> OwnedHandle {
    let handle = with_kernel(|kernel| kernel.add_capability(binding, scope, meta_rights, 0));
    unsafe { OwnedHandle::from_raw(handle) }
}

/// Mint a fake capability with independent metadata and protocol rights.
/// Production handles carry these in separate fields; keeping the loopback
/// API explicit prevents tests from accidentally passing protocol bits as
/// disposition/meta rights.
pub fn add_capability_with_rights(
    binding: u32,
    scope: u64,
    meta_rights: u64,
    protocol_rights: u64,
) -> OwnedHandle {
    let handle =
        with_kernel(|kernel| kernel.add_capability(binding, scope, meta_rights, protocol_rights));
    unsafe { OwnedHandle::from_raw(handle) }
}

/// Copy reply payload bytes into a user-provided buffer.
///
/// Returns `(actual, required)` byte counts.
///
/// # Safety
/// `pointer` must be null or valid for `capacity` bytes of writable memory.
unsafe fn fill_bytes(pointer: u64, capacity: u64, data: &[u8]) -> (u64, u64) {
    let required = data.len() as u64;
    if pointer == 0 || capacity == 0 {
        return (0, required);
    }
    let count = core::cmp::min(capacity as usize, data.len());
    unsafe {
        core::ptr::copy_nonoverlapping(data.as_ptr(), pointer as *mut u8, count);
    }
    (count as u64, required)
}

/// Install transferred resource handles into a user-provided array.
///
/// Returns `(actual, required)` handle counts; handles beyond the capacity
/// are closed again so nothing leaks in the fake table.
///
/// # Safety
/// `pointer` must be null or valid for `capacity` handles.
unsafe fn install_handles(pointer: u64, capacity: u64, handles: &[sys::Handle]) -> (u64, u64) {
    let required = handles.len() as u64;
    if pointer == 0 || capacity == 0 {
        return (0, required);
    }
    let room = core::cmp::min(capacity as usize, handles.len());
    unsafe {
        core::ptr::copy_nonoverlapping(handles.as_ptr(), pointer as *mut sys::Handle, room);
    }
    // Handles that did not fit are dropped from the table entirely: the
    // caller never observed them and the fake kernel keeps no orphans.
    for handle in &handles[room..] {
        with_kernel(|kernel| {
            kernel.objects[*handle as usize - 1] = None;
        });
    }
    (room as u64, required)
}

/// Read a request payload out of a submit/send frame.
///
/// # Safety
/// `pointer` must be null or valid for `count` readable bytes.
unsafe fn read_bytes(pointer: u64, count: u64) -> Vec<u8> {
    if pointer == 0 || count == 0 {
        return Vec::new();
    }
    unsafe { core::slice::from_raw_parts(pointer as *const u8, count as usize) }.into()
}

/// # Safety
/// `pointer` must be null or valid for `count` disposition entries.
unsafe fn read_dispositions(pointer: u64, count: u64) -> Vec<sys::ResourceDisposition> {
    if pointer == 0 || count == 0 {
        return Vec::new();
    }
    unsafe {
        core::slice::from_raw_parts(pointer as *const sys::ResourceDisposition, count as usize)
    }
    .into()
}

extern "C" fn fake_handle_close(handle: sys::Handle) -> sys::Status {
    with_kernel(|kernel| {
        if kernel.object(handle).is_none() {
            return sys::STATUS_INVALID_HANDLE;
        }
        match kernel.object(handle).map(|object| &object.kind) {
            Some(Kind::ChannelEnd { peer, .. }) => {
                let peer = *peer;
                if let Some(object) = kernel.object_mut(peer) {
                    if let Kind::ChannelEnd { peer_closed, .. } = &mut object.kind {
                        *peer_closed = true;
                    }
                }
            }
            Some(Kind::Responder { invocation, .. }) => {
                // An unanswered responder dying surfaces RESPONDER_ABANDONED
                // to whoever still waits on the invocation.
                let invocation = *invocation;
                if let Some(object) = kernel.object_mut(invocation) {
                    if let Kind::Invocation { result, consumed } = &mut object.kind {
                        if !*consumed && result.is_none() {
                            *result = Some(ResultRecord {
                                method_id: 0,
                                bytes: Vec::new(),
                                handles: Vec::new(),
                                execution: crate::EXECUTION_NOT_DELIVERED,
                                reason: crate::REASON_RESPONDER_ABANDONED,
                                protocol_error: 0,
                            });
                        }
                    }
                }
            }
            _ => {}
        }
        kernel.objects[handle as usize - 1] = None;
        sys::STATUS_OK
    })
}

extern "C" fn fake_handle_duplicate(
    handle: sys::Handle,
    rights: u64,
    out: *mut sys::Handle,
) -> sys::Status {
    with_kernel(|kernel| {
        let Some(mut meta) = kernel.meta(handle) else {
            return sys::STATUS_INVALID_HANDLE;
        };
        if rights != 0 && meta.meta_rights & rights != rights {
            return sys::STATUS_ACCESS_DENIED;
        }
        if rights != 0 {
            meta.meta_rights &= rights;
        }
        let minted = kernel.mint(Kind::Capability(meta));
        if !out.is_null() {
            unsafe { *out = minted };
        }
        sys::STATUS_OK
    })
}

extern "C" fn fake_handle_restrict(
    handle: sys::Handle,
    restriction: *const sys::HandleRestriction,
    out: *mut sys::Handle,
) -> sys::Status {
    with_kernel(|kernel| {
        let Some(mut meta) = kernel.meta(handle) else {
            return sys::STATUS_INVALID_HANDLE;
        };
        if restriction.is_null() {
            return sys::STATUS_INVALID_ARGUMENT;
        }
        let restriction = unsafe { &*restriction };
        if restriction.scope != 0 {
            meta.scope = restriction.scope;
        }
        if restriction.revision != 0 {
            meta.revision = restriction.revision;
        }
        if restriction.features != 0 {
            meta.features = restriction.features;
        }
        if restriction.meta_rights != 0 {
            meta.meta_rights &= restriction.meta_rights;
        }
        if restriction.protocol_rights != 0 {
            meta.protocol_rights &= restriction.protocol_rights;
        }
        if restriction.flags & sys::RESTRICTION_RANGE != 0 {
            if meta.binding != sys::BINDING_MEMORY_OBJECT
                || meta.scope != crate::memory_object::PROTOCOL_SCOPE
                || restriction.view_length == 0
                || restriction.view_offset > meta.view_length
                || restriction.view_length > meta.view_length - restriction.view_offset
            {
                return sys::STATUS_INVALID_ARGUMENT;
            }
            let Some(view_offset) = meta.view_offset.checked_add(restriction.view_offset) else {
                return sys::STATUS_INVALID_ARGUMENT;
            };
            meta.view_offset = view_offset;
            meta.view_length = restriction.view_length;
        }
        let minted = kernel.mint(Kind::Capability(meta));
        if !out.is_null() {
            unsafe { *out = minted };
        }
        sys::STATUS_OK
    })
}

extern "C" fn fake_handle_get_info(handle: sys::Handle, info: *mut sys::HandleInfo) -> sys::Status {
    with_kernel(|kernel| {
        let Some(meta) = kernel.meta(handle) else {
            return sys::STATUS_INVALID_HANDLE;
        };
        if info.is_null() {
            return sys::STATUS_INVALID_ARGUMENT;
        }
        let info = unsafe { &mut *info };
        info.binding = meta.binding;
        info.scope = meta.scope;
        info.revision = meta.revision;
        info.features = meta.features;
        info.meta_rights = meta.meta_rights;
        info.protocol_rights = meta.protocol_rights;
        info.object_id = meta.object_id;
        info.view_offset = meta.view_offset;
        info.view_length = meta.view_length;
        sys::STATUS_OK
    })
}

extern "C" fn fake_channel_create(
    _options: *const sys::ChannelOptions,
    left: *mut sys::Handle,
    right: *mut sys::Handle,
) -> sys::Status {
    with_kernel(|kernel| {
        let first = kernel.mint(Kind::ChannelEnd {
            peer: 0,
            peer_closed: false,
            queue: VecDeque::new(),
        });
        let second = kernel.mint(Kind::ChannelEnd {
            peer: first,
            peer_closed: false,
            queue: VecDeque::new(),
        });
        if let Some(object) = kernel.object_mut(first) {
            if let Kind::ChannelEnd { peer, .. } = &mut object.kind {
                *peer = second;
            }
        }
        unsafe {
            if !left.is_null() {
                *left = first;
            }
            if !right.is_null() {
                *right = second;
            }
        }
        sys::STATUS_OK
    })
}

extern "C" fn fake_channel_send(
    endpoint: sys::Handle,
    frame: *const sys::ChannelSendFrame,
) -> sys::Status {
    with_kernel(|kernel| {
        let peer = match kernel.object(endpoint).map(|object| &object.kind) {
            Some(Kind::ChannelEnd { peer, .. }) => *peer,
            _ => return sys::STATUS_INVALID_HANDLE,
        };
        if frame.is_null() {
            return sys::STATUS_INVALID_ARGUMENT;
        }
        let frame = unsafe { &*frame };
        // SAFETY: the frame's pointers describe the caller's own buffers.
        let dispositions = unsafe { read_dispositions(frame.resources, frame.resource_count) };
        let Ok(handles) = kernel.transfer(&dispositions) else {
            return sys::STATUS_INVALID_HANDLE;
        };
        let message = Message {
            method_id: 0,
            bytes: unsafe { read_bytes(frame.bytes, frame.byte_count) },
            handles,
            responder: None,
        };
        if let Some(object) = kernel.object_mut(peer) {
            if let Kind::ChannelEnd { queue, .. } = &mut object.kind {
                queue.push_back(message);
            }
        }
        sys::STATUS_OK
    })
}

fn channel_is_closed(kernel: &Kernel, endpoint: sys::Handle) -> bool {
    matches!(
        kernel.object(endpoint).map(|object| &object.kind),
        Some(Kind::ChannelEnd {
            peer_closed: true,
            ..
        })
    )
}

fn take_message(
    kernel: &mut Kernel,
    endpoint: sys::Handle,
    is_server_end: bool,
) -> Option<Message> {
    match kernel.object(endpoint)?.kind {
        Kind::ChannelEnd { .. } => {
            if let Some(object) = kernel.object_mut(endpoint) {
                if let Kind::ChannelEnd { queue, .. } = &mut object.kind {
                    return queue.pop_front();
                }
            }
            None
        }
        Kind::ServerEnd { connection } if is_server_end => kernel
            .connections
            .get_mut(connection)
            .and_then(VecDeque::pop_front),
        _ => None,
    }
}

extern "C" fn fake_channel_receive(
    endpoint: sys::Handle,
    frame: *mut sys::ChannelReceiveFrame,
) -> sys::Status {
    with_kernel(|kernel| {
        let is_server_end = matches!(
            kernel.object(endpoint).map(|object| &object.kind),
            Some(Kind::ServerEnd { .. })
        );
        if !is_server_end
            && !matches!(
                kernel.object(endpoint).map(|object| &object.kind),
                Some(Kind::ChannelEnd { .. })
            )
        {
            return sys::STATUS_INVALID_HANDLE;
        }
        let Some(message) = take_message(kernel, endpoint, is_server_end) else {
            return if !is_server_end && channel_is_closed(kernel, endpoint) {
                sys::STATUS_PEER_CLOSED
            } else {
                sys::STATUS_WOULD_BLOCK
            };
        };
        let frame = unsafe { &mut *frame };
        // SAFETY: the frame's buffers belong to the receiver.
        let (actual_bytes, required_bytes) =
            unsafe { fill_bytes(frame.bytes, frame.byte_capacity, &message.bytes) };
        let (actual_resources, required_resources) =
            unsafe { install_handles(frame.resources, frame.resource_capacity, &message.handles) };
        frame.method_id = message.method_id;
        frame.caller_pid = CALLER_PID;
        frame.responder = message.responder.unwrap_or(sys::HANDLE_INVALID);
        frame.actual_bytes = actual_bytes;
        frame.required_bytes = required_bytes;
        frame.actual_resources = actual_resources;
        frame.required_resources = required_resources;
        sys::STATUS_OK
    })
}
extern "C" fn fake_channel_discard(endpoint: sys::Handle) -> sys::Status {
    with_kernel(|kernel| {
        let is_server_end = matches!(
            kernel.object(endpoint).map(|object| &object.kind),
            Some(Kind::ServerEnd { .. })
        );
        match take_message(kernel, endpoint, is_server_end) {
            Some(_) => sys::STATUS_OK,
            None => sys::STATUS_WOULD_BLOCK,
        }
    })
}

extern "C" fn fake_protocol_descriptor_create(
    descriptor: *const sys::ProtocolDescriptor,
    out: *mut sys::Handle,
) -> sys::Status {
    with_kernel(|kernel| {
        if descriptor.is_null() || out.is_null() {
            return sys::STATUS_INVALID_ARGUMENT;
        }
        // Only the scope matters for endpoint creation below; it is
        // inherited by both endpoint halves' capability metadata.
        let scope = unsafe { (&*descriptor).scope };
        let minted = kernel.mint(Kind::Descriptor { scope });
        unsafe { *out = minted };
        sys::STATUS_OK
    })
}

extern "C" fn fake_protocol_endpoint_create(
    descriptor: sys::Handle,
    _options: *const sys::ProtocolEndpointOptions,
    client_out: *mut sys::Handle,
    server_out: *mut sys::Handle,
) -> sys::Status {
    with_kernel(|kernel| {
        let scope = match kernel.object(descriptor).map(|object| &object.kind) {
            Some(Kind::Descriptor { scope }) => *scope,
            _ => return sys::STATUS_WRONG_BINDING,
        };
        let connection = kernel.connections.len();
        kernel.connections.push(VecDeque::new());
        let client = kernel.mint(Kind::ClientEnd { connection });
        let server = kernel.mint(Kind::ServerEnd { connection });
        let client_object_id = kernel.allocate_object_id();
        let server_object_id = kernel.allocate_object_id();
        // Mirror `kernel_view_metadata`: both halves carry the protocol
        // scope plus the standard meta rights, the client keeps INVOKE.
        for (handle, binding) in [
            (client, sys::BINDING_CLIENT_END),
            (server, sys::BINDING_SERVER_END),
        ] {
            kernel.endpoint_meta.insert(
                handle,
                CapabilityMeta {
                    binding,
                    scope,
                    meta_rights: sys::RIGHT_DUPLICATE
                        | sys::RIGHT_TRANSFER
                        | sys::RIGHT_WAIT
                        | sys::RIGHT_INSPECT,
                    object_id: if handle == client {
                        client_object_id
                    } else {
                        server_object_id
                    },
                    ..CapabilityMeta::default()
                },
            );
        }
        unsafe {
            if !client_out.is_null() {
                *client_out = client;
            }
            if !server_out.is_null() {
                *server_out = server;
            }
        }
        sys::STATUS_OK
    })
}

extern "C" fn fake_invoke_submit(
    target: sys::Handle,
    frame: *const sys::SubmitFrame,
    invocation_out: *mut sys::Handle,
) -> sys::Status {
    with_kernel(|kernel| {
        let connection = match kernel.object(target).map(|object| &object.kind) {
            Some(Kind::ClientEnd { connection }) => *connection,
            _ => return sys::STATUS_WRONG_BINDING,
        };
        if frame.is_null() {
            return sys::STATUS_INVALID_ARGUMENT;
        }
        let frame = unsafe { &*frame };
        // SAFETY: the frame's pointers describe the caller's own buffers.
        let dispositions = unsafe { read_dispositions(frame.resources, frame.resource_count) };
        let Ok(handles) = kernel.transfer(&dispositions) else {
            return sys::STATUS_INVALID_HANDLE;
        };
        let invocation = kernel.mint(Kind::Invocation {
            result: None,
            consumed: false,
        });
        let responder = kernel.mint(Kind::Responder {
            invocation,
            method_id: frame.method_id,
        });
        let message = Message {
            method_id: frame.method_id,
            bytes: unsafe { read_bytes(frame.request, frame.request_bytes) },
            handles,
            responder: Some(responder),
        };
        if let Some(queue) = kernel.connections.get_mut(connection) {
            queue.push_back(message);
        }
        if !invocation_out.is_null() {
            unsafe { *invocation_out = invocation };
        }
        sys::STATUS_OK
    })
}

extern "C" fn fake_invoke_oneway(
    target: sys::Handle,
    frame: *const sys::SubmitFrame,
) -> sys::Status {
    with_kernel(|kernel| {
        let connection = match kernel.object(target).map(|object| &object.kind) {
            Some(Kind::ClientEnd { connection }) => *connection,
            _ => return sys::STATUS_WRONG_BINDING,
        };
        if frame.is_null() {
            return sys::STATUS_INVALID_ARGUMENT;
        }
        let frame = unsafe { &*frame };
        // SAFETY: the frame's pointers describe the caller's own buffers.
        let dispositions = unsafe { read_dispositions(frame.resources, frame.resource_count) };
        let Ok(handles) = kernel.transfer(&dispositions) else {
            return sys::STATUS_INVALID_HANDLE;
        };
        let message = Message {
            method_id: frame.method_id,
            bytes: unsafe { read_bytes(frame.request, frame.request_bytes) },
            handles,
            responder: None,
        };
        if let Some(queue) = kernel.connections.get_mut(connection) {
            queue.push_back(message);
        }
        sys::STATUS_OK
    })
}

extern "C" fn fake_invocation_cancel(invocation: sys::Handle) -> sys::Status {
    with_kernel(|kernel| {
        let Some(object) = kernel.object_mut(invocation) else {
            return sys::STATUS_INVALID_HANDLE;
        };
        match &mut object.kind {
            Kind::Invocation { result, consumed } => {
                if *consumed {
                    return sys::STATUS_ALREADY_CONSUMED;
                }
                if result.is_none() {
                    *result = Some(ResultRecord {
                        method_id: 0,
                        bytes: Vec::new(),
                        handles: Vec::new(),
                        execution: crate::EXECUTION_NOT_DELIVERED,
                        reason: crate::REASON_CANCEL_REQUESTED,
                        protocol_error: 0,
                    });
                }
                sys::STATUS_OK
            }
            _ => sys::STATUS_WRONG_BINDING,
        }
    })
}

extern "C" fn fake_invocation_take_result(
    invocation: sys::Handle,
    frame: *mut sys::ResultFrame,
) -> sys::Status {
    with_kernel(|kernel| {
        let record = {
            let Some(object) = kernel.object_mut(invocation) else {
                return sys::STATUS_INVALID_HANDLE;
            };
            let Kind::Invocation { result, consumed } = &mut object.kind else {
                return sys::STATUS_WRONG_BINDING;
            };
            if *consumed {
                return sys::STATUS_ALREADY_CONSUMED;
            }
            match result.take() {
                Some(record) => {
                    *consumed = true;
                    record
                }
                None => return sys::STATUS_WOULD_BLOCK,
            }
        };
        if frame.is_null() {
            return sys::STATUS_OK;
        }
        let frame = unsafe { &mut *frame };
        // SAFETY: the frame's buffers belong to the receiver.
        let (actual_bytes, required_bytes) =
            unsafe { fill_bytes(frame.bytes, frame.byte_capacity, &record.bytes) };
        let (actual_resources, required_resources) =
            unsafe { install_handles(frame.resources, frame.resource_capacity, &record.handles) };
        frame.method_id = record.method_id;
        frame.actual_bytes = actual_bytes;
        frame.required_bytes = required_bytes;
        frame.actual_resources = actual_resources;
        frame.required_resources = required_resources;
        frame.execution_outcome = record.execution;
        frame.outcome_reason = record.reason;
        frame.protocol_error = record.protocol_error;
        sys::STATUS_OK
    })
}

fn deliver_result(
    kernel: &mut Kernel,
    responder: sys::Handle,
    mut record: ResultRecord,
) -> sys::Status {
    let (invocation, method_id) = match kernel.object(responder).map(|object| &object.kind) {
        Some(Kind::Responder {
            invocation,
            method_id,
        }) => (*invocation, *method_id),
        _ => return sys::STATUS_INVALID_HANDLE,
    };
    record.method_id = method_id;
    let Some(object) = kernel.object_mut(invocation) else {
        return sys::STATUS_INVALID_HANDLE;
    };
    let Kind::Invocation { result, consumed } = &mut object.kind else {
        return sys::STATUS_WRONG_BINDING;
    };
    if *consumed || result.is_some() {
        return sys::STATUS_ALREADY_CONSUMED;
    }
    *result = Some(record);
    // A successful delivery consumes the responder capability.
    kernel.objects[responder as usize - 1] = None;
    sys::STATUS_OK
}

extern "C" fn fake_responder_reply(
    responder: sys::Handle,
    frame: *const sys::ReplyFrame,
) -> sys::Status {
    with_kernel(|kernel| {
        if frame.is_null() {
            return sys::STATUS_INVALID_ARGUMENT;
        }
        let frame = unsafe { &*frame };
        // SAFETY: the frame's pointers describe the responder's own buffers.
        let dispositions = unsafe { read_dispositions(frame.resources, frame.resource_count) };
        let Ok(handles) = kernel.transfer(&dispositions) else {
            return sys::STATUS_INVALID_HANDLE;
        };
        let record = ResultRecord {
            method_id: 0,
            bytes: unsafe { read_bytes(frame.bytes, frame.byte_count) },
            handles,
            execution: crate::EXECUTION_NONE,
            reason: crate::REASON_NONE,
            protocol_error: 0,
        };
        deliver_result(kernel, responder, record)
    })
}

extern "C" fn fake_responder_fail(
    responder: sys::Handle,
    frame: *const sys::FailFrame,
) -> sys::Status {
    with_kernel(|kernel| {
        if frame.is_null() {
            return sys::STATUS_INVALID_ARGUMENT;
        }
        let frame = unsafe { &*frame };
        let record = ResultRecord {
            method_id: 0,
            bytes: Vec::new(),
            handles: Vec::new(),
            execution: frame.execution_outcome,
            reason: frame.outcome_reason,
            protocol_error: frame.protocol_error,
        };
        deliver_result(kernel, responder, record)
    })
}

static FAKE_OPS: KernelOps = KernelOps {
    handle_close: fake_handle_close,
    handle_duplicate: fake_handle_duplicate,
    handle_restrict: fake_handle_restrict,
    handle_get_info: fake_handle_get_info,
    channel_create: fake_channel_create,
    channel_send: fake_channel_send,
    channel_receive: fake_channel_receive,
    channel_discard: fake_channel_discard,
    protocol_descriptor_create: fake_protocol_descriptor_create,
    protocol_endpoint_create: fake_protocol_endpoint_create,
    invoke_submit: fake_invoke_submit,
    invoke_oneway: fake_invoke_oneway,
    invocation_cancel: fake_invocation_cancel,
    invocation_take_result: fake_invocation_take_result,
    responder_reply: fake_responder_reply,
    responder_fail: fake_responder_fail,
};

/// Install the fake kernel over the syscall seam.
///
/// Installation is permanent for the process: restoring the real table in a
/// guard would race with sibling test threads that are still making
/// syscalls through the seam.  Per-test isolation comes from the
/// thread-local [`KERNEL`] state instead, so every test thread observes an
/// independent fake kernel while all of them share the same function table.
pub fn install() {
    let table: &'static KernelOps = &FAKE_OPS;
    kernel_ops::install(table as *const KernelOps as *mut KernelOps);
}

/// Submit a raw request payload without generated encoding.
///
/// Tests use this to drive malformed or out-of-protocol requests through the
/// same submit path the kernel exposes.
pub fn raw_invoke_submit(
    target: sys::Handle,
    method_id: u64,
    payload: &[u8],
) -> Result<crate::Invocation, sys::Status> {
    raw_invoke_submit_with_resources(target, method_id, payload, &[])
}

/// Submit a raw request with an explicit disposition list.  This is useful
/// for testing transfer semantics independently of a generated method's
/// resource contract.
pub fn raw_invoke_submit_with_resources(
    target: sys::Handle,
    method_id: u64,
    payload: &[u8],
    dispositions: &[sys::ResourceDisposition],
) -> Result<crate::Invocation, sys::Status> {
    let frame = sys::SubmitFrame {
        struct_size: core::mem::size_of::<sys::SubmitFrame>() as u32,
        method_id,
        request: payload.as_ptr() as u64,
        request_bytes: payload.len() as u64,
        resources: dispositions.as_ptr() as u64,
        resource_count: dispositions.len() as u64,
        ..sys::SubmitFrame::default()
    };
    let mut handle = sys::HANDLE_INVALID;
    let status = fake_invoke_submit(target, &frame, &mut handle);
    if status == sys::STATUS_OK && handle != sys::HANDLE_INVALID {
        Ok(unsafe { crate::Invocation::from_raw(handle) })
    } else {
        Err(status)
    }
}

/// Outcome of [`raw_take_result`].
pub struct RawResult {
    pub method_id: u64,
    pub actual_bytes: u64,
    pub actual_resources: u64,
    pub execution: u32,
    pub reason: u32,
    pub protocol_error: i64,
}

/// Take a result without any protocol typing (tests).
///
/// Unlike the generated `take_{method}` functions this does not check the
/// method id first, so invocations submitted with out-of-protocol ids can
/// still observe their typed failure.
pub fn raw_take_result(invocation: sys::Handle) -> Result<RawResult, sys::Status> {
    let mut frame = sys::ResultFrame {
        struct_size: core::mem::size_of::<sys::ResultFrame>() as u32,
        ..sys::ResultFrame::default()
    };
    let status = fake_invocation_take_result(invocation, &mut frame);
    if status != sys::STATUS_OK {
        return Err(status);
    }
    Ok(RawResult {
        method_id: frame.method_id,
        actual_bytes: frame.actual_bytes,
        actual_resources: frame.actual_resources,
        execution: frame.execution_outcome,
        reason: frame.outcome_reason,
        protocol_error: frame.protocol_error,
    })
}

/// Take a raw result while retaining its payload and transferred handles.
/// This is deliberately a test-only companion to [`raw_take_result`], used by
/// private protocols that do not have generated public bindings.
pub fn raw_take_result_into(
    invocation: sys::Handle,
    bytes: &mut [u8],
    handles: &mut [sys::Handle],
) -> Result<RawResult, sys::Status> {
    let mut frame = sys::ResultFrame {
        struct_size: core::mem::size_of::<sys::ResultFrame>() as u32,
        bytes: bytes.as_mut_ptr() as u64,
        byte_capacity: bytes.len() as u64,
        resources: handles.as_mut_ptr() as u64,
        resource_capacity: handles.len() as u64,
        ..sys::ResultFrame::default()
    };
    let status = fake_invocation_take_result(invocation, &mut frame);
    if status != sys::STATUS_OK {
        return Err(status);
    }
    Ok(RawResult {
        method_id: frame.method_id,
        actual_bytes: frame.actual_bytes,
        actual_resources: frame.actual_resources,
        execution: frame.execution_outcome,
        reason: frame.outcome_reason,
        protocol_error: frame.protocol_error,
    })
}
