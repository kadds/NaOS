#![no_std]

#[cfg(feature = "naos")]
mod kernel_ops;
#[cfg(all(feature = "test-loopback", feature = "naos"))]
pub mod loopback;
#[cfg(feature = "naos")]
pub mod server;
pub mod transport;

#[cfg(feature = "naos")]
use naos_sys as sys;
#[cfg(feature = "naos")]
pub use server::{
    DispatchOutcome, EXECUTION_NONE, EXECUTION_NOT_DELIVERED, EXECUTION_OUTCOME_UNKNOWN,
    FailInvocation, IncomingRequest, MethodReply, OptionalResponder, REASON_BROKER_FAILURE,
    REASON_CANCEL_REQUESTED, REASON_NONE, REASON_OBJECT_REVOKED, REASON_OPERATION_DEADLINE,
    REASON_PEER_CLOSED, REASON_PROTOCOL_VIOLATION, REASON_REQUEST_DISCARDED,
    REASON_RESPONDER_ABANDONED, REASON_UNSUPPORTED, ReplySink, ResponderHandle,
    create_endpoints_from_descriptor, receive_request, reject_protocol_violation, reject_request,
    reject_unsupported,
};

#[cfg(feature = "alloc")]
extern crate alloc;

// The loopback fake kernel keeps per-test queues and needs `thread_local!`.
#[cfg(any(test, feature = "test-loopback"))]
extern crate std;

/// True when the in-process fake kernel from `naos_idl::loopback` is
/// installed.
///
/// A host test substitutes fake capability metadata for the kernel's, so a
/// handle-addressed lookup may resolve against test state instead of issuing
/// a real syscall.  Production never installs the fake kernel, so this is
/// always false there and callers must not touch a capability handle.
#[cfg(feature = "naos")]
pub fn host_fake_kernel_installed() -> bool {
    kernel_ops::overridden()
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum CodecError {
    NullBuffer,
    Overflow,
    Truncated,
    BoundExceeded,
    InvalidUtf8,
    InvalidMessage,
    InvalidResource,
}

pub struct Encoder<'a> {
    buffer: &'a mut [u8],
    offset: usize,
    failed: bool,
}

impl<'a> Encoder<'a> {
    pub fn new(buffer: &'a mut [u8]) -> Self {
        Self {
            buffer,
            offset: 0,
            failed: false,
        }
    }

    pub fn put_u8(&mut self, value: u8) -> Result<(), CodecError> {
        self.put_raw(&[value])
    }

    pub fn put_u16(&mut self, value: u16) -> Result<(), CodecError> {
        self.put_raw(&value.to_le_bytes())
    }

    pub fn put_u32(&mut self, value: u32) -> Result<(), CodecError> {
        self.put_raw(&value.to_le_bytes())
    }

    pub fn put_u64(&mut self, value: u64) -> Result<(), CodecError> {
        self.put_raw(&value.to_le_bytes())
    }

    pub fn put_i8(&mut self, value: i8) -> Result<(), CodecError> {
        self.put_u8(value as u8)
    }

    pub fn put_i16(&mut self, value: i16) -> Result<(), CodecError> {
        self.put_u16(value as u16)
    }

    pub fn put_i32(&mut self, value: i32) -> Result<(), CodecError> {
        self.put_u32(value as u32)
    }

    pub fn put_i64(&mut self, value: i64) -> Result<(), CodecError> {
        self.put_u64(value as u64)
    }

    pub fn put_f32(&mut self, value: f32) -> Result<(), CodecError> {
        self.put_u32(value.to_bits())
    }

    pub fn put_f64(&mut self, value: f64) -> Result<(), CodecError> {
        self.put_u64(value.to_bits())
    }

    pub fn put_bytes(&mut self, value: &[u8]) -> Result<(), CodecError> {
        self.put_raw(value)
    }

    pub fn put_bounded_bytes(&mut self, value: &[u8], bound: usize) -> Result<(), CodecError> {
        if value.len() > bound {
            return self.fail(CodecError::BoundExceeded);
        }
        self.put_bytes(value)
    }

    pub fn as_bytes(&self) -> &[u8] {
        &self.buffer[..self.offset]
    }

