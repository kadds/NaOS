//! Wire codecs for the private VFS control protocols
//! (doc/VFS_BLOCK_DEVICE_ADR.md §6.4).
//!
//! `NamespaceBinding`, `MountControl` and `MutationTicket` schemas live in
//! `idl/internal/`, which the naos-idl Rust build deliberately does not
//! discover (contracts.md: internal IDL is worker-toolchain-only, C++ only).
//! The generated Rust bindings are therefore unavailable to this crate; this
//! module reproduces exactly the encoding the generator emits (verified
//! against `naoidl.py generate-rust` output): little-endian fixed header
//! fields in `@id` order via [`Encoder`]/[`Decoder`], dynamic `bytes<N>`
//! tails addressed by their `u64` length fields.
//!
//! Endpoint creation mirrors the generated `create_endpoints`: a protocol
//! descriptor handle followed by `_na_protocol_endpoint_create`.

use alloc::vec::Vec;
use naos_idl::{
    CallError, CodecError, Decoder, Encoder, Invocation, MAX_RESOURCES, ProtocolClientEndpoint,
    ProtocolServerEndpoint, RawHandleGuard, ReceivedResources, ResourceSlot, ResourceTable,
};
use naos_sys as sys;

/// Worker-local, generation-tagged node identity (idl/internal/fragments).
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq, PartialOrd, Ord)]
pub struct NodeKey {
    pub node_id: u64,
    pub generation: u64,
}

impl NodeKey {
    pub const ZERO: NodeKey = NodeKey {
        node_id: 0,
        generation: 0,
    };

    pub fn is_zero(self) -> bool {
        self.node_id == 0 && self.generation == 0
    }

    pub fn encode(&self, encoder: &mut Encoder<'_>) -> Result<(), CodecError> {
        encoder.put_u64(self.node_id)?;
        encoder.put_u64(self.generation)?;
        Ok(())
    }

    pub fn decode(decoder: &mut Decoder<'_>) -> Result<Self, CodecError> {
        let node_id = decoder.get_u64()?;
        let generation = decoder.get_u64()?;
        Ok(NodeKey {
            node_id,
            generation,
        })
    }
}

/// Global walk budget passed by value across every hop (§6.4).
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct WalkContext {
    pub open_flags: u64,
    pub remaining_symlinks: u32,
    pub reserved: u32,
}

impl WalkContext {
    pub fn encode(&self, encoder: &mut Encoder<'_>) -> Result<(), CodecError> {
        encoder.put_u64(self.open_flags)?;
        encoder.put_u32(self.remaining_symlinks)?;
        encoder.put_u32(self.reserved)?;
        Ok(())
    }

    pub fn decode(decoder: &mut Decoder<'_>) -> Result<Self, CodecError> {
        let open_flags = decoder.get_u64()?;
        let remaining_symlinks = decoder.get_u32()?;
        let reserved = decoder.get_u32()?;
        Ok(WalkContext {
            open_flags,
            remaining_symlinks,
            reserved,
        })
    }
}

fn encode_message(
    wire: &mut [u8],
    header_bytes: usize,
    dynamic: usize,
    fill: impl FnOnce(&mut Encoder<'_>) -> Result<(), CodecError>,
) -> Result<usize, CodecError> {
    let required = header_bytes
        .checked_add(dynamic)
        .ok_or(CodecError::Overflow)?;
    if required > wire.len() {
        return Err(CodecError::Overflow);
    }
    let mut encoder = Encoder::new(wire);
    fill(&mut encoder)?;
    debug_assert_eq!(encoder.written(), required);
    Ok(encoder.written())
}

/// Decode the dynamic `(length-field, bytes)` tail after a fixed header.
///
/// Mirrors the generated cursor logic: bound check per segment
/// (`BoundExceeded`), truncation (`Truncated`) and trailing-byte rejection
/// (`InvalidMessage`). The caller has already decoded and bounds-checked the
/// fixed header against `header_bytes`.
fn decode_segments<'a>(
    wire: &'a [u8],
    header_bytes: usize,
    lengths: &[u64],
    bounds: &[usize],
) -> Result<Vec<&'a [u8]>, CodecError> {
    let mut out = Vec::with_capacity(lengths.len());
    let mut cursor = header_bytes;
    for (raw_len, bound) in lengths.iter().zip(bounds.iter()) {
        let count = *raw_len as usize;
        if count > *bound {
            return Err(CodecError::BoundExceeded);
        }
        let end = cursor.checked_add(count).ok_or(CodecError::Overflow)?;
        if end > wire.len() {
            return Err(CodecError::Truncated);
        }
        out.push(&wire[cursor..end]);
        cursor = end;
    }
    if cursor != wire.len() {
        return Err(CodecError::InvalidMessage);
    }
    Ok(out)
}

