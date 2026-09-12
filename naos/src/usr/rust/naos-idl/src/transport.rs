//! Platform-neutral RPC contract used by generated bindings and service
//! adapters.
//!
//! This module deliberately contains no syscall, file-descriptor, or runtime
//! types.  A transport owns the platform-specific endpoint and implements the
//! asynchronous invocation boundary; generated protocol code only supplies
//! the UUID/revision/method and already encoded payload.

use core::future::Future;

use crate::CodecError;

/// Direction of a bulk region from the point of view of the invocation.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(u8)]
pub enum BulkDirection {
    In = 1,
    Out = 2,
    InOut = 3,
}

/// A descriptor for a separately shared data region.
///
/// `region_id` is opaque to the wire codec.  `offset` and `length` are
/// validated by both transport admission and the service before access.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct BulkBuffer {
    pub region_id: u64,
    pub offset: u64,
    pub length: u64,
    pub generation: u64,
    pub direction: BulkDirection,
    /// Transport-local rights.  The codec never interprets these bits.
    pub rights: u64,
}

impl BulkBuffer {
    pub const fn new(
        region_id: u64,
        offset: u64,
        length: u64,
        generation: u64,
        direction: BulkDirection,
        rights: u64,
    ) -> Self {
        Self {
            region_id,
            offset,
            length,
            generation,
            direction,
            rights,
        }
    }

    /// Validate arithmetic before a transport or backend touches a region.
    pub const fn checked_end(self) -> Option<u64> {
        self.offset.checked_add(self.length)
    }

    pub const fn is_valid(self) -> bool {
        self.checked_end().is_some() && self.generation != 0 && self.region_id != 0
    }
}

/// An opaque capability/resource descriptor.  Linux transports use an opaque
/// process-local id; NaOS transports map it to a capability disposition.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct ResourceDescriptor {
    pub resource_id: u64,
    pub binding: u32,
    pub scope: u64,
    pub rights: u64,
    /// Absolute range in the storage identity for a MemoryObject resource.
    /// Non-memory resources leave both fields zero.
    pub view_offset: u64,
    pub view_length: u64,
}

/// A fully described request handed to a platform transport.
#[derive(Clone, Copy, Debug)]
pub struct RpcRequest<'a> {
    pub protocol_uuid: [u8; 16],
    pub revision: u64,
    pub method_id: u64,
    pub request_id: u64,
    /// Transport-level authority for the endpoint invocation. NaOS carries
    /// this capability in the channel target; Linux transports may encode an
    /// opaque descriptor beside the method resources.
    pub target: Option<ResourceDescriptor>,
    pub payload: &'a [u8],
    pub resources: &'a [ResourceDescriptor],
    pub bulk: &'a [BulkBuffer],
}

/// Owned request used by service runners.  The type is available only with
/// the crate's `alloc` feature, but remains platform-neutral: a UDS runner,
/// a kernel-channel runner, and an in-process test runner can all produce it.
#[cfg(feature = "alloc")]
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct RpcRequestOwned {
    pub protocol_uuid: [u8; 16],
    pub revision: u64,
    pub method_id: u64,
    pub request_id: u64,
    pub target: Option<ResourceDescriptor>,
    pub payload: alloc::vec::Vec<u8>,
    pub resources: alloc::vec::Vec<ResourceDescriptor>,
    pub bulk: alloc::vec::Vec<BulkBuffer>,
}

#[cfg(feature = "alloc")]
impl RpcRequestOwned {
    pub fn as_request(&self) -> RpcRequest<'_> {
        RpcRequest {
            protocol_uuid: self.protocol_uuid,
            revision: self.revision,
            method_id: self.method_id,
            request_id: self.request_id,
            target: self.target,
            payload: &self.payload,
            resources: &self.resources,
            bulk: &self.bulk,
        }
    }
}

/// Transport-independent outcome attached to every response.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct RpcOutcome {
    pub execution: u32,
    pub reason: u32,
    pub protocol_error: i64,
}

impl RpcOutcome {
    pub const SUCCESS: Self = Self {
        execution: 0,
        reason: 0,
        protocol_error: 0,
    };
}

/// Response returned by a transport.  The response representation is owned
/// by the transport implementation, so this core contract stays `no_std`.
pub trait RpcResponse {
    fn protocol_uuid(&self) -> [u8; 16];
    fn revision(&self) -> u64;
    fn method_id(&self) -> u64;
    fn request_id(&self) -> u64;
    fn outcome(&self) -> RpcOutcome;
    fn payload(&self) -> &[u8];
    fn resources(&self) -> &[ResourceDescriptor];
    fn bulk(&self) -> &[BulkBuffer];
}