    pub fn finish(self) -> &'a [u8] {
        let offset = self.offset;
        let buffer = self.buffer;
        &buffer[..offset]
    }

    pub fn written(&self) -> usize {
        self.offset
    }

    fn put_raw(&mut self, value: &[u8]) -> Result<(), CodecError> {
        if self.failed {
            return Err(CodecError::Overflow);
        }
        let end = self
            .offset
            .checked_add(value.len())
            .ok_or(CodecError::Overflow)?;
        if end > self.buffer.len() {
            return self.fail(CodecError::Overflow);
        }
        self.buffer[self.offset..end].copy_from_slice(value);
        self.offset = end;
        Ok(())
    }

    fn fail<T>(&mut self, error: CodecError) -> Result<T, CodecError> {
        self.failed = true;
        Err(error)
    }
}

pub struct Decoder<'a> {
    buffer: &'a [u8],
    offset: usize,
    failed: bool,
}

impl<'a> Decoder<'a> {
    pub fn new(buffer: &'a [u8]) -> Self {
        Self {
            buffer,
            offset: 0,
            failed: false,
        }
    }

    pub fn get_u8(&mut self) -> Result<u8, CodecError> {
        Ok(self.get_raw(1)?[0])
    }

    pub fn get_u16(&mut self) -> Result<u16, CodecError> {
        Ok(u16::from_le_bytes(
            self.get_raw(2)?.try_into().unwrap_or([0; 2]),
        ))
    }

    pub fn get_u32(&mut self) -> Result<u32, CodecError> {
        Ok(u32::from_le_bytes(
            self.get_raw(4)?.try_into().unwrap_or([0; 4]),
        ))
    }

    pub fn get_u64(&mut self) -> Result<u64, CodecError> {
        Ok(u64::from_le_bytes(
            self.get_raw(8)?.try_into().unwrap_or([0; 8]),
        ))
    }

    pub fn get_i8(&mut self) -> Result<i8, CodecError> {
        Ok(self.get_u8()? as i8)
    }

    pub fn get_i16(&mut self) -> Result<i16, CodecError> {
        Ok(self.get_u16()? as i16)
    }

    pub fn get_i32(&mut self) -> Result<i32, CodecError> {
        Ok(self.get_u32()? as i32)
    }

    pub fn get_i64(&mut self) -> Result<i64, CodecError> {
        Ok(self.get_u64()? as i64)
    }

    pub fn get_f32(&mut self) -> Result<f32, CodecError> {
        Ok(f32::from_bits(self.get_u32()?))
    }

    pub fn get_f64(&mut self) -> Result<f64, CodecError> {
        Ok(f64::from_bits(self.get_u64()?))
    }

    pub fn get_bytes(&mut self, size: usize) -> Result<&'a [u8], CodecError> {
        let end = self.offset.checked_add(size).ok_or(CodecError::Overflow)?;
        if end > self.buffer.len() {
            self.failed = true;
            return Err(CodecError::Truncated);
        }
        let value = &self.buffer[self.offset..end];
        self.offset = end;
        Ok(value)
    }

    pub fn remaining(&self) -> usize {
        self.buffer.len().saturating_sub(self.offset)
    }

    pub fn offset(&self) -> usize {
        self.offset
    }

    pub fn is_empty(&self) -> bool {
        !self.failed && self.remaining() == 0
    }

    pub fn is_failed(&self) -> bool {
        self.failed
    }

    fn get_raw(&mut self, size: usize) -> Result<&'a [u8], CodecError> {
        self.get_bytes(size)
    }
}

/// A process-local capability with exactly one owner. Dropping it closes the
/// handle; `into_raw` is the explicit MOVE escape hatch for a syscall frame.
#[cfg(feature = "naos")]
pub struct OwnedHandle(sys::Handle);

#[cfg(feature = "naos")]
impl OwnedHandle {
    pub unsafe fn from_raw(handle: sys::Handle) -> Self {
        Self(handle)
    }

    pub fn invalid() -> Self {
        Self(sys::HANDLE_INVALID)
    }

    pub fn get(&self) -> sys::Handle {
        self.0
    }