fn require_exact(wire: &[u8], consumed: usize) -> Result<(), CodecError> {
    if wire.len() < consumed {
        Err(CodecError::Truncated)
    } else if consumed != wire.len() {
        Err(CodecError::InvalidMessage)
    } else {
        Ok(())
    }
}

// ---------------------------------------------------------------------------
// NamespaceBinding (scope 18)
// ---------------------------------------------------------------------------

pub mod namespace_binding {
    use super::*;

    pub const PROTOCOL_UUID: [u8; 16] = [
        43, 158, 60, 46, 140, 125, 79, 177, 158, 33, 76, 75, 14, 10, 16, 35,
    ];
    pub const PROTOCOL_SCOPE: u64 = 18;
    /// `namespace_route` (bit 21); trailing bit matches the generator's
    /// descriptor convention observed in every generated module.
    pub const PROTOCOL_RIGHTS: u64 = (1 << 21) | 1;

    pub const METHOD_RESOLVE_ABSOLUTE: u64 = 1;
    pub const METHOD_ROUTE_ABOVE: u64 = 2;
    pub const METHOD_ENTER_CHILD_MOUNT: u64 = 3;
    pub const METHOD_DERIVE_CHROOT: u64 = 4;
    pub const METHOD_BEGIN_MUTATION: u64 = 5;

    /// Frozen operation values for `begin_mutation` (ADR §6.2).
    pub const OP_RENAME: u32 = 0;
    pub const OP_LINK: u32 = 1;
    pub const OP_SYMLINK: u32 = 2;
    pub const OP_CREATE: u32 = 3;
    pub const OP_MKDIR: u32 = 4;
    pub const OP_UNLINK: u32 = 5;
    pub const OP_RMDIR: u32 = 6;

    pub const RESOLVE_ABSOLUTE_HEADER: usize = 24; // walk(16) + path_size(8)
    pub const ENTER_CHILD_MOUNT_HEADER: usize = 16;
    pub const DERIVE_CHROOT_HEADER: usize = 16;
    pub const BEGIN_MUTATION_HEADER: usize = 84;

