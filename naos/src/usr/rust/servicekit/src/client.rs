//! Platform-neutral protocol client facade.
//!
//! Generated IDL bindings describe the wire schema. This module owns the
//! transport-specific part: UDS on Linux and capability invocations on NaOS.
//! Daemons use the same request/response and resource ownership API on both.

use alloc::vec::Vec;
use naos_idl::ResourceSlot;

/// Remove a received resource without changing the indices of later wire
/// slots.  Response resource indices are part of the encoded protocol reply;
/// `Vec::remove` would shift slot 1 into slot 0 after slot 0 is consumed.
fn take_resource_slot<T>(resources: &mut Vec<Option<T>>, slot: ResourceSlot) -> Option<T> {
    resources.get_mut(slot.index() as usize)?.take()
}

#[cfg(target_os = "linux")]
mod platform {
    use super::super::linux::{LinuxEndpoint, LinuxUdsTransport, TransportError};
    use crate::memory::MemoryObject;
    use crate::server::ServiceDirectory;
    use naos_idl::ResourceSlot;
    use naos_idl::transport::{
        BulkBuffer, BulkDirection, ResourceDescriptor, RpcRequest, RpcResponseOwned,
    };
    use naos_sys as sys;
    use std::sync::Arc;
    use std::vec;
    use std::vec::Vec;

    #[derive(Clone, Debug, Eq, PartialEq)]
    pub enum ClientError {
        Io,
        PeerClosed,
        Protocol,
        Status(sys::Status),
    }

    struct ClientResourceInner {
        descriptor: ResourceDescriptor,
        endpoint: LinuxEndpoint,
    }

    impl Drop for ClientResourceInner {
        fn drop(&mut self) {
            crate::linux::release_resource(
                self.endpoint.path(),
                self.descriptor.resource_id,
            );
        }
    }

    pub struct ClientResource {
        inner: Arc<ClientResourceInner>,
    }

    impl ClientResource {
        pub fn descriptor(&self) -> ResourceDescriptor {
            self.inner.descriptor
        }
    }

    struct MemoryTransfer {
        pub resource: ClientResource,
        pub bulk: Vec<BulkBuffer>,
    }

    pub struct Response {
        pub protocol_uuid: [u8; 16],
        pub revision: u64,
        pub method_id: u64,
        pub request_id: u64,
        pub protocol_error: i64,
        pub execution: u32,
        pub reason: u32,
        pub payload: Vec<u8>,
        pub resources: Vec<Option<ClientResource>>,
    }

    impl Response {
        pub fn take_resource(&mut self, slot: ResourceSlot) -> Option<ClientResource> {
            super::take_resource_slot(&mut self.resources, slot)
        }

        pub fn is_success(&self) -> bool {
            self.protocol_error == 0 && self.execution == 0 && self.reason == 0
        }
    }

    #[derive(Clone)]
    pub struct Client {
        transport: LinuxUdsTransport,
        endpoint: LinuxEndpoint,
        protocol_uuid: [u8; 16],
        revision: u64,
        // Keeps a dynamic endpoint lease alive when this client is built from
        // a received resource. The transport endpoint itself is stored above;
        // this field exists solely for the lease's Drop implementation.
        #[allow(dead_code)]
        resource: Option<Arc<ClientResourceInner>>,
    }

    impl Client {
        pub async fn connect(
            directory: &ServiceDirectory,
            uri: &str,
            descriptor: &sys::ProtocolDescriptor,
        ) -> Result<Self, ClientError> {
            let endpoint = directory
                .endpoint_for(uri)
                .map_err(|_| ClientError::Protocol)?;
            Ok(Self {
                transport: LinuxUdsTransport::new(),
                endpoint,
                protocol_uuid: descriptor.uuid.bytes,
                revision: descriptor.revision,
                resource: None,
            })
        }

        pub fn from_resource(
            resource: ClientResource,
            descriptor: &sys::ProtocolDescriptor,
        ) -> Result<Self, ClientError> {
            let resource_descriptor = resource.descriptor();
            if resource_descriptor.rights & sys::RIGHT_TRANSFER == 0 {
                return Err(ClientError::Status(sys::STATUS_ACCESS_DENIED));
            }
            let inner = resource.inner;
            Ok(Self {
                transport: LinuxUdsTransport::new(),
                endpoint: inner.endpoint.with_target(resource_descriptor),
                protocol_uuid: descriptor.uuid.bytes,
                revision: descriptor.revision,
                resource: Some(inner),
            })
        }