    pub fn is_valid(&self) -> bool {
        self.0 != sys::HANDLE_INVALID
    }

    pub fn into_raw(mut self) -> sys::Handle {
        core::mem::replace(&mut self.0, sys::HANDLE_INVALID)
    }

    pub fn duplicate(&self, rights: u64) -> Result<Self, sys::Status> {
        let mut result = sys::HANDLE_INVALID;
        let status = unsafe { (kernel_ops::ops().handle_duplicate)(self.0, rights, &mut result) };
        if status == sys::STATUS_OK && result != sys::HANDLE_INVALID {
            Ok(unsafe { Self::from_raw(result) })
        } else {
            Err(if status == sys::STATUS_OK {
                sys::STATUS_INVALID_HANDLE
            } else {
                status
            })
        }
    }

    pub fn restrict(&self, restriction: &sys::HandleRestriction) -> Result<Self, sys::Status> {
        let mut result = sys::HANDLE_INVALID;
        let status =
            unsafe { (kernel_ops::ops().handle_restrict)(self.0, restriction, &mut result) };
        if status == sys::STATUS_OK && result != sys::HANDLE_INVALID {
            Ok(unsafe { Self::from_raw(result) })
        } else {
            Err(if status == sys::STATUS_OK {
                sys::STATUS_INVALID_HANDLE
            } else {
                status
            })
        }
    }
}

#[cfg(feature = "naos")]
impl Drop for OwnedHandle {
    fn drop(&mut self) {
        if self.0 != sys::HANDLE_INVALID {
            let _ = unsafe { (kernel_ops::ops().handle_close)(self.0) };
            self.0 = sys::HANDLE_INVALID;
        }
    }
}

#[cfg(feature = "naos")]
impl core::fmt::Debug for OwnedHandle {
    fn fmt(&self, formatter: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        formatter.debug_tuple("OwnedHandle").field(&self.0).finish()
    }
}

#[cfg(feature = "naos")]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum CallError {
    Status(sys::Status),
    Codec(CodecError),
    Resource(ResourceError),
    InvalidHandle,
    InvalidInvocation,
    Outcome {
        execution: u32,
        reason: u32,
        protocol_error: i64,
    },
}

macro_rules! define_protocol_endpoint {
    ($name:ident) => {
        pub struct $name(OwnedHandle);

        impl $name {
            pub unsafe fn from_raw(handle: sys::Handle) -> Self {
                Self(unsafe { OwnedHandle::from_raw(handle) })
            }

            pub fn get(&self) -> sys::Handle {
                self.0.get()
            }

            pub fn is_valid(&self) -> bool {
                self.0.is_valid()
            }

            pub fn into_raw(self) -> sys::Handle {
                self.0.into_raw()
            }
        }
    };
}

#[cfg(feature = "naos")]
define_protocol_endpoint!(ProtocolClientEndpoint);
#[cfg(feature = "naos")]
define_protocol_endpoint!(ProtocolServerEndpoint);

#[cfg(feature = "naos")]
pub struct ChannelEndpoint(OwnedHandle);

#[cfg(feature = "naos")]
impl ChannelEndpoint {
    pub fn create(options: Option<&sys::ChannelOptions>) -> Result<(Self, Self), sys::Status> {
        let mut left = sys::HANDLE_INVALID;
        let mut right = sys::HANDLE_INVALID;
        let options = options.map_or(core::ptr::null(), |value| value as *const _);
        let status = unsafe { (kernel_ops::ops().channel_create)(options, &mut left, &mut right) };
        if status != sys::STATUS_OK {
            return Err(status);
        }
        if left == sys::HANDLE_INVALID || right == sys::HANDLE_INVALID {
            if left != sys::HANDLE_INVALID {
                let _ = unsafe { sys::_na_handle_close(left) };
            }
            if right != sys::HANDLE_INVALID {
                let _ = unsafe { sys::_na_handle_close(right) };
            }
            return Err(sys::STATUS_INVALID_HANDLE);
        }
        Ok((
            Self(unsafe { OwnedHandle::from_raw(left) }),
            Self(unsafe { OwnedHandle::from_raw(right) }),
        ))
    }