    #[derive(Clone, Copy, Debug)]
    pub struct resolve_absolute_request<'a> {
        pub walk: WalkContext,
        pub path: &'a [u8],
    }

    pub fn encode_resolve_absolute_request(
        value: &resolve_absolute_request<'_>,
        wire: &mut [u8],
    ) -> Result<usize, CodecError> {
        encode_message(wire, RESOLVE_ABSOLUTE_HEADER, value.path.len(), |enc| {
            value.walk.encode(enc)?;
            enc.put_u64(value.path.len() as u64)?;
            enc.put_bounded_bytes(value.path, 4095)?;
            Ok(())
        })
    }

    pub fn decode_resolve_absolute_request<'a>(
        wire: &'a [u8],
    ) -> Result<resolve_absolute_request<'a>, CodecError> {
        if wire.len() < RESOLVE_ABSOLUTE_HEADER {
            return Err(CodecError::Truncated);
        }
        let mut dec = Decoder::new(wire);
        let walk = WalkContext::decode(&mut dec)?;
        let path_size = dec.get_u64()?;
        if dec.offset() != RESOLVE_ABSOLUTE_HEADER {
            return Err(CodecError::InvalidMessage);
        }
        let mut segments = decode_segments(wire, RESOLVE_ABSOLUTE_HEADER, &[path_size], &[4095])?;
        let path = segments.pop().ok_or(CodecError::InvalidMessage)?;
        Ok(resolve_absolute_request { walk, path })
    }

    /// Single-slot response body (`handle`/`client_end` field encoded as
    /// one u32 resource index).
    pub fn encode_slot_response(slot: ResourceSlot, wire: &mut [u8]) -> Result<usize, CodecError> {
        encode_message(wire, 4, 0, |enc| enc.put_u32(slot.index()))
    }

    pub fn decode_slot_response(wire: &[u8]) -> Result<ResourceSlot, CodecError> {
        if wire.len() < 4 {
            return Err(CodecError::Truncated);
        }
        let mut dec = Decoder::new(wire);
        let raw = dec.get_u32()?;
        ResourceSlot::new(raw).ok_or(CodecError::InvalidResource)
    }

    pub fn encode_enter_child_mount_request(
        mountpoint: NodeKey,
        wire: &mut [u8],
    ) -> Result<usize, CodecError> {
        encode_message(wire, ENTER_CHILD_MOUNT_HEADER, 0, |enc| {
            mountpoint.encode(enc)
        })
    }

    pub fn decode_enter_child_mount_request(wire: &[u8]) -> Result<NodeKey, CodecError> {
        if wire.len() < ENTER_CHILD_MOUNT_HEADER {
            return Err(CodecError::Truncated);
        }
        let mut dec = Decoder::new(wire);
        let mountpoint = NodeKey::decode(&mut dec)?;
        require_exact(wire, ENTER_CHILD_MOUNT_HEADER)?;
        Ok(mountpoint)
    }

    pub fn encode_derive_chroot_request(
        subtree_root: NodeKey,
        wire: &mut [u8],
    ) -> Result<usize, CodecError> {
        encode_message(wire, DERIVE_CHROOT_HEADER, 0, |enc| {
            subtree_root.encode(enc)
        })
    }

    pub fn decode_derive_chroot_request(wire: &[u8]) -> Result<NodeKey, CodecError> {
        if wire.len() < DERIVE_CHROOT_HEADER {
            return Err(CodecError::Truncated);
        }
        let mut dec = Decoder::new(wire);
        let subtree_root = NodeKey::decode(&mut dec)?;
        require_exact(wire, DERIVE_CHROOT_HEADER)?;
        Ok(subtree_root)
    }

    #[derive(Clone, Copy, Debug)]
    pub struct begin_mutation_request<'a> {
        pub operation: u32,
        pub old_parent: NodeKey,
        pub old_target: NodeKey,
        pub new_parent: NodeKey,
        pub new_target: NodeKey,
        pub old_name: &'a [u8],
        pub new_name: &'a [u8],
    }

    pub fn encode_begin_mutation_request(
        value: &begin_mutation_request<'_>,
        wire: &mut [u8],
    ) -> Result<usize, CodecError> {
        let dynamic = value.old_name.len().saturating_add(value.new_name.len());
        encode_message(wire, BEGIN_MUTATION_HEADER, dynamic, |enc| {
            enc.put_u32(value.operation)?;
            value.old_parent.encode(enc)?;
            value.old_target.encode(enc)?;
            value.new_parent.encode(enc)?;
            value.new_target.encode(enc)?;
            enc.put_u64(value.old_name.len() as u64)?;
            enc.put_u64(value.new_name.len() as u64)?;
            enc.put_bounded_bytes(value.old_name, 255)?;
            enc.put_bounded_bytes(value.new_name, 255)?;
            Ok(())
        })
    }

    pub fn decode_begin_mutation_request<'a>(
        wire: &'a [u8],
    ) -> Result<begin_mutation_request<'a>, CodecError> {
        if wire.len() < BEGIN_MUTATION_HEADER {
            return Err(CodecError::Truncated);
        }
        let mut dec = Decoder::new(wire);
        let operation = dec.get_u32()?;
        let old_parent = NodeKey::decode(&mut dec)?;
        let old_target = NodeKey::decode(&mut dec)?;
        let new_parent = NodeKey::decode(&mut dec)?;
        let new_target = NodeKey::decode(&mut dec)?;
        let old_name_size = dec.get_u64()?;
        let new_name_size = dec.get_u64()?;
        if dec.offset() != BEGIN_MUTATION_HEADER {
            return Err(CodecError::InvalidMessage);
        }
        let mut segments = decode_segments(
            wire,
            BEGIN_MUTATION_HEADER,
            &[old_name_size, new_name_size],
            &[255, 255],
        )?;
        let new_name = segments.pop().ok_or(CodecError::InvalidMessage)?;
        let old_name = segments.pop().ok_or(CodecError::InvalidMessage)?;
        Ok(begin_mutation_request {
            operation,
            old_parent,
            old_target,
            new_parent,
            new_target,
            old_name,
            new_name,
        })
    }

    /// Create an endpoint pair carrying the internal NamespaceBinding scope.
    pub fn create_endpoints()
    -> Result<(ProtocolClientEndpoint, ProtocolServerEndpoint), sys::Status> {
        super::create_internal_endpoints(PROTOCOL_UUID, PROTOCOL_SCOPE, PROTOCOL_RIGHTS, 5, 1, 1)
    }
}