        /// Build the descriptor transfer for one region.
        ///
        /// The resource carries the descriptor bits and the bulk entry carries
        /// the window; both are admitted through the same shared contract the
        /// NaOS capability path uses, so equal requests get equal status codes.
        fn memory_transfer(
            &self,
            memory: &MemoryObject,
            offset: u64,
            length: u64,
            direction: crate::memory::MemoryDirection,
            rights: u64,
        ) -> Result<MemoryTransfer, ClientError> {
            crate::memory::check_direction_rights(direction, rights, offset, length, memory.size())
                .map_err(|error| match error {
                    crate::memory::MemoryError::AccessDenied => {
                        ClientError::Status(sys::STATUS_ACCESS_DENIED)
                    }
                    _ => ClientError::Protocol,
                })?;
            let direction = match direction {
                crate::memory::MemoryDirection::In => BulkDirection::In,
                crate::memory::MemoryDirection::Out => BulkDirection::Out,
                crate::memory::MemoryDirection::InOut => BulkDirection::InOut,
            };
            let view = memory
                .subspan(offset, length)
                .map_err(|_| ClientError::Status(sys::STATUS_ACCESS_DENIED))?;
            view.register(&self.endpoint).map_err(|_| ClientError::Io)?;
            let descriptor = view
                .descriptor(0, length, direction, rights)
                .map_err(|_| ClientError::Protocol)?;
            Ok(MemoryTransfer {
                resource: ClientResource {
                    inner: Arc::new(ClientResourceInner {
                        descriptor: ResourceDescriptor {
                            resource_id: descriptor.region_id,
                            binding: sys::BINDING_MEMORY_OBJECT,
                            scope: naos_idl::memory_object::PROTOCOL_SCOPE,
                            rights,
                            view_offset: descriptor.offset,
                            view_length: descriptor.length,
                            ..ResourceDescriptor::default()
                        },
                        endpoint: self.endpoint.without_target(),
                    }),
                },
                bulk: vec![descriptor],
            })
        }

        pub fn invoke_memory_blocking(
            &self,
            method_id: u64,
            payload: &[u8],
            memory: &MemoryObject,
            offset: u64,
            length: u64,
            direction: crate::memory::MemoryDirection,
            rights: u64,
        ) -> Result<Response, ClientError> {
            let transfer = self.memory_transfer(memory, offset, length, direction, rights)?;
            self.invoke_blocking(
                method_id,
                payload,
                std::slice::from_ref(&transfer.resource),
                &transfer.bulk,
            )
        }

        pub async fn invoke(
            &self,
            method_id: u64,
            payload: &[u8],
            resources: &[ClientResource],
            bulk: &[BulkBuffer],
        ) -> Result<Response, ClientError> {
            let descriptors: Vec<_> = resources.iter().map(ClientResource::descriptor).collect();
            if descriptors
                .iter()
                .any(|resource| resource.rights & sys::RIGHT_TRANSFER == 0)
            {
                return Err(ClientError::Status(sys::STATUS_ACCESS_DENIED));
            }
            let request = RpcRequest {
                protocol_uuid: self.protocol_uuid,
                revision: self.revision,
                method_id,
                request_id: 0,
                target: None,
                payload,
                resources: &descriptors,
                bulk,
            };
            let response = self
                .transport
                .invoke_async(&self.endpoint, request)
                .await
                .map_err(map_error)?;
            Ok(response_from_owned(response, &self.endpoint))
        }

        pub fn invoke_blocking(
            &self,
            method_id: u64,
            payload: &[u8],
            resources: &[ClientResource],
            bulk: &[BulkBuffer],
        ) -> Result<Response, ClientError> {
            let descriptors: Vec<_> = resources.iter().map(ClientResource::descriptor).collect();
            if descriptors
                .iter()
                .any(|resource| resource.rights & sys::RIGHT_TRANSFER == 0)
            {
                return Err(ClientError::Status(sys::STATUS_ACCESS_DENIED));
            }
            let request = RpcRequest {
                protocol_uuid: self.protocol_uuid,
                revision: self.revision,
                method_id,
                request_id: 0,
                target: None,
                payload,
                resources: &descriptors,
                bulk,
            };
            let response = self
                .transport
                .invoke_blocking(&self.endpoint, request)
                .map_err(map_error)?;
            Ok(response_from_owned(response, &self.endpoint))
        }
    }

