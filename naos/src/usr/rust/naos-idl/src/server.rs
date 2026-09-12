//! Server-side invocation runtime shared by every generated protocol binding.
//!
//! The generated code stays thin: it only knows how to decode/encode one
//! protocol and how to route a method id to a handler call.  Everything that
//! touches the kernel (receiving requests, delivering replies or typed
//! failures) lives here.

use naos_sys as sys;

use crate::kernel_ops::ops;
use crate::{
    CallError, CodecError, MAX_RESOURCES, OwnedHandle, ProtocolServerEndpoint, RawHandleGuard,
    ReceivedResources, ResourceTable,
};

/// Execution outcomes of an invocation, mirroring `na_execution_outcome_t`
/// from `naos/abi.h`.
pub const EXECUTION_NONE: u32 = 0;
pub const EXECUTION_NOT_DELIVERED: u32 = 1;
pub const EXECUTION_OUTCOME_UNKNOWN: u32 = 2;

/// Outcome reasons, mirroring `na_outcome_reason_t` from `naos/abi.h`.
pub const REASON_NONE: u32 = 0;
pub const REASON_PEER_CLOSED: u32 = 1;
pub const REASON_OBJECT_REVOKED: u32 = 2;
pub const REASON_OPERATION_DEADLINE: u32 = 3;
pub const REASON_CANCEL_REQUESTED: u32 = 4;
pub const REASON_REQUEST_DISCARDED: u32 = 5;
pub const REASON_RESPONDER_ABANDONED: u32 = 6;
pub const REASON_BROKER_FAILURE: u32 = 7;
pub const REASON_PROTOCOL_VIOLATION: u32 = 8;
pub const REASON_UNSUPPORTED: u32 = 9;

/// A typed failure a server handler wants delivered to the caller.
///
/// Domain errors (the `@errors(...)` sets of a method) use
/// [`FailInvocation::domain`] with the negative POSIX errno; transport-style
/// failures use the explicit outcome/reason pairs.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct FailInvocation {
    pub execution: u32,
    pub reason: u32,
    pub protocol_error: i64,
}

impl FailInvocation {
    /// Domain failure carrying a negative errno (for example one of the
    /// generated `{METHOD}_ERROR_{NAME}` constants).
    pub const fn domain(protocol_error: i64) -> Self {
        Self {
            execution: EXECUTION_NONE,
            reason: REASON_NONE,
            protocol_error,
        }
    }

    /// The requested method is not part of this revision of the protocol.
    pub const fn unsupported() -> Self {
        Self {
            execution: EXECUTION_NOT_DELIVERED,
            reason: REASON_UNSUPPORTED,
            protocol_error: 0,
        }
    }

    /// The peer violated the wire/disposition contract of the protocol.
    pub const fn protocol_violation() -> Self {
        Self {
            execution: EXECUTION_NOT_DELIVERED,
            reason: REASON_PROTOCOL_VIOLATION,
            protocol_error: 0,
        }
    }

    /// The request could not be delivered to a live handler.
    pub const fn not_delivered(reason: u32) -> Self {
        Self {
            execution: EXECUTION_NOT_DELIVERED,
            reason,
            protocol_error: 0,
        }
    }
}

/// Delivery channel for exactly one invocation's reply.
///
/// The production implementation is [`ResponderHandle`] (forwards to
/// `_na_responder_reply` / `_na_responder_fail`); tests can record frames with
/// an in-process sink instead of talking to a kernel.
pub trait ReplySink {
    fn reply(
        &mut self,
        bytes: &[u8],
        dispositions: &[sys::ResourceDisposition],
    ) -> Result<(), sys::Status>;

    fn fail(&mut self, failure: FailInvocation) -> Result<(), sys::Status>;
}

fn status_result(status: sys::Status) -> Result<(), sys::Status> {
    if status == sys::STATUS_OK {
        Ok(())
    } else {
        Err(status)
    }
}

/// The responder capability the kernel handed us for one received request.
///
/// Dropping it closes the responder handle, which the kernel surfaces to the
/// waiting caller as `RESPONDER_ABANDONED`.  A successful `reply`/`fail`
/// consumes the handle.
pub struct ResponderHandle(OwnedHandle);