// ---------------------------------------------------------------------------
// MountControl (scope 21)
// ---------------------------------------------------------------------------

pub mod mount_control {
    use super::*;

    pub const PROTOCOL_UUID: [u8; 16] = [
        43, 158, 60, 46, 140, 125, 79, 177, 158, 33, 76, 75, 14, 10, 16, 38,
    ];
    pub const PROTOCOL_SCOPE: u64 = 21;
    pub const PROTOCOL_RIGHTS: u64 = (1 << 22) | 1;

    pub const METHOD_BIND_ROOT: u64 = 1;
    pub const METHOD_BIND_NODE: u64 = 2;
    pub const METHOD_SYNC: u64 = 3;
    pub const METHOD_PREPARE_UNMOUNT: u64 = 4;
    pub const METHOD_SHUTDOWN: u64 = 5;
    pub const METHOD_LOOKUP_TARGET: u64 = 6;

    pub const BIND_NODE_HEADER: usize = 28; // NodeKey(16) + flags(8) + slot(4)
    pub const LOOKUP_TARGET_RESPONSE_HEADER: usize = 40; // parent(16)+node(16)+name_size(8)

    #[derive(Clone, Copy, Debug)]
    pub struct bind_node_request {
        pub node: NodeKey,
        pub flags: u64,
        pub binding: ResourceSlot,
    }

    pub fn encode_bind_node_request(
        value: &bind_node_request,
        wire: &mut [u8],
    ) -> Result<usize, CodecError> {
        encode_message(wire, BIND_NODE_HEADER, 0, |enc| {
            value.node.encode(enc)?;
            enc.put_u64(value.flags)?;
            enc.put_u32(value.binding.index())
        })
    }

    pub fn decode_bind_node_request(wire: &[u8]) -> Result<bind_node_request, CodecError> {
        if wire.len() < BIND_NODE_HEADER {
            return Err(CodecError::Truncated);
        }
        let mut dec = Decoder::new(wire);
        let node = NodeKey::decode(&mut dec)?;
        let flags = dec.get_u64()?;
        let raw = dec.get_u32()?;
        require_exact(wire, BIND_NODE_HEADER)?;
        let binding = ResourceSlot::new(raw).ok_or(CodecError::InvalidResource)?;
        Ok(bind_node_request {
            node,
            flags,
            binding,
        })
    }