/// Error returned by generated transport-aware client helpers.  Codec errors
/// are rejected before admission; transport errors are kept opaque to the
/// generated protocol binding.
#[derive(Debug)]
pub enum TransportCallError<E> {
    Codec(CodecError),
    Transport(E),
}

/// Owned response exchanged between a platform runner and a service.  Its
/// fields deliberately match [`RpcRequestOwned`] and contain no Linux/NaOS
/// handle types.
#[cfg(feature = "alloc")]
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct RpcResponseOwned {
    pub protocol_uuid: [u8; 16],
    pub revision: u64,
    pub method_id: u64,
    pub request_id: u64,
    /// Present only when this owned frame is used as an internal decoded
    /// request envelope. Responses generated by services leave it `None`.
    pub target: Option<ResourceDescriptor>,
    pub outcome: RpcOutcome,
    pub payload: alloc::vec::Vec<u8>,
    pub resources: alloc::vec::Vec<ResourceDescriptor>,
    pub bulk: alloc::vec::Vec<BulkBuffer>,
}

#[cfg(feature = "alloc")]
impl RpcResponseOwned {
    pub fn success(request: &RpcRequestOwned, payload: alloc::vec::Vec<u8>) -> Self {
        Self {
            protocol_uuid: request.protocol_uuid,
            revision: request.revision,
            method_id: request.method_id,
            request_id: request.request_id,
            target: None,
            outcome: RpcOutcome::SUCCESS,
            payload,
            resources: alloc::vec::Vec::new(),
            bulk: alloc::vec::Vec::new(),
        }
    }

    pub fn domain_error(request: &RpcRequestOwned, protocol_error: i64) -> Self {
        let mut response = Self::success(request, alloc::vec::Vec::new());
        response.outcome.protocol_error = protocol_error;
        response
    }
}

#[cfg(feature = "alloc")]
impl RpcResponse for RpcResponseOwned {
    fn protocol_uuid(&self) -> [u8; 16] {
        self.protocol_uuid
    }

    fn revision(&self) -> u64 {
        self.revision
    }

    fn method_id(&self) -> u64 {
        self.method_id
    }

    fn request_id(&self) -> u64 {
        self.request_id
    }

    fn outcome(&self) -> RpcOutcome {
        self.outcome
    }

    fn payload(&self) -> &[u8] {
        &self.payload
    }

    fn resources(&self) -> &[ResourceDescriptor] {
        &self.resources
    }

    fn bulk(&self) -> &[BulkBuffer] {
        &self.bulk
    }
}

/// The only platform boundary visible to shared daemon/service code.
///
/// The associated future lets NaOS use its own executor while Linux uses
/// Tokio.  No `async-trait`, `tokio`, Unix fd, or NaOS handle leaks into this
/// interface.
pub trait RpcTransport {
    type Endpoint: Clone;
    type Error;
    type Response: RpcResponse;
    type Invoke<'a>: Future<Output = Result<Self::Response, Self::Error>>
    where
        Self: 'a;

    fn invoke<'a>(
        &'a self,
        endpoint: &'a Self::Endpoint,
        request: RpcRequest<'a>,
    ) -> Self::Invoke<'a>;
}

/// Platform-neutral service discovery boundary.
///
/// NaOS implements this with the generated `ServiceDirectory` protocol;
/// Linux resolves the same URI into a deterministic local UDS endpoint.  The
/// daemon never needs to know which mechanism produced the endpoint.
pub trait ServiceLocator {
    type Endpoint: Clone;
    type Error;
    type Resolve<'a>: Future<Output = Result<Self::Endpoint, Self::Error>>
    where
        Self: 'a;
    type List<'a>: Future<Output = Result<usize, Self::Error>>
    where
        Self: 'a;

    fn resolve<'a>(&'a self, uri: &'a str) -> Self::Resolve<'a>;
    fn list<'a>(&'a self, prefix: &'a str, endpoints: &'a mut [Self::Endpoint]) -> Self::List<'a>;
}

/// Platform-neutral asynchronous service hook.  A Linux server can run this
/// future from Tokio; a NaOS runner can poll the same service core from its
/// native runtime.
pub trait RpcService {
    type Error;
    type Response: RpcResponse;
    type Dispatch<'a>: Future<Output = Result<Self::Response, Self::Error>>
    where
        Self: 'a;

    fn dispatch<'a>(&'a mut self, request: RpcRequest<'a>) -> Self::Dispatch<'a>;
}