impl ResponderHandle {
    /// Adopt a responder handle produced by `_na_channel_receive`.
    ///
    /// # Safety
    /// The caller must own exactly one reference to `handle` and must not use
    /// it afterwards.
    pub unsafe fn from_raw(handle: sys::Handle) -> Self {
        // SAFETY: caller guarantees unique ownership of `handle`.
        Self(unsafe { OwnedHandle::from_raw(handle) })
    }

    pub fn get(&self) -> sys::Handle {
        self.0.get()
    }

    pub fn is_valid(&self) -> bool {
        self.0.is_valid()
    }
    /// Deliver a successful reply and consume the responder.
    ///
    /// Also reachable through the [`ReplySink`] trait implementation below;
    /// the inherent form exists so generated bindings can call it without
    /// importing the trait.
    pub fn reply(
        &mut self,
        bytes: &[u8],
        dispositions: &[sys::ResourceDisposition],
    ) -> Result<(), sys::Status> {
        let raw = core::mem::replace(&mut self.0, OwnedHandle::invalid()).into_raw();
        let frame = sys::ReplyFrame {
            struct_size: core::mem::size_of::<sys::ReplyFrame>() as u32,
            bytes: if bytes.is_empty() {
                0
            } else {
                bytes.as_ptr() as u64
            },
            byte_count: bytes.len() as u64,
            resources: if dispositions.is_empty() {
                0
            } else {
                dispositions.as_ptr() as u64
            },
            resource_count: dispositions.len() as u64,
            ..sys::ReplyFrame::default()
        };
        let status = unsafe { (ops().responder_reply)(raw, &frame) };
        if status != sys::STATUS_OK {
            // Keep ownership so Drop still closes the responder.
            self.0 = unsafe { OwnedHandle::from_raw(raw) };
        }
        status_result(status)
    }

    /// Deliver a typed failure and consume the responder.
    pub fn fail(&mut self, failure: FailInvocation) -> Result<(), sys::Status> {
        let raw = core::mem::replace(&mut self.0, OwnedHandle::invalid()).into_raw();
        let frame = sys::FailFrame {
            struct_size: core::mem::size_of::<sys::FailFrame>() as u32,
            execution_outcome: failure.execution,
            outcome_reason: failure.reason,
            protocol_error: failure.protocol_error,
            ..sys::FailFrame::default()
        };
        let status = unsafe { (ops().responder_fail)(raw, &frame) };
        if status != sys::STATUS_OK {
            self.0 = unsafe { OwnedHandle::from_raw(raw) };
        }
        status_result(status)
    }
}

impl ReplySink for ResponderHandle {
    fn reply(
        &mut self,
        bytes: &[u8],
        dispositions: &[sys::ResourceDisposition],
    ) -> Result<(), sys::Status> {
        ResponderHandle::reply(self, bytes, dispositions)
    }

    fn fail(&mut self, failure: FailInvocation) -> Result<(), sys::Status> {
        ResponderHandle::fail(self, failure)
    }
}

/// Sink over an optional responder: oneway invocations arrive without one.
///
/// Using the sink while no responder exists reports `STATUS_INVALID_HANDLE`,
/// which keeps "reply attempted but impossible" observable instead of silent.
pub struct OptionalResponder(Option<ResponderHandle>);

impl From<Option<ResponderHandle>> for OptionalResponder {
    fn from(responder: Option<ResponderHandle>) -> Self {
        Self(responder)
    }
}

impl OptionalResponder {
    pub fn none() -> Self {
        Self(None)
    }
}

impl ReplySink for OptionalResponder {
    fn reply(
        &mut self,
        bytes: &[u8],
        dispositions: &[sys::ResourceDisposition],
    ) -> Result<(), sys::Status> {
        match self.0.as_mut() {
            Some(responder) => responder.reply(bytes, dispositions),
            None => Err(sys::STATUS_INVALID_HANDLE),
        }
    }

    fn fail(&mut self, failure: FailInvocation) -> Result<(), sys::Status> {
        match self.0.as_mut() {
            Some(responder) => responder.fail(failure),
            None => Err(sys::STATUS_INVALID_HANDLE),
        }
    }
}

/// What a single dispatch step did with a received request.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum DispatchOutcome {
    /// A reply or typed failure was delivered through the sink.
    Completed,
    /// An oneway invocation was accepted; failures are best-effort only.
    Accepted,
    /// The request violated the protocol (malformed wire, bad resource
    /// disposition, or unknown/reserved method id).  A `NOT_DELIVERED` fail
    /// frame was sent; the caller SHOULD close the server endpoint, mirroring
    /// the C++ dispatcher.
    Rejected,
}