    #[derive(Clone, Copy, Debug)]
    pub struct lookup_target_request<'a> {
        pub walk: WalkContext,
        pub path: &'a [u8],
    }

    pub fn encode_lookup_target_request(
        value: &lookup_target_request<'_>,
        wire: &mut [u8],
    ) -> Result<usize, CodecError> {
        encode_message(wire, 24, value.path.len(), |enc| {
            value.walk.encode(enc)?;
            enc.put_u64(value.path.len() as u64)?;
            enc.put_bounded_bytes(value.path, 4095)?;
            Ok(())
        })
    }

    pub fn decode_lookup_target_request<'a>(
        wire: &'a [u8],
    ) -> Result<lookup_target_request<'a>, CodecError> {
        if wire.len() < 24 {
            return Err(CodecError::Truncated);
        }
        let mut dec = Decoder::new(wire);
        let walk = WalkContext::decode(&mut dec)?;
        let path_size = dec.get_u64()?;
        if dec.offset() != 24 {
            return Err(CodecError::InvalidMessage);
        }
        let mut segments = decode_segments(wire, 24, &[path_size], &[4095])?;
        let path = segments.pop().ok_or(CodecError::InvalidMessage)?;
        Ok(lookup_target_request { walk, path })
    }

    #[derive(Clone, Copy, Debug)]
    pub struct lookup_target_response<'a> {
        pub parent_dir: NodeKey,
        pub node: NodeKey,
        pub name: &'a [u8],
    }

    pub fn encode_lookup_target_response(
        value: &lookup_target_response<'_>,
        wire: &mut [u8],
    ) -> Result<usize, CodecError> {
        encode_message(
            wire,
            LOOKUP_TARGET_RESPONSE_HEADER,
            value.name.len(),
            |enc| {
                value.parent_dir.encode(enc)?;
                value.node.encode(enc)?;
                enc.put_u64(value.name.len() as u64)?;
                enc.put_bounded_bytes(value.name, 255)?;
                Ok(())
            },
        )
    }

    pub fn decode_lookup_target_response<'a>(
        wire: &'a [u8],
    ) -> Result<lookup_target_response<'a>, CodecError> {
        if wire.len() < LOOKUP_TARGET_RESPONSE_HEADER {
            return Err(CodecError::Truncated);
        }
        let mut dec = Decoder::new(wire);
        let parent_dir = NodeKey::decode(&mut dec)?;
        let node = NodeKey::decode(&mut dec)?;
        let name_size = dec.get_u64()?;
        if dec.offset() != LOOKUP_TARGET_RESPONSE_HEADER {
            return Err(CodecError::InvalidMessage);
        }
        let mut segments =
            decode_segments(wire, LOOKUP_TARGET_RESPONSE_HEADER, &[name_size], &[255])?;
        let name = segments.pop().ok_or(CodecError::InvalidMessage)?;
        Ok(lookup_target_response {
            parent_dir,
            node,
            name,
        })
    }

    /// Create an endpoint pair carrying the internal MountControl scope.
    pub fn create_endpoints()
    -> Result<(ProtocolClientEndpoint, ProtocolServerEndpoint), sys::Status> {
        super::create_internal_endpoints(PROTOCOL_UUID, PROTOCOL_SCOPE, PROTOCOL_RIGHTS, 6, 0, 1)
    }

    /// Submit the worker-side bind_node call. This is kept beside the private
    /// codec so vfsd can use the same wire contract without exposing an
    /// application SDK binding for MountControl. `target` is a borrowed raw
    /// client-end handle; its owning `OwnedHandle` must outlive the returned
    /// invocation.
    pub fn submit_bind_node(
        target: sys::Handle,
        value: &bind_node_request,
        resources: ResourceTable<'_>,
        wire: &mut [u8],
        operation_budget: u64,
    ) -> Result<Invocation, CallError> {
        let written = encode_bind_node_request(value, wire).map_err(CallError::Codec)?;
        let dispositions = resources.as_slice();
        let frame = sys::SubmitFrame {
            struct_size: core::mem::size_of::<sys::SubmitFrame>() as u32,
            method_id: METHOD_BIND_NODE,
            request: wire.as_ptr() as u64,
            request_bytes: written as u64,
            resources: dispositions.as_ptr() as u64,
            resource_count: dispositions.len() as u64,
            operation_budget,
            ..sys::SubmitFrame::default()
        };
        let mut raw_invocation = sys::HANDLE_INVALID;
        let status = unsafe { sys::_na_invoke_submit(target, &frame, &mut raw_invocation) };
        if status != sys::STATUS_OK {
            return Err(CallError::Status(status));
        }
        resources.commit_move();
        if raw_invocation == sys::HANDLE_INVALID {
            return Err(CallError::InvalidInvocation);
        }
        Ok(unsafe { Invocation::from_raw(raw_invocation) })
    }

    /// Submit a lifecycle request which has an empty request and response
    /// body.  MountControl lifecycle calls intentionally share this tiny
    /// adapter so vfsd cannot accidentally invent a second wire shape.
    pub fn submit_lifecycle(
        target: sys::Handle,
        method_id: u64,
        operation_budget: u64,
    ) -> Result<Invocation, CallError> {
        let frame = sys::SubmitFrame {
            struct_size: core::mem::size_of::<sys::SubmitFrame>() as u32,
            method_id,
            request: 0,
            request_bytes: 0,
            resources: 0,
            resource_count: 0,
            operation_budget,
            ..sys::SubmitFrame::default()
        };
        let mut raw_invocation = sys::HANDLE_INVALID;
        let status = unsafe { sys::_na_invoke_submit(target, &frame, &mut raw_invocation) };
        if status != sys::STATUS_OK {
            return Err(CallError::Status(status));
        }
        if raw_invocation == sys::HANDLE_INVALID {
            return Err(CallError::InvalidInvocation);
        }
        Ok(unsafe { Invocation::from_raw(raw_invocation) })
    }

    /// Consume an empty MountControl lifecycle response and surface worker
    /// execution/protocol errors to the caller.
    pub fn take_lifecycle(
        invocation: &mut Invocation,
        expected_method: u64,
        wire: &mut [u8],
    ) -> Result<(), CallError> {
        let mut raw_resources = [sys::HANDLE_INVALID; MAX_RESOURCES];
        let mut frame = sys::ResultFrame {
            struct_size: core::mem::size_of::<sys::ResultFrame>() as u32,
            bytes: wire.as_mut_ptr() as u64,
            byte_capacity: wire.len() as u64,
            resources: raw_resources.as_mut_ptr() as u64,
            resource_capacity: MAX_RESOURCES as u64,
            ..sys::ResultFrame::default()
        };
        let status = unsafe { sys::_na_invocation_take_result(invocation.get(), &mut frame) };
        if status != sys::STATUS_OK {
            return Err(CallError::Status(status));
        }
        invocation.mark_completed();
        let guard = RawHandleGuard::new(&raw_resources, MAX_RESOURCES)
            .ok_or(CallError::Codec(CodecError::BoundExceeded))?;
        if frame.method_id != expected_method
            || frame.actual_bytes != 0
            || frame.actual_resources != 0
        {
            return Err(CallError::Codec(CodecError::InvalidMessage));
        }
        if frame.execution_outcome != 0 || frame.protocol_error != 0 {
            core::mem::forget(guard);
            return Err(CallError::Outcome {
                execution: frame.execution_outcome,
                reason: frame.outcome_reason,
                protocol_error: frame.protocol_error,
            });
        }
        core::mem::forget(guard);
        Ok(())
    }

    /// Take a bind_node reply containing one moved Directory client end.
    pub fn take_bind_node(
        invocation: &mut Invocation,
        wire: &mut [u8],
    ) -> Result<(ResourceSlot, ReceivedResources), CallError> {
        let mut raw_resources = [sys::HANDLE_INVALID; MAX_RESOURCES];
        let mut frame = sys::ResultFrame {
            struct_size: core::mem::size_of::<sys::ResultFrame>() as u32,
            bytes: wire.as_mut_ptr() as u64,
            byte_capacity: wire.len() as u64,
            resources: raw_resources.as_mut_ptr() as u64,
            resource_capacity: MAX_RESOURCES as u64,
            ..sys::ResultFrame::default()
        };
        let status = unsafe { sys::_na_invocation_take_result(invocation.get(), &mut frame) };
        if status != sys::STATUS_OK {
            return Err(CallError::Status(status));
        }
        invocation.mark_completed();
        let mut guard = RawHandleGuard::new(&raw_resources, MAX_RESOURCES)
            .ok_or(CallError::Codec(CodecError::BoundExceeded))?;
        if frame.method_id != METHOD_BIND_NODE
            || frame.actual_bytes > wire.len() as u64
            || frame.actual_resources > MAX_RESOURCES as u64
        {
            return Err(CallError::Codec(CodecError::InvalidMessage));
        }
        let resources = match unsafe {
            ReceivedResources::from_raw(&raw_resources[..frame.actual_resources as usize])
        } {
            Ok(resources) => {
                guard.disarm();
                resources
            }
            Err(error) => {
                // from_raw owns cleanup on rejection; disarm the fallback
                // guard so it cannot close those handles a second time.
                guard.disarm();
                return Err(CallError::Resource(error));
            }
        };
        if frame.execution_outcome != 0 || frame.protocol_error != 0 {
            return Err(CallError::Outcome {
                execution: frame.execution_outcome,
                reason: frame.outcome_reason,
                protocol_error: frame.protocol_error,
            });
        }
        let slot = namespace_binding::decode_slot_response(&wire[..frame.actual_bytes as usize])
            .map_err(CallError::Codec)?;
        if resources.len() != 1 {
            return Err(CallError::Codec(CodecError::InvalidResource));
        }
        Ok((slot, resources))
    }
}