    fn map_error(error: TransportError) -> ClientError {
        match error {
            TransportError::Io => ClientError::Io,
            TransportError::PeerClosed => ClientError::PeerClosed,
            TransportError::Protocol | TransportError::FrameTooLarge | TransportError::Bulk => {
                ClientError::Protocol
            }
        }
    }

    fn response_from_owned(response: RpcResponseOwned, endpoint: &LinuxEndpoint) -> Response {
        Response {
            protocol_uuid: response.protocol_uuid,
            revision: response.revision,
            method_id: response.method_id,
            request_id: response.request_id,
            protocol_error: response.outcome.protocol_error,
            execution: response.outcome.execution,
            reason: response.outcome.reason,
            payload: response.payload,
            resources: response
                .resources
                .into_iter()
                .map(|descriptor| {
                    Some(ClientResource {
                        inner: Arc::new(ClientResourceInner {
                            descriptor,
                            endpoint: endpoint.without_target(),
                        }),
                    })
                })
                .collect(),
        }
    }
}

#[cfg(target_os = "naos")]
mod platform {
    use crate::naos;
    use crate::server::ServiceDirectory;
    use crate::sys;
    use alloc::vec::Vec;
    use core::cell::RefCell;
    use core::time::Duration;
    use naos_idl::transport::{BulkBuffer, BulkDirection, ResourceDescriptor};
    use naos_idl::{CallError, OwnedHandle, ProtocolClientEndpoint, ResourceSlot, ResourceTable};

    #[derive(Clone, Copy, Debug, Eq, PartialEq)]
    pub enum ClientError {
        Io,
        PeerClosed,
        Protocol,
        Status(sys::Status),
    }

    pub struct ClientResource {
        descriptor: ResourceDescriptor,
        handle: Option<OwnedHandle>,
    }

    impl ClientResource {
        pub fn descriptor(&self) -> ResourceDescriptor {
            self.descriptor
        }

        /// Consume a received NaOS resource when the caller owns the
        /// endpoint role represented by its descriptor. Linux UDS descriptors
        /// are intentionally not exposed through this API because they are
        /// not kernel handles or server endpoints.
        pub fn into_owned_handle(mut self) -> Result<OwnedHandle, ClientError> {
            self.handle.take().ok_or(ClientError::Protocol)
        }

        fn handle(&self) -> Option<&OwnedHandle> {
            self.handle.as_ref()
        }
    }

    struct MemoryTransfer {
        pub resource: ClientResource,
        pub bulk: Vec<BulkBuffer>,
    }

    pub struct Response {
        pub protocol_uuid: [u8; 16],
        pub revision: u64,
        pub method_id: u64,
        pub request_id: u64,
        pub protocol_error: i64,
        pub execution: u32,
        pub reason: u32,
        pub payload: Vec<u8>,
        pub resources: Vec<Option<ClientResource>>,
    }

    impl Response {
        pub fn take_resource(&mut self, slot: ResourceSlot) -> Option<ClientResource> {
            super::take_resource_slot(&mut self.resources, slot)
        }

        pub fn is_success(&self) -> bool {
            self.protocol_error == 0 && self.execution == 0 && self.reason == 0
        }
    }

    #[derive(Clone)]
    pub struct Client {
        endpoint: alloc::rc::Rc<ProtocolClientEndpoint>,
        protocol_uuid: [u8; 16],
        revision: u64,
        /// Byte capacity of one response envelope, from the descriptor the
        /// caller connected with.  Bulk payloads travel in the caller's
        /// region, so the reply is only a header plus small inline fields.
        response_bytes: usize,
        /// Result wire reused by every invocation of this client.  Sizing it
        /// once from the descriptor's declared response bound keeps a fixed
        /// scratch out of the per-call path; `Response` copies the reply
        /// bytes out because its payload is owned.
        wire: alloc::rc::Rc<RefCell<Vec<u8>>>,
    }