/// Send the canonical protocol-violation failure and report rejection.
pub fn reject_protocol_violation(sink: &mut dyn ReplySink) -> Result<DispatchOutcome, CallError> {
    sink.fail(FailInvocation::protocol_violation())
        .map(|_| DispatchOutcome::Rejected)
        .map_err(CallError::Status)
}

/// Send the canonical unsupported-method failure and report rejection.
pub fn reject_unsupported(sink: &mut dyn ReplySink) -> Result<DispatchOutcome, CallError> {
    sink.fail(FailInvocation::unsupported())
        .map(|_| DispatchOutcome::Rejected)
        .map_err(CallError::Status)
}

/// Handler response plus the resources transferred with it.
///
/// `resources` entries must be referenced by the `ResourceSlot`s embedded in
/// `response`; the generated dispatcher validates the pairing before sending.
pub struct MethodReply<'a, R> {
    pub response: R,
    pub resources: ResourceTable<'a>,
}

impl<'a, R> MethodReply<'a, R> {
    pub fn new(response: R) -> Self {
        Self {
            response,
            resources: ResourceTable::new(),
        }
    }

    pub fn with_resources(response: R, resources: ResourceTable<'a>) -> Self {
        Self {
            response,
            resources,
        }
    }
}

/// A request received on a protocol server endpoint, before method routing.
pub struct IncomingRequest<'w> {
    pub method_id: u64,
    pub caller_pid: u64,
    /// `None` for oneway invocations.
    pub responder: Option<ResponderHandle>,
    /// Handles the kernel installed for this request; unclaimed handles close
    /// on drop.
    pub resources: ReceivedResources,
    /// Request payload, borrowed from the buffer passed to
    /// [`receive_request`].
    pub wire: &'w [u8],
}

/// Receive the next invocation from a protocol server endpoint without
/// committing to a method.
///
/// This is the generic counterpart of the per-method `receive_{method}`
/// functions: it does not check the method id, so a serve loop can route
/// unknown ids itself (or hand them to a generated `dispatch`).  Unlike the
/// per-method variants it never fails on unexpected method ids.
pub fn receive_request<'w>(
    endpoint: &ProtocolServerEndpoint,
    wire: &'w mut [u8],
) -> Result<IncomingRequest<'w>, CallError> {
    let mut raw_resources = [sys::HANDLE_INVALID; MAX_RESOURCES];
    let mut frame = sys::ChannelReceiveFrame {
        struct_size: core::mem::size_of::<sys::ChannelReceiveFrame>() as u32,
        bytes: if wire.is_empty() {
            0
        } else {
            wire.as_mut_ptr() as u64
        },
        byte_capacity: wire.len() as u64,
        resources: raw_resources.as_mut_ptr() as u64,
        resource_capacity: MAX_RESOURCES as u64,
        ..sys::ChannelReceiveFrame::default()
    };
    let status = unsafe { (ops().channel_receive)(endpoint.get(), &mut frame) };
    if status != sys::STATUS_OK {
        return Err(CallError::Status(status));
    }
    let mut raw_resource_guard = RawHandleGuard::new(&raw_resources, MAX_RESOURCES)
        .ok_or(CallError::Codec(CodecError::BoundExceeded))?;
    if frame.actual_resources > MAX_RESOURCES as u64 {
        return Err(CallError::Codec(CodecError::BoundExceeded));
    }
    let resources = match unsafe {
        ReceivedResources::from_raw(&raw_resources[..frame.actual_resources as usize])
    } {
        Ok(resources) => {
            raw_resource_guard.disarm();
            resources
        }
        Err(error) => {
            raw_resource_guard.disarm();
            return Err(CallError::Resource(error));
        }
    };
    if frame.actual_bytes > wire.len() as u64 {
        return Err(CallError::Codec(CodecError::BoundExceeded));
    }
    let responder = if frame.responder == sys::HANDLE_INVALID {
        None
    } else {
        // SAFETY: the kernel handed us sole ownership of this responder.
        Some(unsafe { ResponderHandle::from_raw(frame.responder) })
    };
    Ok(IncomingRequest {
        method_id: frame.method_id,
        caller_pid: frame.caller_pid,
        responder,
        resources,
        wire: &wire[..frame.actual_bytes as usize],
    })
}