    pub unsafe fn from_raw(handle: sys::Handle) -> Self {
        Self(unsafe { OwnedHandle::from_raw(handle) })
    }

    pub fn get(&self) -> sys::Handle {
        self.0.get()
    }

    pub fn into_raw(self) -> sys::Handle {
        self.0.into_raw()
    }
}

/// A native invocation capability. Dropping a pending invocation requests
/// cancellation before the underlying handle is closed.
#[cfg(feature = "naos")]
pub struct Invocation {
    handle: OwnedHandle,
    completed: bool,
}

/// Result envelope for a protocol invocation submitted through the generic
/// servicekit client facade. Generated protocol bindings expose typed
/// equivalents, while this shape lets platform transports share one client
/// implementation without leaking raw channel frames to daemons.
#[cfg(feature = "naos")]
pub struct RawInvocationResult {
    pub method_id: u64,
    pub execution: u32,
    pub reason: u32,
    pub protocol_error: i64,
    pub bytes: usize,
    pub resources: ReceivedResources,
}

#[cfg(feature = "naos")]
pub fn submit_invocation(
    target: &ProtocolClientEndpoint,
    method_id: u64,
    request: &[u8],
    resources: ResourceTable<'_>,
    operation_budget: u64,
) -> Result<Invocation, CallError> {
    let dispositions = resources.as_slice();
    let frame = sys::SubmitFrame {
        struct_size: core::mem::size_of::<sys::SubmitFrame>() as u32,
        flags: 0,
        method_id,
        request: if request.is_empty() {
            0
        } else {
            request.as_ptr() as u64
        },
        request_bytes: request.len() as u64,
        resources: if dispositions.is_empty() {
            0
        } else {
            dispositions.as_ptr() as u64
        },
        resource_count: dispositions.len() as u64,
        operation_budget,
        ..sys::SubmitFrame::default()
    };
    let mut raw_invocation = sys::HANDLE_INVALID;
    let status =
        unsafe { (kernel_ops::ops().invoke_submit)(target.get(), &frame, &mut raw_invocation) };
    if status != sys::STATUS_OK {
        return Err(CallError::Status(status));
    }
    resources.commit_move();
    Ok(unsafe { Invocation::from_raw(raw_invocation) })
}

#[cfg(feature = "naos")]
pub fn take_invocation_result(
    invocation: &mut Invocation,
    wire: &mut [u8],
) -> Result<RawInvocationResult, CallError> {
    let mut raw_resources = [sys::HANDLE_INVALID; MAX_RESOURCES];
    let mut frame = sys::ResultFrame {
        struct_size: core::mem::size_of::<sys::ResultFrame>() as u32,
        bytes: if wire.is_empty() {
            0
        } else {
            wire.as_mut_ptr() as u64
        },
        byte_capacity: wire.len() as u64,
        resources: raw_resources.as_mut_ptr() as u64,
        resource_capacity: MAX_RESOURCES as u64,
        ..sys::ResultFrame::default()
    };
    let status =
        unsafe { (kernel_ops::ops().invocation_take_result)(invocation.get(), &mut frame) };
    if status != sys::STATUS_OK {
        return Err(CallError::Status(status));
    }
    invocation.mark_completed();
    if frame.actual_bytes > wire.len() as u64 || frame.actual_resources > MAX_RESOURCES as u64 {
        return Err(CallError::Codec(CodecError::BoundExceeded));
    }
    let mut guard = RawHandleGuard::new(&raw_resources, frame.actual_resources as usize)
        .ok_or(CallError::Codec(CodecError::BoundExceeded))?;
    let resources = match unsafe {
        ReceivedResources::from_raw(&raw_resources[..frame.actual_resources as usize])
    } {
        Ok(resources) => {
            guard.disarm();
            resources
        }
        Err(error) => {
            // from_raw closes handles when it rejects the raw set. Do not
            // let the fallback guard close the same descriptors again.
            guard.disarm();
            return Err(CallError::Resource(error));
        }
    };
    Ok(RawInvocationResult {
        method_id: frame.method_id,
        execution: frame.execution_outcome,
        reason: frame.outcome_reason,
        protocol_error: frame.protocol_error,
        bytes: frame.actual_bytes as usize,
        resources,
    })
}