// ---------------------------------------------------------------------------
// MutationTicket (scope 22)
// ---------------------------------------------------------------------------

pub mod mutation_ticket {
    use super::*;

    pub const PROTOCOL_UUID: [u8; 16] = [
        43, 158, 60, 46, 140, 125, 79, 177, 158, 33, 76, 75, 14, 10, 16, 39,
    ];
    pub const PROTOCOL_SCOPE: u64 = 22;
    pub const PROTOCOL_RIGHTS: u64 = (1 << 26) | 1;

    pub const METHOD_COMMIT: u64 = 1;
    pub const METHOD_ABORT: u64 = 2;
    pub const METHOD_STATUS: u64 = 3;

    /// Empty request shared by commit/abort/status.
    pub const EMPTY_REQUEST: &[u8] = &[];

    /// status response: canonical state table shared with MountTicket
    /// (`PREPARED=0 .. EXPIRED=4`); see `mount.rs`.
    pub fn encode_status_response(state: u32, wire: &mut [u8]) -> Result<usize, CodecError> {
        encode_message(wire, 4, 0, |enc| enc.put_u32(state))
    }

    pub fn decode_status_response(wire: &[u8]) -> Result<u32, CodecError> {
        if wire.len() < 4 {
            return Err(CodecError::Truncated);
        }
        let mut dec = Decoder::new(wire);
        let state = dec.get_u32()?;
        require_exact(wire, 4)?;
        Ok(state)
    }