    impl Client {
        pub async fn connect(
            directory: &ServiceDirectory,
            uri: &str,
            descriptor: &sys::ProtocolDescriptor,
        ) -> Result<Self, ClientError> {
            let endpoint = naos::connect_until(
                directory.raw(),
                uri,
                descriptor.uuid.bytes,
                descriptor.protocol_rights,
                descriptor.revision,
                descriptor.features,
                Duration::from_secs(30),
            )
            .await
            .map_err(map_call_error)?;
            Ok(Self {
                endpoint: alloc::rc::Rc::new(endpoint),
                protocol_uuid: descriptor.uuid.bytes,
                revision: descriptor.revision,
                response_bytes: response_bytes(descriptor),
                wire: alloc::rc::Rc::new(RefCell::new(Vec::new())),
            })
        }

        pub fn from_resource(
            resource: ClientResource,
            descriptor: &sys::ProtocolDescriptor,
        ) -> Result<Self, ClientError> {
            if resource.descriptor.rights & sys::RIGHT_TRANSFER == 0 {
                return Err(ClientError::Status(sys::STATUS_ACCESS_DENIED));
            }
            let handle = resource.handle.ok_or(ClientError::Protocol)?;
            Ok(Self {
                endpoint: alloc::rc::Rc::new(unsafe {
                    ProtocolClientEndpoint::from_raw(handle.into_raw())
                }),
                protocol_uuid: descriptor.uuid.bytes,
                revision: descriptor.revision,
                response_bytes: response_bytes(descriptor),
                wire: alloc::rc::Rc::new(RefCell::new(Vec::new())),
            })
        }

        /// Build the capability transfer for one region.
        ///
        /// Both transports turn the requested range into a bounded
        /// MemoryView. NaOS transfers that restricted capability; Linux puts
        /// the same absolute range into its authoritative descriptor. The
        /// direction decides which `MEMORY_RIGHT_*` set must be carried, so a
        /// caller that asks to read a region it only holds WRITE for is
        /// rejected here instead of at the service boundary.
        fn memory_transfer(
            &self,
            memory: &crate::memory::MemoryObject,
            offset: u64,
            length: u64,
            direction: crate::memory::MemoryDirection,
            rights: u64,
        ) -> Result<MemoryTransfer, ClientError> {
            crate::memory::check_direction_rights(direction, rights, offset, length, memory.size())
                .map_err(|_| ClientError::Status(sys::STATUS_ACCESS_DENIED))?;
            let view = memory
                .subspan(offset, length)
                .map_err(|_| ClientError::Status(sys::STATUS_ACCESS_DENIED))?;
            let handle = view.into_handle();
            let resource_id = naos_idl::object_id(handle.get()).map_err(map_call_error)?;
            let info = naos_idl::handle_info(handle.get()).map_err(map_call_error)?;
            Ok(MemoryTransfer {
                resource: ClientResource {
                    descriptor: ResourceDescriptor {
                        resource_id,
                        binding: sys::BINDING_MEMORY_OBJECT,
                        scope: naos_idl::memory_object::PROTOCOL_SCOPE,
                        rights,
                        view_offset: info.view_offset,
                        view_length: info.view_length,
                        ..ResourceDescriptor::default()
                    },
                    handle: Some(handle),
                },
                bulk: Vec::new(),
            })
        }

        pub fn invoke_memory_blocking(
            &self,
            method_id: u64,
            payload: &[u8],
            memory: &crate::memory::MemoryObject,
            offset: u64,
            length: u64,
            direction: crate::memory::MemoryDirection,
            rights: u64,
        ) -> Result<Response, ClientError> {
            let transfer = self.memory_transfer(memory, offset, length, direction, rights)?;
            self.invoke_blocking(
                method_id,
                payload,
                core::slice::from_ref(&transfer.resource),
                &transfer.bulk,
            )
        }