#[cfg(feature = "naos")]
impl Invocation {
    pub unsafe fn from_raw(handle: sys::Handle) -> Self {
        Self {
            handle: unsafe { OwnedHandle::from_raw(handle) },
            completed: false,
        }
    }

    pub fn get(&self) -> sys::Handle {
        self.handle.get()
    }

    pub fn is_valid(&self) -> bool {
        self.handle.is_valid()
    }

    /// Mark a result as consumed after a caller has taken its result frame.
    /// Generated bindings and private protocol adapters both use this to
    /// prevent `Drop` from issuing a second cancellation.
    pub fn mark_completed(&mut self) {
        self.completed = true;
    }

    pub fn cancel(&self) -> sys::Status {
        if !self.is_valid() {
            return sys::STATUS_INVALID_HANDLE;
        }
        unsafe { (kernel_ops::ops().invocation_cancel)(self.get()) }
    }
}

#[cfg(feature = "naos")]
impl Drop for Invocation {
    fn drop(&mut self) {
        if self.is_valid() && !self.completed {
            let _ = self.cancel();
        }
    }
}

#[cfg(feature = "naos")]
pub struct RawHandleGuard<'a> {
    handles: &'a [sys::Handle],
    armed: bool,
}

#[cfg(feature = "naos")]
impl<'a> RawHandleGuard<'a> {
    pub fn new(handles: &'a [sys::Handle], length: usize) -> Option<Self> {
        (length <= handles.len()).then_some(Self {
            handles: &handles[..length],
            armed: true,
        })
    }

    pub fn as_slice(&self) -> &[sys::Handle] {
        self.handles
    }

    pub fn disarm(&mut self) {
        self.armed = false;
    }
}

#[cfg(feature = "naos")]
impl Drop for RawHandleGuard<'_> {
    fn drop(&mut self) {
        if self.armed {
            for handle in self.handles.iter().copied() {
                if handle != sys::HANDLE_INVALID {
                    let _ = unsafe { (kernel_ops::ops().handle_close)(handle) };
                }
            }
        }
    }
}

include!(concat!(env!("OUT_DIR"), "/bindings.rs"));

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct ResourceSlot(u32);

impl ResourceSlot {
    pub const fn new(index: u32) -> Option<Self> {
        if (index as usize) < MAX_RESOURCES {
            Some(Self(index))
        } else {
            None
        }
    }

    pub const fn index(self) -> u32 {
        self.0
    }
}

pub const MAX_RESOURCES: usize = 64;

/// Validate a generated resource slot against the neutral Linux/NaOS
/// transport descriptor.  The platform adapter performs the authoritative
/// identity check; this helper enforces the generated method's exact slot,
/// binding, scope and minimum rights contract before a handler runs.
pub fn validate_transport_resource_slot(
    resources: &[transport::ResourceDescriptor],
    slot: ResourceSlot,
    expected_binding: u32,
    expected_scope: u64,
    required_rights: u64,
    seen: &mut [bool; MAX_RESOURCES],
    used: &mut usize,
) -> Result<(), CodecError> {
    let index = slot.index() as usize;
    if index >= resources.len() || index >= MAX_RESOURCES || seen[index] {
        return Err(CodecError::InvalidResource);
    }
    let resource = resources[index];
    if resource.resource_id == 0
        || (expected_binding != 0 && resource.binding != expected_binding)
        || (expected_scope != 0 && resource.scope != expected_scope)
        || resource.rights & required_rights != required_rights
        || resource.rights & (1 << 1) == 0
    {
        return Err(CodecError::InvalidResource);
    }
    seen[index] = true;
    *used = used.checked_add(1).ok_or(CodecError::Overflow)?;
    Ok(())
}