/// Create a protocol endpoint pair from an explicit descriptor.
///
/// Generalization of the generated per-protocol `create_endpoints`: private
/// protocols whose bindings are not part of the public SDK (VFS
/// BLOCK_DEVICE_ADR §6.4) build their descriptor by hand and still need the
/// kernel-indirected creation path so in-process loopback tests work.
pub fn create_endpoints_from_descriptor(
    descriptor: &sys::ProtocolDescriptor,
) -> Result<(crate::ProtocolClientEndpoint, crate::ProtocolServerEndpoint), CallError> {
    let mut raw_descriptor = sys::HANDLE_INVALID;
    // SAFETY: `descriptor` outlives the call; the kernel copies it.
    let status = unsafe {
        (crate::kernel_ops::ops().protocol_descriptor_create)(descriptor, &mut raw_descriptor)
    };
    if status != sys::STATUS_OK {
        return Err(CallError::Status(status));
    }
    if raw_descriptor == sys::HANDLE_INVALID {
        return Err(CallError::InvalidHandle);
    }
    let descriptor = unsafe { OwnedHandle::from_raw(raw_descriptor) };
    let mut raw_client = sys::HANDLE_INVALID;
    let mut raw_server = sys::HANDLE_INVALID;
    // SAFETY: null options mean protocol defaults.
    let status = unsafe {
        (crate::kernel_ops::ops().protocol_endpoint_create)(
            descriptor.get(),
            core::ptr::null(),
            &mut raw_client,
            &mut raw_server,
        )
    };
    if status != sys::STATUS_OK {
        return Err(CallError::Status(status));
    }
    if raw_client == sys::HANDLE_INVALID || raw_server == sys::HANDLE_INVALID {
        if raw_client != sys::HANDLE_INVALID {
            let _ = unsafe { (crate::kernel_ops::ops().handle_close)(raw_client) };
        }
        if raw_server != sys::HANDLE_INVALID {
            let _ = unsafe { (crate::kernel_ops::ops().handle_close)(raw_server) };
        }
        return Err(CallError::InvalidHandle);
    }
    Ok((
        // SAFETY: freshly minted, uniquely owned handles.
        unsafe { crate::ProtocolClientEndpoint::from_raw(raw_client) },
        unsafe { crate::ProtocolServerEndpoint::from_raw(raw_server) },
    ))
}

/// Reject a received request, delivering the canonical typed failure when a
/// responder capability exists.
///
/// Oneway arrivals carry no responder, so their rejection is silent (the
/// delivery contract is best-effort); the received resource handles close
/// when `incoming` is dropped.
pub fn reject_request(
    incoming: IncomingRequest<'_>,
    unsupported: bool,
) -> Result<DispatchOutcome, CallError> {
    let mut sink = OptionalResponder::from(incoming.responder);
    if unsupported {
        reject_unsupported(&mut sink)
    } else {
        reject_protocol_violation(&mut sink)
    }
}

/// Default per-endpoint execution model: one serial serve loop.
///
/// `serve_requests` pulls invocations off `endpoint` one at a time and hands
/// each to `handle` together with the reply scratch buffer.  Every dispatch
/// outcome, including [`DispatchOutcome::Rejected`], keeps the loop alive;
/// the loop returns only when the underlying channel receive fails (peer
/// closed, object revoked, and so on), so the server decides shutdown.
///
/// Methods annotated `@concurrent` declare that their handler tolerates
/// re-entrant invocation.  This loop still executes them serially today;
/// multiplexing concurrent dispatchers over one endpoint is future work
/// layered directly on [`receive_request`] plus the generated per-protocol
/// `dispatch` functions, both of which are loop-free by design.
pub fn serve_requests<F>(
    endpoint: &ProtocolServerEndpoint,
    wire: &mut [u8],
    reply_wire: &mut [u8],
    mut handle: F,
) -> Result<(), CallError>
where
    F: FnMut(IncomingRequest<'_>, &mut [u8]) -> Result<DispatchOutcome, CallError>,
{
    loop {
        let incoming = receive_request(endpoint, wire)?;
        handle(incoming, reply_wire)?;
    }
}