        /// Translate wire bulk descriptors onto the capability transfers.
        ///
        /// NaOS carries bulk as a transferred MemoryObject capability rather
        /// than as a separate descriptor, so each descriptor is admitted
        /// against the resource it names and then dropped from the wire: the
        /// capability is the authority.  Every check the Linux descriptor
        /// path performs on a descriptor is performed here, so a request that
        /// Linux rejects for rights, direction, identity or range is rejected
        /// by NaOS too.
        fn admit_bulk(
            &self,
            resources: &[ClientResource],
            bulk: &[BulkBuffer],
        ) -> Result<(), ClientError> {
            let mut matched = 0usize;
            for descriptor in bulk {
                if !descriptor.is_valid() {
                    return Err(ClientError::Protocol);
                }
                let mut found = None;
                for resource in resources {
                    if resource.descriptor.resource_id == descriptor.region_id {
                        found = Some(resource);
                        break;
                    }
                }
                let Some(resource) = found else {
                    return Err(ClientError::Protocol);
                };
                // The descriptor must describe the capability that is actually
                // transferred; a wire right set wider or narrower than the
                // resource is a contract violation, not an attenuated view.
                if resource.descriptor.rights != descriptor.rights {
                    return Err(ClientError::Status(sys::STATUS_ACCESS_DENIED));
                }
                if resource.descriptor.view_offset != descriptor.offset
                    || resource.descriptor.view_length != descriptor.length
                {
                    return Err(ClientError::Status(sys::STATUS_ACCESS_DENIED));
                }
                let direction = match descriptor.direction {
                    BulkDirection::In => crate::memory::MemoryDirection::In,
                    BulkDirection::Out => crate::memory::MemoryDirection::Out,
                    BulkDirection::InOut => crate::memory::MemoryDirection::InOut,
                };
                crate::memory::check_direction_rights(
                    direction,
                    descriptor.rights,
                    descriptor.offset,
                    descriptor.length,
                    None,
                )
                .map_err(|_| ClientError::Status(sys::STATUS_ACCESS_DENIED))?;
                matched += 1;
            }
            if matched != bulk.len() {
                return Err(ClientError::Protocol);
            }
            Ok(())
        }

        pub async fn invoke(
            &self,
            method_id: u64,
            payload: &[u8],
            resources: &[ClientResource],
            bulk: &[BulkBuffer],
        ) -> Result<Response, ClientError> {
            self.admit_bulk(resources, bulk)?;
            let mut table = ResourceTable::new();
            for resource in resources {
                if resource.descriptor.rights & sys::RIGHT_TRANSFER == 0 {
                    return Err(ClientError::Status(sys::STATUS_ACCESS_DENIED));
                }
                let handle = resource.handle().ok_or(ClientError::Protocol)?;
                table
                    .push_duplicate(handle)
                    .map_err(|_| ClientError::Status(sys::STATUS_RESOURCE_EXHAUSTED))?;
            }
            let mut invocation =
                naos_idl::submit_invocation(&self.endpoint, method_id, payload, table, 0)
                    .map_err(map_call_error)?;
            // An async client must not park the Tokio worker on the
            // synchronous epoll compatibility path. This is especially
            // important during mount commit: vfsd may synchronously call the
            // worker's MountControl while the worker is awaiting the ticket
            // result. The shared NaOS readiness adapter makes this wait
            // cancellable and keeps the selector available to other tasks.
            let ready = naos::wait_ready_async(&[invocation.get()], None)
                .await
                .map_err(ClientError::Status)?;
            if !ready.readable {
                return Err(ClientError::Io);
            }
            self.take_response(&mut invocation)
        }

        pub fn invoke_blocking(
            &self,
            method_id: u64,
            payload: &[u8],
            resources: &[ClientResource],
            bulk: &[BulkBuffer],
        ) -> Result<Response, ClientError> {
            self.admit_bulk(resources, bulk)?;
            let mut table = ResourceTable::new();
            for resource in resources {
                if resource.descriptor.rights & sys::RIGHT_TRANSFER == 0 {
                    return Err(ClientError::Status(sys::STATUS_ACCESS_DENIED));
                }
                let handle = resource.handle().ok_or(ClientError::Protocol)?;
                table
                    .push_duplicate(handle)
                    .map_err(|_| ClientError::Status(sys::STATUS_RESOURCE_EXHAUSTED))?;
            }
            let mut invocation =
                naos_idl::submit_invocation(&self.endpoint, method_id, payload, table, 0)
                    .map_err(map_call_error)?;
            if !naos::wait_for_completion(invocation.get(), u64::MAX) {
                return Err(ClientError::Io);
            }
            self.take_response(&mut invocation)
        }