#[cfg(feature = "naos")]
pub fn validate_resource_slot(
    resources: &[sys::ResourceDisposition],
    slot: ResourceSlot,
    operation: u32,
    required_rights: u64,
    expected_scope: u64,
    seen: &mut [bool; MAX_RESOURCES],
    used: &mut usize,
) -> Result<(), CodecError> {
    if resources.len() > MAX_RESOURCES {
        return Err(CodecError::BoundExceeded);
    }
    let index = slot.index() as usize;
    if index >= resources.len() || seen[index] {
        return Err(CodecError::InvalidResource);
    }
    let disposition = resources[index];
    if disposition.flags != 0
        || disposition.operation != operation
        || (disposition.rights != 0 && disposition.rights & required_rights != required_rights)
        || (expected_scope != 0 && disposition.scope != 0 && disposition.scope != expected_scope)
    {
        return Err(CodecError::InvalidResource);
    }
    seen[index] = true;
    *used = used.checked_add(1).ok_or(CodecError::Overflow)?;
    Ok(())
}

#[cfg(feature = "naos")]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ResourceError {
    Full,
    InvalidHandle,
}

#[cfg(feature = "naos")]
pub struct ResourceTable<'a> {
    dispositions: [sys::ResourceDisposition; MAX_RESOURCES],
    owners: [Option<OwnedHandle>; MAX_RESOURCES],
    len: usize,
    borrowed: core::marker::PhantomData<&'a OwnedHandle>,
}

/// Owned capabilities returned by a native invocation or protocol receive.
/// The kernel has already installed these handles in the current process;
/// this type makes the Rust ownership transition explicit and closes every
/// unclaimed handle on drop.
#[cfg(feature = "naos")]
pub struct ReceivedResources {
    owners: [Option<OwnedHandle>; MAX_RESOURCES],
    len: usize,
}

#[cfg(feature = "naos")]
impl ReceivedResources {
    /// Adopt handles produced by `_na_invocation_take_result` or
    /// `_na_channel_receive`. The caller must pass exactly the handles written
    /// by that syscall and must not use them after this function returns.
    pub unsafe fn from_raw(handles: &[sys::Handle]) -> Result<Self, ResourceError> {
        if handles.len() > MAX_RESOURCES
            || handles.iter().any(|handle| *handle == sys::HANDLE_INVALID)
        {
            for handle in handles.iter().copied() {
                if handle != sys::HANDLE_INVALID {
                    let _ = unsafe { sys::_na_handle_close(handle) };
                }
            }
            return Err(ResourceError::InvalidHandle);
        }
        let mut result = Self {
            owners: core::array::from_fn(|_| None),
            len: 0,
        };
        for (index, handle) in handles.iter().copied().enumerate() {
            result.owners[index] = Some(unsafe { OwnedHandle::from_raw(handle) });
            result.len += 1;
        }
        Ok(result)
    }

    pub fn len(&self) -> usize {
        self.len
    }

    pub fn is_empty(&self) -> bool {
        self.len == 0
    }

    pub fn get(&self, slot: ResourceSlot) -> Option<&OwnedHandle> {
        self.owners
            .get(slot.index() as usize)
            .and_then(Option::as_ref)
    }

    /// Relinquish ownership of the handle stored at `slot`, moving it out of
    /// this set.  The slot stops resolving and Drop will not close a taken
    /// handle; use this to MOVE a received capability onward.
    pub fn take(&mut self, slot: ResourceSlot) -> Option<OwnedHandle> {
        self.owners
            .get_mut(slot.index() as usize)
            .and_then(Option::take)
    }
}

/// Check the metadata of a handle received from the kernel before exposing a
/// generated typed response/request to user code.
#[cfg(feature = "naos")]
pub fn validate_received_resource(
    resources: &ReceivedResources,
    slot: ResourceSlot,
    expected_binding: u32,
    expected_scope: u64,
    required_meta_rights: u64,
    required_protocol_rights: u64,
) -> Result<(), CodecError> {
    let owner = resources.get(slot).ok_or(CodecError::InvalidResource)?;
    let mut info = sys::HandleInfo {
        struct_size: core::mem::size_of::<sys::HandleInfo>() as u32,
        ..sys::HandleInfo::default()
    };
    let status = unsafe { (kernel_ops::ops().handle_get_info)(owner.get(), &mut info) };
    if status != sys::STATUS_OK
        || (expected_binding != sys::BINDING_NONE && info.binding != expected_binding)
        || (expected_scope != 0 && info.scope != expected_scope)
        || (info.meta_rights & required_meta_rights) != required_meta_rights
        || (info.protocol_rights & required_protocol_rights) != required_protocol_rights
    {
        return Err(CodecError::InvalidResource);
    }
    Ok(())
}