    /// Create an endpoint pair carrying the internal MutationTicket scope.
    pub fn create_endpoints()
    -> Result<(ProtocolClientEndpoint, ProtocolServerEndpoint), sys::Status> {
        super::create_internal_endpoints(PROTOCOL_UUID, PROTOCOL_SCOPE, PROTOCOL_RIGHTS, 3, 0, 0)
    }
}

// ---------------------------------------------------------------------------
// Endpoint creation over the raw syscall surface
// ---------------------------------------------------------------------------

/// Build a protocol descriptor + endpoint pair for an internal protocol.
///
/// `method_count` methods are all twoway with identical `rights`; the bitmap
/// marks ids `1..=method_count`. `max_request_slots`/`max_response_slots`
/// size the descriptor's resource ceilings conservatively.
fn create_internal_endpoints(
    uuid: [u8; 16],
    scope: u64,
    rights: u64,
    method_count: usize,
    _max_request_slots: usize,
    max_response_slots: usize,
) -> Result<(ProtocolClientEndpoint, ProtocolServerEndpoint), sys::Status> {
    let mut descriptor = sys::ProtocolDescriptor {
        struct_size: core::mem::size_of::<sys::ProtocolDescriptor>() as u32,
        flags: 0,
        uuid: sys::Uuid { bytes: uuid },
        scope,
        revision: 1,
        features: 0,
        protocol_rights: rights,
        method_count: method_count as u64,
        max_request_bytes: 65536,
        max_response_bytes: 65536,
        max_resources: max_response_slots.max(1) as u64,
        ..sys::ProtocolDescriptor::default()
    };
    // method_bitmap is four u64 words marking twoway ids 1..=method_count.
    let mut bitmap = [0u64; 4];
    for id in 1..=method_count {
        bitmap[(id - 1) / 64] |= 1 << ((id - 1) % 64);
    }
    descriptor.method_bitmap = bitmap;
    descriptor.oneway_bitmap = [0; 4];
    let mut method_rights = [0u64; 256];
    for entry in method_rights.iter_mut().take(method_count) {
        *entry = rights;
    }
    descriptor.method_rights = method_rights;

    let mut raw_descriptor = sys::HANDLE_INVALID;
    let status = unsafe { sys::_na_protocol_descriptor_create(&descriptor, &mut raw_descriptor) };
    if status != sys::STATUS_OK {
        return Err(status);
    }
    if raw_descriptor == sys::HANDLE_INVALID {
        return Err(sys::STATUS_INVALID_HANDLE);
    }

    // SAFETY: freshly created descriptor handle, uniquely owned here; closed
    // on every exit path below.
    let descriptor = unsafe { naos_idl::OwnedHandle::from_raw(raw_descriptor) };
    let mut raw_client = sys::HANDLE_INVALID;
    let mut raw_server = sys::HANDLE_INVALID;
    let status = unsafe {
        sys::_na_protocol_endpoint_create(
            descriptor.get(),
            core::ptr::null(),
            &mut raw_client,
            &mut raw_server,
        )
    };
    if status != sys::STATUS_OK {
        return Err(status);
    }
    if raw_client == sys::HANDLE_INVALID || raw_server == sys::HANDLE_INVALID {
        return Err(sys::STATUS_INVALID_HANDLE);
    }
    // SAFETY: freshly minted endpoint handles from the kernel/loopback.
    Ok((
        unsafe { ProtocolClientEndpoint::from_raw(raw_client) },
        unsafe { ProtocolServerEndpoint::from_raw(raw_server) },
    ))
}