        /// Take one invocation result and copy its envelope out of the
        /// reusable wire.
        ///
        /// Every migrated reply carries its bulk payload in the caller's
        /// region, so the inline reply is bounded by the descriptor's
        /// declared response size.  The wire grows to that bound on first use
        /// and is never shrunk; the exact reply bytes are copied out because
        /// `Response.payload` is owned by the caller.
        fn take_response(
            &self,
            invocation: &mut naos_idl::Invocation,
        ) -> Result<Response, ClientError> {
            let mut wire = self.wire.borrow_mut();
            if wire.len() < self.response_bytes {
                wire.resize(self.response_bytes, 0);
            }
            let mut result = naos_idl::take_invocation_result(invocation, wire.as_mut_slice())
                .map_err(map_call_error)?;
            let payload = wire[..result.bytes].to_vec();
            let mut response_resources = Vec::new();
            for index in 0..result.resources.len() {
                let slot = ResourceSlot::new(index as u32).ok_or(ClientError::Protocol)?;
                let handle = result.resources.take(slot).ok_or(ClientError::Protocol)?;
                let descriptor = descriptor_for(&handle);
                response_resources.push(Some(ClientResource {
                    descriptor,
                    handle: Some(handle),
                }));
            }
            Ok(Response {
                protocol_uuid: self.protocol_uuid,
                revision: self.revision,
                method_id: result.method_id,
                request_id: 0,
                protocol_error: result.protocol_error,
                execution: result.execution,
                reason: result.reason,
                payload,
                resources: response_resources,
            })
        }
    }

    /// Envelope capacity one client needs for a reply, taken from the
    /// descriptor's declared bound.  `usize` is 64-bit on both targets, so the
    /// conversion always succeeds; a zero bound would fail the take loudly
    /// instead of allocating an unbounded scratch.
    fn response_bytes(descriptor: &sys::ProtocolDescriptor) -> usize {
        usize::try_from(descriptor.max_response_bytes).unwrap_or(0)
    }

    fn descriptor_for(handle: &OwnedHandle) -> ResourceDescriptor {
        let mut info = sys::HandleInfo {
            struct_size: core::mem::size_of::<sys::HandleInfo>() as u32,
            ..sys::HandleInfo::default()
        };
        if unsafe { sys::_na_handle_get_info(handle.get(), &mut info) } == sys::STATUS_OK {
            ResourceDescriptor {
                resource_id: info.object_id,
                binding: info.binding,
                scope: info.scope,
                rights: info.meta_rights | info.protocol_rights,
                view_offset: info.view_offset,
                view_length: info.view_length,
            }
        } else {
            ResourceDescriptor {
                resource_id: handle.get(),
                binding: sys::BINDING_NONE,
                scope: 0,
                rights: sys::RIGHT_TRANSFER,
                ..ResourceDescriptor::default()
            }
        }
    }

    fn map_call_error(error: CallError) -> ClientError {
        match error {
            CallError::Status(status) => ClientError::Status(status),
            CallError::Outcome { protocol_error, .. } => {
                if protocol_error == 0 {
                    ClientError::Io
                } else {
                    ClientError::Protocol
                }
            }
            _ => ClientError::Protocol,
        }
    }
}

pub use platform::{Client, ClientError, ClientResource, Response};

#[cfg(test)]
mod tests {
    use super::take_resource_slot;
    use naos_idl::ResourceSlot;

    #[test]
    fn response_resource_slots_remain_stable_after_consumption() {
        let mut resources = vec![Some("control"), Some("ticket")];
        assert_eq!(
            take_resource_slot(&mut resources, ResourceSlot::new(0).unwrap()),
            Some("control")
        );
        assert_eq!(
            take_resource_slot(&mut resources, ResourceSlot::new(1).unwrap()),
            Some("ticket")
        );
        assert!(take_resource_slot(&mut resources, ResourceSlot::new(0).unwrap()).is_none());
    }
}