/// Return the kernel identity of a capability rather than its process-local
/// handle number.  MOVE transfers may renumber a handle in the receiving
/// process, while this identity remains stable for the lifetime of the
/// underlying object.
#[cfg(feature = "naos")]
pub fn object_id(handle: sys::Handle) -> Result<u64, CallError> {
    let info = handle_info(handle)?;
    if info.object_id == 0 {
        return Err(CallError::InvalidHandle);
    }
    Ok(info.object_id)
}

/// Read the kernel's authoritative description of a received capability.
///
/// The kernel capability table, not a peer-supplied descriptor, is the
/// authority for binding, scope and rights on NaOS; admission decisions read
/// these fields rather than trusting the request payload.
#[cfg(feature = "naos")]
pub fn handle_info(handle: sys::Handle) -> Result<sys::HandleInfo, CallError> {
    let mut info = sys::HandleInfo {
        struct_size: core::mem::size_of::<sys::HandleInfo>() as u32,
        ..sys::HandleInfo::default()
    };
    let status = unsafe { (kernel_ops::ops().handle_get_info)(handle, &mut info) };
    if status != sys::STATUS_OK {
        return Err(CallError::Status(status));
    }
    Ok(info)
}

#[cfg(feature = "naos")]
impl<'a> ResourceTable<'a> {
    pub fn new() -> Self {
        Self {
            dispositions: [sys::ResourceDisposition::default(); MAX_RESOURCES],
            owners: core::array::from_fn(|_| None),
            len: 0,
            borrowed: core::marker::PhantomData,
        }
    }

    pub fn push_move(&mut self, owner: OwnedHandle) -> Result<ResourceSlot, ResourceError> {
        if !owner.is_valid() {
            return Err(ResourceError::InvalidHandle);
        }
        let slot = self.reserve()?;
        self.owners[slot.0 as usize] = Some(owner);
        self.dispositions[slot.0 as usize] = sys::ResourceDisposition {
            handle: self.owners[slot.0 as usize].as_ref().unwrap().get(),
            operation: sys::RESOURCE_MOVE,
            ..sys::ResourceDisposition::default()
        };
        Ok(slot)
    }

    pub fn push_duplicate(
        &mut self,
        owner: &'a OwnedHandle,
    ) -> Result<ResourceSlot, ResourceError> {
        if !owner.is_valid() {
            return Err(ResourceError::InvalidHandle);
        }
        let slot = self.reserve()?;
        self.dispositions[slot.0 as usize] = sys::ResourceDisposition {
            handle: owner.get(),
            operation: sys::RESOURCE_DUPLICATE,
            ..sys::ResourceDisposition::default()
        };
        Ok(slot)
    }

    pub fn as_slice(&self) -> &[sys::ResourceDisposition] {
        &self.dispositions[..self.len]
    }

    pub fn len(&self) -> usize {
        self.len
    }

    pub fn is_empty(&self) -> bool {
        self.len == 0
    }

    /// Commit a successful send/invocation. MOVE entries are now owned by the
    /// peer, so their local owners must not close the transferred handles.
    pub fn commit_move(mut self) {
        for owner in &mut self.owners[..self.len] {
            if let Some(owner) = owner.take() {
                core::mem::forget(owner);
            }
        }
    }

    fn reserve(&mut self) -> Result<ResourceSlot, ResourceError> {
        if self.len == MAX_RESOURCES {
            return Err(ResourceError::Full);
        }
        let slot = ResourceSlot(self.len as u32);
        self.len += 1;
        Ok(slot)
    }
}

#[cfg(feature = "naos")]
impl Default for ResourceTable<'_> {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(feature = "naos")]
impl Drop for ReceivedResources {
    fn drop(&mut self) {
        for owner in &mut self.owners[..self.len] {
            owner.take();
        }
    }
}
