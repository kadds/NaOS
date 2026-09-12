//! Platform-neutral service server facade.
//!
//! A daemon sees one request/response API here. Linux implements it with a
//! UDS connection and NaOS implements it with a capability channel and
//! generated protocol endpoints. The platform details intentionally stay in
//! this module instead of leaking into daemon crates.

#[cfg(target_os = "linux")]
mod platform {
    use super::super::linux::{
        self, LinuxResourceAuthority, LinuxServiceDirectory, LinuxServiceSession,
    };
    use crate::admission::{Admission, AdmissionPermit};
    use crate::memory;
    use naos_idl::ResourceSlot;
    use naos_idl::transport::{
        BulkBuffer, ResourceDescriptor, RpcOutcome, RpcRequestOwned, RpcResponseOwned,
    };
    use naos_sys as sys;
    use std::path::PathBuf;
    use std::sync::atomic::{AtomicU64, Ordering};
    use std::time::Duration;
    use std::vec::Vec;
    use tokio::io::AsyncWriteExt;
    use tokio::net::{UnixListener, UnixStream};

    pub type ServiceDirectory = LinuxServiceDirectory;

    #[derive(Clone, Debug, Eq, PartialEq)]
    pub enum ServerError {
        Io,
        PeerClosed,
        Protocol,
        Status(sys::Status),
    }

    pub struct EndpointResource {
        descriptor: ResourceDescriptor,
    }

    impl EndpointResource {
        pub fn descriptor(&self) -> ResourceDescriptor {
            self.descriptor
        }
    }

    /// Method-specific resource admission supplied by the generated/service
    /// binding.  The transport validates identity and lifetime separately.
    pub type RequestValidator = fn(&RpcRequestOwned) -> Result<(), i64>;

    pub struct Request {
        pub protocol_uuid: [u8; 16],
        pub revision: u64,
        pub method_id: u64,
        pub request_id: u64,
        pub target: Option<ResourceDescriptor>,
        pub payload: Vec<u8>,
        pub resources: Vec<ResourceDescriptor>,
        pub bulk: Vec<BulkBuffer>,
        stream: Option<UnixStream>,
        session: LinuxServiceSession,
        contract: ProtocolContract,
        /// Held until the request is answered or dropped, so a peer that
        /// disconnects mid-request returns its admission slot.
        _admission: Option<AdmissionPermit>,
    }

    impl Request {
        pub fn resource_count(&self) -> usize {
            self.resources.len()
        }

        pub fn read_memory(
            &self,
            slot: ResourceSlot,
            offset: u64,
            data: &mut [u8],
        ) -> Result<(), ServerError> {
            if !self.validate_memory(slot, true) {
                return Err(ServerError::Status(sys::STATUS_ACCESS_DENIED));
            }
            let descriptor = self.bulk.first().copied().ok_or(ServerError::Protocol)?;
            let resource = self
                .resources
                .iter()
                .find(|resource| resource.resource_id == descriptor.region_id)
                .ok_or(ServerError::Protocol)?;
            let absolute = resource
                .view_offset
                .checked_add(offset)
                .ok_or(ServerError::Protocol)?;
            if descriptor.offset != absolute
                || descriptor.length != data.len() as u64
                || offset > resource.view_length
                || descriptor.length > resource.view_length - offset
            {
                return Err(ServerError::Protocol);
            }
            memory::read_descriptor_for(
                self.session.bulk_endpoint(),
                self.session.peer_pid(),
                descriptor,
                data,
            )
            .map_err(|_| ServerError::Status(5))
        }

        pub fn write_memory(
            &self,
            slot: ResourceSlot,
            offset: u64,
            data: &[u8],
        ) -> Result<(), ServerError> {
            if !self.validate_memory(slot, false) {
                return Err(ServerError::Status(sys::STATUS_ACCESS_DENIED));
            }
            let descriptor = self.bulk.first().copied().ok_or(ServerError::Protocol)?;
            let resource = self
                .resources
                .iter()
                .find(|resource| resource.resource_id == descriptor.region_id)
                .ok_or(ServerError::Protocol)?;
            let absolute = resource
                .view_offset
                .checked_add(offset)
                .ok_or(ServerError::Protocol)?;
            if descriptor.offset != absolute
                || descriptor.length != data.len() as u64
                || offset > resource.view_length
                || descriptor.length > resource.view_length - offset
            {
                return Err(ServerError::Protocol);
            }
            memory::write_descriptor_for(
                self.session.bulk_endpoint(),
                self.session.peer_pid(),
                descriptor,
                data,
            )
            .map_err(|_| ServerError::Status(5))
        }

        /// Run a service-side read directly against the caller's MemoryObject
        /// data plane. Linux uses its private bulk backend to populate a
        /// temporary slice with the same semantics; NaOS maps the received
        /// object in place.
        pub fn with_read_memory<T>(
            &self,
            slot: ResourceSlot,
            offset: u64,
            length: usize,
            f: impl FnOnce(&[u8]) -> T,
        ) -> Result<T, ServerError> {
            let mut data = vec![0; length];
            self.read_memory(slot, offset, &mut data)?;
            Ok(f(&data))
        }

        /// Run a service-side write directly against the caller's MemoryObject
        /// data plane. The Linux adapter writes the resulting slice back to
        /// its registered bulk region after the callback returns.
        pub fn with_write_memory<T>(
            &self,
            slot: ResourceSlot,
            offset: u64,
            length: usize,
            f: impl FnOnce(&mut [u8]) -> Result<T, i64>,
        ) -> Result<Result<T, i64>, ServerError> {
            let mut data = vec![0; length];
            let result = f(&mut data);
            if result.is_ok() {
                self.write_memory(slot, offset, &data)?;
            }
            Ok(result)
        }

        pub fn validate_memory(&self, slot: ResourceSlot, write: bool) -> bool {
            let Some(descriptor) = self.bulk.first() else {
                return false;
            };
            if self.bulk.len() != 1 {
                return false;
            }
            let Some(resource) = self
                .resources
                .iter()
                .find(|resource| resource.resource_id == descriptor.region_id)
            else {
                return false;
            };
            // A normal request has only its memory resource.  Keep accepting
            // the legacy host envelope where a dynamic endpoint occupies slot
            // zero and the bulk region follows it; the region identity still
            // has to match exactly and the endpoint cannot be mistaken for a
            // memory object.
            let legacy_endpoint_prefix = slot.index() == 0
                && self.resources.len() == 2
                && self
                    .resources
                    .first()
                    .is_some_and(|value| value.binding != sys::BINDING_MEMORY_OBJECT);
            if self.resources.len() != 1 && !legacy_endpoint_prefix {
                return false;
            }
            let direction = match descriptor.direction {
                naos_idl::transport::BulkDirection::In => crate::memory::RegionDirection::In,
                naos_idl::transport::BulkDirection::Out => crate::memory::RegionDirection::Out,
                naos_idl::transport::BulkDirection::InOut => crate::memory::RegionDirection::InOut,
            };
            crate::memory::admit_service_region(
                write,
                resource.binding,
                resource.scope,
                resource.rights,
                Some(direction),
            )
            .is_ok()
                && resource.view_length != 0
                && resource.view_offset == descriptor.offset
                && descriptor.length <= resource.view_length
        }

        pub fn validate_resource(&self, resource: ResourceDescriptor) -> bool {
            self.session.validate_resource(resource)
        }
    }

    impl Drop for Request {
        fn drop(&mut self) {
            if !self.bulk.is_empty() {
                linux::bulk::release_for(
                    self.session.bulk_endpoint(),
                    self.session.peer_pid(),
                    &self.bulk,
                );
            }
            self.session.close();
        }
    }

    pub struct Response {
        protocol_uuid: [u8; 16],
        revision: u64,
        method_id: u64,
        request_id: u64,
        outcome: RpcOutcome,
        payload: Vec<u8>,
        resources: Vec<EndpointResource>,
    }

    impl Response {
        pub fn success(request: &Request, payload: Vec<u8>) -> Self {
            Self {
                protocol_uuid: request.protocol_uuid,
                revision: request.revision,
                method_id: request.method_id,
                request_id: request.request_id,
                outcome: RpcOutcome::SUCCESS,
                payload,
                resources: Vec::new(),
            }
        }

        pub fn error(request: &Request, protocol_error: i64) -> Self {
            let mut response = Self::success(request, Vec::new());
            response.outcome.protocol_error = protocol_error;
            response
        }

        pub fn push_resource(&mut self, resource: EndpointResource) {
            self.resources.push(resource);
        }
    }

    /// Requests a service may hold before the transport refuses more.
    pub const DEFAULT_MAX_IN_FLIGHT: u32 = 64;
    const FIRST_FRAME_TIMEOUT: Duration = Duration::from_secs(2);

    /// The subset of a published protocol descriptor needed by the host
    /// admission path.  Keeping an owned copy is important: a Linux socket has
    /// no kernel ProtocolEndpoint carrying this metadata for us.
    #[derive(Clone)]
    pub(crate) struct ProtocolContract {
        pub(crate) uuid: [u8; 16],
        pub(crate) revision: u64,
        pub(crate) max_request_bytes: u64,
        pub(crate) max_response_bytes: u64,
        pub(crate) max_resources: u64,
        pub(crate) method_count: u64,
        pub(crate) method_bitmap: [u64; 4],
        pub(crate) method_rights: [u64; 256],
        pub(crate) protocol_rights: u64,
    }

    impl ProtocolContract {
        fn from_descriptor(descriptor: &sys::ProtocolDescriptor) -> Result<Self, ServerError> {
            let valid_uuid = descriptor.uuid.bytes.iter().any(|value| *value != 0);
            let valid_shape = descriptor.struct_size as usize
                >= core::mem::size_of::<sys::ProtocolDescriptor>()
                && descriptor.flags & !1 == 0
                && descriptor.reserved0 == 0
                && descriptor.reserved1 == 0
                && descriptor.scope != 0
                && valid_uuid
                && descriptor.revision != 0
                && descriptor.method_count != 0
                && descriptor.method_count <= 256
                && descriptor.max_request_bytes <= linux::MAX_FRAME_BYTES as u64
                && descriptor.max_response_bytes <= linux::MAX_FRAME_BYTES as u64
                && descriptor.max_resources <= linux::MAX_RESOURCES as u64
                && descriptor.protocol_rights & sys::PROTOCOL_RIGHT_INVOKE != 0;
            if !valid_shape {
                return Err(ServerError::Protocol);
            }
            if descriptor.oneway_bitmap.iter().any(|word| *word != 0) {
                // The Linux envelope has no one-way frame kind.  Accepting a
                // descriptor that advertises one-way methods would silently
                // turn a protocol contract violation into a request/reply.
                return Err(ServerError::Protocol);
            }
            for method_id in 1..=descriptor.method_count as usize {
                let word = (method_id - 1) / 64;
                let bit = 1_u64 << ((method_id - 1) % 64);
                if descriptor.method_bitmap[word] & bit == 0
                    || descriptor.method_rights[method_id - 1] == 0
                    || descriptor.method_rights[method_id - 1] & sys::PROTOCOL_RIGHT_INVOKE == 0
                    || descriptor.method_rights[method_id - 1] & !descriptor.protocol_rights != 0
                {
                    return Err(ServerError::Protocol);
                }
            }
            for method_id in descriptor.method_count as usize..256 {
                let word = method_id / 64;
                let bit = 1_u64 << (method_id % 64);
                if descriptor.method_bitmap[word] & bit != 0
                    || descriptor.method_rights[method_id] != 0
                {
                    return Err(ServerError::Protocol);
                }
            }
            Ok(Self {
                uuid: descriptor.uuid.bytes,
                revision: descriptor.revision,
                max_request_bytes: descriptor.max_request_bytes,
                max_response_bytes: descriptor.max_response_bytes,
                max_resources: descriptor.max_resources,
                method_count: descriptor.method_count,
                method_bitmap: descriptor.method_bitmap,
                method_rights: descriptor.method_rights,
                protocol_rights: descriptor.protocol_rights,
            })
        }

        fn validate_request(&self, request: &RpcRequestOwned) -> Result<(), i64> {
            if request.protocol_uuid != self.uuid
                || request.revision != self.revision
                || request.method_id == 0
                || request.method_id > self.method_count
                || (self.max_request_bytes != 0
                    && request.payload.len() as u64 > self.max_request_bytes)
                || (self.max_resources != 0 && request.resources.len() as u64 > self.max_resources)
            {
                return Err(-71);
            }
            let index = request.method_id as usize - 1;
            let bit = 1_u64 << (index % 64);
            if self.method_bitmap[index / 64] & bit == 0 {
                return Err(-71);
            }
            let required = self.method_rights[index];
            if let Some(target) = request.target {
                if target.rights & required != required {
                    return Err(-13);
                }
            } else if required & !self.protocol_rights != 0 {
                return Err(-13);
            }
            Ok(())
        }

        fn validate_response(&self, response: &RpcResponseOwned) -> Result<(), i64> {
            if response.protocol_uuid != self.uuid
                || response.revision != self.revision
                || response.method_id == 0
                || response.method_id > self.method_count
                || (self.max_response_bytes != 0
                    && response.payload.len() as u64 > self.max_response_bytes)
                || (self.max_resources != 0 && response.resources.len() as u64 > self.max_resources)
                || !response.bulk.is_empty()
                || response.target.is_some()
            {
                return Err(-71);
            }
            let index = response.method_id as usize - 1;
            let bit = 1_u64 << (index % 64);
            if self.method_bitmap[index / 64] & bit == 0 {
                return Err(-71);
            }
            Ok(())
        }
    }

    pub struct Server {
        path: std::path::PathBuf,
        authority: LinuxResourceAuthority,
        admission: Admission,
        receiver: tokio::sync::mpsc::Receiver<PendingRequest>,
        accept_task: tokio::task::JoinHandle<()>,
        closed_resources: std::sync::Arc<std::sync::Mutex<Vec<ResourceDescriptor>>>,
        _bulk_server: linux::bulk::RegistrationServer,
        _release_server: linux::ReleaseServer,
    }

    struct PendingRequest {
        stream: UnixStream,
        session: LinuxServiceSession,
        request: RpcRequestOwned,
        admission: AdmissionPermit,
        contract: ProtocolContract,
    }

    async fn reject_admission(
        stream: &mut UnixStream,
        request: &RpcRequestOwned,
        protocol_error: i64,
        bulk_endpoint: &std::path::Path,
        owner: u64,
    ) {
        let response = RpcResponseOwned {
            protocol_uuid: request.protocol_uuid,
            revision: request.revision,
            method_id: request.method_id,
            request_id: request.request_id,
            target: None,
            outcome: RpcOutcome {
                execution: 0,
                reason: 0,
                protocol_error,
            },
            payload: Vec::new(),
            resources: Vec::new(),
            bulk: Vec::new(),
        };
        if let Ok(frame) = linux::encode_response(&response) {
            let _ = stream.write_all(&frame).await;
        }
        linux::bulk::release_for(bulk_endpoint, owner, &request.bulk);
    }

    async fn read_connection(
        mut stream: UnixStream,
        session: LinuxServiceSession,
        authority: LinuxResourceAuthority,
        admission: Admission,
        contract: ProtocolContract,
        request_validator: RequestValidator,
        sender: tokio::sync::mpsc::Sender<PendingRequest>,
        connection_permit: tokio::sync::OwnedSemaphorePermit,
    ) {
        let _connection_permit = connection_permit;
        let body =
            match tokio::time::timeout(FIRST_FRAME_TIMEOUT, linux::read_frame_async(&mut stream))
                .await
            {
                Ok(Ok(body)) => body,
                _ => return,
            };
        let decoded = match linux::decode_request(&body) {
            Ok(request) => request,
            Err(_) => return,
        };
        let request_contract = decoded
            .target
            .and_then(|target| authority.target_contract(target))
            .unwrap_or_else(|| contract.clone());
        let contract_result = if let Some(target) = decoded.target {
            authority.validate_target_protocol(target, &decoded)
        } else {
            request_contract
                .validate_request(&decoded)
                .and_then(|()| request_validator(&decoded))
        };
        if let Err(protocol_error) = contract_result {
            reject_admission(
                &mut stream,
                &decoded,
                protocol_error,
                session.bulk_endpoint(),
                session.peer_pid(),
            )
            .await;
            return;
        }
        if let Some(target) = decoded.target {
            if !authority.validate(target) || !session.adopt_resource(target) {
                reject_admission(
                    &mut stream,
                    &decoded,
                    -13,
                    session.bulk_endpoint(),
                    session.peer_pid(),
                )
                .await;
                return;
            }
        }
        for resource in decoded.resources.iter().copied() {
            let accepted =
                if resource.rights & sys::RIGHT_TRANSFER != 0 && authority.validate(resource) {
                    session.adopt_resource(resource)
                } else if valid_memory_transfer(
                    resource,
                    &decoded.bulk,
                    session.bulk_endpoint(),
                    session.peer_pid(),
                ) {
                    session.import_external_resource(resource)
                } else {
                    false
                };
            if !accepted {
                let protocol_error =
                    if resource.binding == 0 || resource.scope == 0 || resource.rights == 0 {
                        -22
                    } else {
                        -13
                    };
                reject_admission(
                    &mut stream,
                    &decoded,
                    protocol_error,
                    session.bulk_endpoint(),
                    session.peer_pid(),
                )
                .await;
                return;
            }
        }
        let permit = match admission.try_admit() {
            Ok(permit) => permit,
            Err(refusal) => {
                reject_admission(
                    &mut stream,
                    &decoded,
                    refusal.errno(),
                    session.bulk_endpoint(),
                    session.peer_pid(),
                )
                .await;
                return;
            }
        };
        if let Err(error) = sender
            .send(PendingRequest {
                stream,
                session,
                request: decoded,
                admission: permit,
                contract: request_contract,
            })
            .await
        {
            // The request did not reach the receiver, so release its bulk
            // registration explicitly.  The admission permit is dropped with
            // the failed message as well.
            linux::bulk::release_for(
                error.0.session.bulk_endpoint(),
                error.0.session.peer_pid(),
                &error.0.request.bulk,
            );
            // The admission permit is dropped here as well.
        }
    }

    async fn accept_connections(
        listener: UnixListener,
        path: PathBuf,
        sender: tokio::sync::mpsc::Sender<PendingRequest>,
        authority: LinuxResourceAuthority,
        admission: Admission,
        contract: ProtocolContract,
        request_validator: RequestValidator,
        next_session: AtomicU64,
        closed_resources: std::sync::Arc<std::sync::Mutex<Vec<ResourceDescriptor>>>,
    ) {
        let connections = std::sync::Arc::new(tokio::sync::Semaphore::new(linux::MAX_CONNECTIONS));
        loop {
            let (stream, _) = match listener.accept().await {
                Ok(value) => value,
                Err(_) => break,
            };
            let Ok(connection_permit) = connections.clone().try_acquire_owned() else {
                // A peer cannot turn the service into an unbounded task set.
                drop(stream);
                continue;
            };
            let peer_pid = stream
                .peer_cred()
                .ok()
                .and_then(|credentials| credentials.pid())
                .map(|pid| pid as u64)
                .unwrap_or(0);
            let session = LinuxServiceSession::with_connection(
                next_session.fetch_add(1, Ordering::Relaxed),
                peer_pid,
                path.clone(),
                authority.clone(),
                closed_resources.clone(),
            );
            tokio::spawn(read_connection(
                stream,
                session,
                authority.clone(),
                admission.clone(),
                contract.clone(),
                request_validator,
                sender.clone(),
                connection_permit,
            ));
        }
    }

    impl Server {
        pub async fn publish(
            directory: &ServiceDirectory,
            uri: &str,
            descriptor: &sys::ProtocolDescriptor,
            request_validator: RequestValidator,
        ) -> Result<Self, ServerError> {
            let contract = ProtocolContract::from_descriptor(descriptor)?;
            let endpoint = directory
                .endpoint_for(uri)
                .map_err(|_| ServerError::Protocol)?;
            let path = endpoint.path().to_path_buf();
            if let Some(parent) = path.parent() {
                std::fs::create_dir_all(parent).map_err(|_| ServerError::Io)?;
            }
            let bulk_server = linux::bulk::bind(&path).map_err(|_| ServerError::Io)?;
            let _ = std::fs::remove_file(&path);
            let listener = UnixListener::bind(&path).map_err(|_| ServerError::Io)?;
            let (sender, receiver) = tokio::sync::mpsc::channel(linux::MAX_CONNECTIONS);
            let authority = LinuxResourceAuthority::new();
            let admission = Admission::new(DEFAULT_MAX_IN_FLIGHT);
            let closed_resources = std::sync::Arc::new(std::sync::Mutex::new(Vec::new()));
            let release_server =
                linux::bind_release(&path, authority.clone(), closed_resources.clone())
                    .map_err(|_| ServerError::Io)?;
            let accept_task = tokio::spawn(accept_connections(
                listener,
                path.clone(),
                sender,
                authority.clone(),
                admission.clone(),
                contract.clone(),
                request_validator,
                AtomicU64::new(1),
                closed_resources.clone(),
            ));
            Ok(Self {
                path,
                authority,
                admission,
                receiver,
                accept_task,
                closed_resources,
                _bulk_server: bulk_server,
                _release_server: release_server,
            })
        }

        /// Publish the concurrency this service can actually dispatch.  The
        /// value must be the same one the service reports in its medium or
        /// device info.
        pub fn set_max_in_flight(&mut self, limit: u32) {
            self.admission.set_limit(limit);
        }

        /// The currently enforced admission bound.
        pub fn max_in_flight(&self) -> u32 {
            self.admission.limit()
        }

        /// Requests admitted and not yet answered.
        pub fn in_flight(&self) -> u32 {
            self.admission.in_flight()
        }

        /// Highest in-flight count observed since the service started.
        pub fn peak_in_flight(&self) -> u32 {
            self.admission.peak_in_flight()
        }

        /// Requests refused because the service was saturated.
        pub fn refused_requests(&self) -> u64 {
            self.admission.refused()
        }

        pub fn create_endpoint(
            &mut self,
            descriptor: &sys::ProtocolDescriptor,
            binding: u32,
            scope: u64,
            rights: u64,
            request_validator: RequestValidator,
        ) -> Result<EndpointResource, ServerError> {
            // Dynamic endpoints carry their own protocol contract.  Validate
            // it at mint time as well as at publish time so a malformed
            // descriptor can never become an admitted target later.
            let _ = ProtocolContract::from_descriptor(descriptor)?;
            let resource = EndpointResource {
                descriptor: self.authority.mint_endpoint(
                    std::process::id() as u64,
                    binding,
                    scope,
                    rights | sys::PROTOCOL_RIGHT_INVOKE | sys::RIGHT_TRANSFER | sys::RIGHT_WAIT,
                    descriptor,
                    request_validator,
                ),
            };
            Ok(resource)
        }

        pub fn take_closed_resources(&mut self) -> Vec<ResourceDescriptor> {
            let Ok(mut closed) = self.closed_resources.lock() else {
                return Vec::new();
            };
            core::mem::take(&mut *closed)
        }

        pub fn memory_resource(
            &mut self,
            _memory: crate::memory::MemoryObject,
            _rights: u64,
        ) -> Result<EndpointResource, ServerError> {
            Err(ServerError::Status(sys::STATUS_NOT_SUPPORTED))
        }

        pub async fn next(&mut self) -> Result<Request, ServerError> {
            let pending = self.receiver.recv().await.ok_or(ServerError::Io)?;
            let PendingRequest {
                stream,
                session,
                request: decoded,
                admission,
                contract,
            } = pending;
            Ok(Request {
                protocol_uuid: decoded.protocol_uuid,
                revision: decoded.revision,
                method_id: decoded.method_id,
                request_id: decoded.request_id,
                target: decoded.target,
                payload: decoded.payload,
                resources: decoded.resources,
                bulk: decoded.bulk,
                stream: Some(stream),
                session,
                _admission: Some(admission),
                contract,
            })
        }

        pub async fn next_with_ready(
            &mut self,
            ready: tokio::sync::oneshot::Sender<()>,
        ) -> Result<Request, ServerError> {
            let _ = ready.send(());
            self.next().await
        }

        /// Serve requests until the endpoint set fails.
        ///
        /// This owns the policy that must be identical in every service: a peer
        /// that disconnects, or that sends a frame this service cannot accept,
        /// costs *that peer* -- not the service, which keeps serving everyone
        /// else.  Anything else is fatal, and because restarting a daemon
        /// belongs to the supervisor this returns the exit code instead of
        /// retrying a broken runtime in place.
        ///
        /// The handler owns everything past admission -- dispatch, ordering and
        /// the reply -- because that is where services legitimately differ: one
        /// overlaps observations, another runs a synchronous filesystem library
        /// off the reactor thread, and each decides its own ordering.  It is a
        /// trait rather than a closure because the work is async and needs the
        /// server, which a higher-ranked closure cannot express without boxing.
        pub async fn serve<H: ServeHandler>(&mut self, handler: &mut H) -> i32 {
            loop {
                let request = match self.next().await {
                    Ok(request) => request,
                    // The peer went away, or its frame was not something this
                    // service accepts.  Both belong to that peer.
                    Err(ServerError::PeerClosed) | Err(ServerError::Protocol) => continue,
                    Err(error) => {
                        log::error!("service receive failed: {error:?}");
                        return 1;
                    }
                };
                handler.handle(self, request).await;
            }
        }

        pub async fn respond(
            &mut self,
            request: Request,
            response: Response,
        ) -> Result<(), ServerError> {
            request.respond(response).await
        }
    }

    impl Request {
        /// Answer this request and release everything it owns.
        ///
        /// Takes `self` rather than a server borrow: the stream and the
        /// admission permit belong to the request, so a service can hand the
        /// reply to another task and keep reading the next one.  Dispatch order
        /// is unchanged; only the reply's write leaves the service loop's
        /// critical path.
        pub async fn respond(mut self, response: Response) -> Result<(), ServerError> {
            let response_resources: Vec<_> = response
                .resources
                .iter()
                .map(EndpointResource::descriptor)
                .collect();
            let owned = RpcResponseOwned {
                protocol_uuid: response.protocol_uuid,
                revision: response.revision,
                method_id: response.method_id,
                request_id: response.request_id,
                target: None,
                outcome: response.outcome,
                payload: response.payload,
                resources: response_resources.clone(),
                bulk: Vec::new(),
            };
            if self.contract.validate_response(&owned).is_err()
                || !self.session.prepare_response_resources(&response_resources)
            {
                self.session
                    .finalize_response_resources(&response_resources, false);
                return Err(ServerError::Protocol);
            }
            let mut stream = match self.stream.take() {
                Some(stream) => stream,
                None => {
                    self.session
                        .finalize_response_resources(&response_resources, false);
                    return Err(ServerError::PeerClosed);
                }
            };
            let frame = match linux::encode_response(&owned) {
                Ok(frame) => frame,
                Err(_) => {
                    self.session
                        .finalize_response_resources(&response_resources, false);
                    return Err(ServerError::Protocol);
                }
            };
            let write_result = stream.write_all(&frame).await;
            self.session
                .finalize_response_resources(&response_resources, write_result.is_ok());
            write_result.map_err(|_| ServerError::Io)?;
            if !self.bulk.is_empty() {
                linux::bulk::release_for(
                    self.session.bulk_endpoint(),
                    self.session.peer_pid(),
                    &self.bulk,
                );
                self.bulk.clear();
            }
            Ok(())
        }
    }

    impl Drop for Server {
        fn drop(&mut self) {
            self.accept_task.abort();
            let _ = std::fs::remove_file(&self.path);
            let mut release = self.path.as_os_str().to_os_string();
            release.push(".release");
            let _ = std::fs::remove_file(release);
        }
    }

    /// What a service does with an admitted request.
    ///
    /// Everything past admission is the service's: which lock, if any, to take;
    /// whether work may overlap; and how the reply is produced.  `server` is
    /// passed because minting an endpoint needs it, which is also the reason a
    /// handler runs inline instead of on a task it would have to hand the
    /// server to.
    #[allow(async_fn_in_trait)]
    pub trait ServeHandler {
        async fn handle(&mut self, server: &mut Server, request: Request);
    }

    fn valid_memory_transfer(
        resource: ResourceDescriptor,
        bulk: &[BulkBuffer],
        endpoint: &std::path::Path,
        owner: u64,
    ) -> bool {
        let Some(descriptor) = bulk.first().copied() else {
            return false;
        };
        let allowed = sys::RIGHT_TRANSFER
            | sys::MEMORY_RIGHT_READ
            | sys::MEMORY_RIGHT_WRITE
            | sys::MEMORY_RIGHT_MAP
            | sys::MEMORY_RIGHT_INFO;
        bulk.len() == 1
            && resource.resource_id == descriptor.region_id
            && resource.binding == sys::BINDING_MEMORY_OBJECT
            && resource.scope == naos_idl::memory_object::PROTOCOL_SCOPE
            && resource.rights & sys::RIGHT_TRANSFER != 0
            && resource.rights & sys::MEMORY_RIGHT_MAP != 0
            && resource.rights & !allowed == 0
            && resource.view_offset == descriptor.offset
            && resource.view_length == descriptor.length
            && linux::bulk::is_registered_for(endpoint, owner, descriptor)
    }

    #[cfg(test)]
    mod tests {
        use super::*;
        use crate::linux::{LinuxServiceDirectory, LinuxUdsTransport};
        use naos_idl::transport::RpcRequest;
        use std::time::Duration;

        fn descriptor() -> sys::ProtocolDescriptor {
            let mut descriptor = sys::ProtocolDescriptor::default();
            descriptor.struct_size = core::mem::size_of::<sys::ProtocolDescriptor>() as u32;
            descriptor.uuid.bytes = [1; 16];
            descriptor.scope = 1;
            descriptor.revision = 1;
            descriptor.protocol_rights = sys::PROTOCOL_RIGHT_INVOKE;
            descriptor.method_count = 1;
            descriptor.method_bitmap[0] = 1;
            descriptor.method_rights[0] = sys::PROTOCOL_RIGHT_INVOKE;
            descriptor
        }

        fn accept_all_resources(_: &RpcRequestOwned) -> Result<(), i64> {
            Ok(())
        }

        #[test]
        fn protocol_contract_rejects_reserved_and_oneway_bits() {
            let mut reserved = descriptor();
            reserved.reserved0 = 1;
            assert!(ProtocolContract::from_descriptor(&reserved).is_err());

            let mut oneway = descriptor();
            oneway.flags = 1;
            oneway.oneway_bitmap[0] = 1;
            assert!(ProtocolContract::from_descriptor(&oneway).is_err());
        }

        #[test]
        fn protocol_contract_rejects_oversized_responses() {
            let mut descriptor = descriptor();
            descriptor.max_response_bytes = 4;
            let contract = ProtocolContract::from_descriptor(&descriptor).unwrap();
            let response = RpcResponseOwned {
                protocol_uuid: descriptor.uuid.bytes,
                revision: descriptor.revision,
                method_id: 1,
                request_id: 1,
                target: None,
                outcome: RpcOutcome::SUCCESS,
                payload: vec![0; 5],
                resources: Vec::new(),
                bulk: Vec::new(),
            };
            assert!(contract.validate_response(&response).is_err());
        }

        #[tokio::test]
        async fn rejected_resource_does_not_stop_server() {
            let root = std::env::temp_dir().join(std::format!(
                "naos-servicekit-admission-{}",
                std::process::id()
            ));
            let directory = LinuxServiceDirectory::new(&root);
            let endpoint = directory
                .endpoint_for(crate::uri::FS_VFS)
                .expect("valid service URI");
            let mut descriptor = sys::ProtocolDescriptor::default();
            descriptor.struct_size = core::mem::size_of::<sys::ProtocolDescriptor>() as u32;
            descriptor.uuid.bytes = [1; 16];
            descriptor.scope = 1;
            descriptor.revision = 1;
            descriptor.protocol_rights = sys::PROTOCOL_RIGHT_INVOKE;
            descriptor.method_count = 1;
            descriptor.max_resources = 1;
            descriptor.method_bitmap[0] = 1;
            descriptor.method_rights[0] = sys::PROTOCOL_RIGHT_INVOKE;
            let mut server = Server::publish(
                &directory,
                crate::uri::FS_VFS,
                &descriptor,
                accept_all_resources,
            )
            .await
            .expect("bind service");

            let server_task = tokio::spawn(async move {
                let request = server
                    .next()
                    .await
                    .expect("server survives rejected request");
                let response = Response::error(&request, 0);
                server
                    .respond(request, response)
                    .await
                    .expect("respond to valid request");
            });

            let transport = LinuxUdsTransport::new();
            let forged = ResourceDescriptor {
                resource_id: 123,
                binding: sys::BINDING_MEMORY_OBJECT,
                scope: naos_idl::memory_object::PROTOCOL_SCOPE,
                rights: sys::RIGHT_TRANSFER,
                ..ResourceDescriptor::default()
            };
            let rejected = transport
                .invoke_async(
                    &endpoint,
                    RpcRequest {
                        protocol_uuid: [1; 16],
                        revision: 1,
                        method_id: 1,
                        request_id: 0,
                        target: None,
                        payload: &[],
                        resources: std::slice::from_ref(&forged),
                        bulk: &[],
                    },
                )
                .await
                .expect("admission rejection response");
            assert_eq!(rejected.outcome.protocol_error, -13);

            let accepted = transport
                .invoke_async(
                    &endpoint,
                    RpcRequest {
                        protocol_uuid: [1; 16],
                        revision: 1,
                        method_id: 1,
                        request_id: 0,
                        target: None,
                        payload: &[],
                        resources: &[],
                        bulk: &[],
                    },
                )
                .await
                .expect("server accepted request after rejection");
            assert_eq!(accepted.outcome.protocol_error, 0);

            tokio::time::timeout(Duration::from_secs(1), server_task)
                .await
                .expect("server task completed")
                .expect("server task did not panic");
            let _ = std::fs::remove_dir_all(root);
        }
    }
}

#[cfg(target_os = "naos")]
mod platform {
    use crate::admission::{Admission, AdmissionPermit};
    use crate::naos;
    use crate::sys;
    use alloc::collections::VecDeque;
    use alloc::vec;
    use alloc::vec::Vec;
    use naos_idl::transport::{BulkBuffer, ResourceDescriptor, RpcOutcome, RpcRequestOwned};
    use naos_idl::{
        CallError, FailInvocation, IncomingRequest, OwnedHandle, ProtocolServerEndpoint,
        ReceivedResources, ResourceSlot, ResourceTable, ResponderHandle,
    };

    /// Shared service-specific admission hook.  NaOS validates the generated
    /// endpoint contract in the kernel; the hook is accepted here so daemon
    /// call sites have one signature on Linux and NaOS.
    pub type RequestValidator = fn(&RpcRequestOwned) -> Result<(), i64>;

    #[derive(Clone, Copy, Debug, Eq, PartialEq)]
    pub struct ServiceDirectory {
        handle: sys::Handle,
    }

    impl ServiceDirectory {
        pub const fn from_raw(handle: sys::Handle) -> Self {
            Self { handle }
        }

        pub const fn raw(self) -> sys::Handle {
            self.handle
        }
    }

    pub struct EndpointResource {
        descriptor: ResourceDescriptor,
        handle: Option<OwnedHandle>,
    }

    impl EndpointResource {
        pub fn descriptor(&self) -> ResourceDescriptor {
            self.descriptor
        }

        /// Wrap an endpoint created by a protocol-specific server so the
        /// common bootstrap adapter can transfer it without manufacturing a
        /// second endpoint pair. The handle remains owned by this wrapper.
        pub fn from_owned(
            handle: OwnedHandle,
            descriptor: &sys::ProtocolDescriptor,
        ) -> Result<Self, ServerError> {
            let resource_id = naos_idl::object_id(handle.get()).map_err(|error| match error {
                CallError::Status(status) => ServerError::Status(status),
                _ => ServerError::Protocol,
            })?;
            Ok(Self {
                descriptor: ResourceDescriptor {
                    resource_id,
                    binding: sys::BINDING_CLIENT_END,
                    scope: descriptor.scope,
                    rights: descriptor.protocol_rights | sys::RIGHT_TRANSFER | sys::RIGHT_WAIT,
                    ..ResourceDescriptor::default()
                },
                handle: Some(handle),
            })
        }

        pub(crate) fn take_handle(&mut self) -> Option<OwnedHandle> {
            self.handle.take()
        }
    }

    struct RequestState {
        responder: Option<ResponderHandle>,
        resources: ReceivedResources,
        /// Held for the request's lifetime: answering a request and abandoning
        /// one release the admission slot through the same `Drop`, so a peer
        /// that disappears cannot leak capacity.
        _admission: Option<AdmissionPermit>,
    }

    pub struct Request {
        pub protocol_uuid: [u8; 16],
        pub revision: u64,
        pub method_id: u64,
        pub request_id: u64,
        pub target: Option<ResourceDescriptor>,
        pub payload: Vec<u8>,
        pub resources: Vec<ResourceDescriptor>,
        pub bulk: Vec<BulkBuffer>,
        state: Option<RequestState>,
    }

    impl Request {
        pub fn resource_count(&self) -> usize {
            self.resources.len()
        }

        pub fn read_memory(
            &self,
            slot: ResourceSlot,
            offset: u64,
            data: &mut [u8],
        ) -> Result<(), ServerError> {
            if !self.validate_memory(slot, true) {
                return Err(ServerError::Status(sys::STATUS_ACCESS_DENIED));
            }
            let state = self.state.as_ref().ok_or(ServerError::PeerClosed)?;
            let handle = state.resources.get(slot).ok_or(ServerError::Protocol)?;
            let memory = crate::memory::MemoryObject::duplicate_from(handle)
                .map_err(|_| ServerError::Status(sys::STATUS_IO_ERROR))?;
            memory
                .read_at(offset, data)
                .map_err(|_| ServerError::Status(sys::STATUS_IO_ERROR))
        }

        pub fn write_memory(
            &self,
            slot: ResourceSlot,
            offset: u64,
            data: &[u8],
        ) -> Result<(), ServerError> {
            if !self.validate_memory(slot, false) {
                return Err(ServerError::Status(sys::STATUS_ACCESS_DENIED));
            }
            let state = self.state.as_ref().ok_or(ServerError::PeerClosed)?;
            let handle = state.resources.get(slot).ok_or(ServerError::Protocol)?;
            let memory = crate::memory::MemoryObject::duplicate_from(handle)
                .map_err(|_| ServerError::Status(sys::STATUS_IO_ERROR))?;
            memory
                .write_at(offset, data)
                .map_err(|_| ServerError::Status(sys::STATUS_IO_ERROR))
        }

        /// Run a service-side read directly against the caller's MemoryObject
        /// data plane. The mapping is borrowed only for this request and is
        /// never retained as a cache or buffer pool.
        pub fn with_read_memory<T>(
            &self,
            slot: ResourceSlot,
            offset: u64,
            length: usize,
            f: impl FnOnce(&[u8]) -> T,
        ) -> Result<T, ServerError> {
            if !self.validate_memory(slot, true) {
                return Err(ServerError::Status(sys::STATUS_ACCESS_DENIED));
            }
            let state = self.state.as_ref().ok_or(ServerError::PeerClosed)?;
            let handle = state.resources.get(slot).ok_or(ServerError::Protocol)?;
            let mapping = crate::memory::map_read_at(handle, offset, length)
                .map_err(|error| ServerError::Status(region_access_status(error)))?;
            Ok(f(mapping.as_slice()))
        }

        /// Run a service-side write directly against the caller's MemoryObject
        /// data plane. The shared mapping makes the callback's writes visible
        /// to the caller when the request completes.
        pub fn with_write_memory<T>(
            &self,
            slot: ResourceSlot,
            offset: u64,
            length: usize,
            f: impl FnOnce(&mut [u8]) -> Result<T, i64>,
        ) -> Result<Result<T, i64>, ServerError> {
            if !self.validate_memory(slot, false) {
                return Err(ServerError::Status(sys::STATUS_ACCESS_DENIED));
            }
            let state = self.state.as_ref().ok_or(ServerError::PeerClosed)?;
            let handle = state.resources.get(slot).ok_or(ServerError::Protocol)?;
            let mut mapping = crate::memory::map_write_at(handle, offset, length)
                .map_err(|error| ServerError::Status(region_access_status(error)))?;
            Ok(f(mapping.as_mut_slice()))
        }

        pub fn validate_memory(&self, slot: ResourceSlot, write: bool) -> bool {
            let Some(state) = self.state.as_ref() else {
                return false;
            };
            let Some(handle) = state.resources.get(slot) else {
                return false;
            };
            let Ok(info) = naos_idl::handle_info(handle.get()) else {
                return false;
            };
            // The kernel capability table is the authority for binding, scope
            // and rights; a request descriptor never widens it.  A NaOS
            // capability carries no direction field, so the granted rights are
            // the direction: `admit_service_region` still requires the right
            // this operation needs and reports the statuses the Linux
            // descriptor path reports.
            crate::memory::admit_service_region(
                write,
                info.binding,
                info.scope,
                info.protocol_rights,
                None,
            )
            .is_ok()
                && handle.is_valid()
        }

        pub fn validate_resource(&self, resource: ResourceDescriptor) -> bool {
            if resource.resource_id == 0
                || resource.rights == 0
                || resource.rights & sys::RIGHT_TRANSFER == 0
            {
                return false;
            }

            // The target is installed by ServerEndpoint when the capability
            // is created; it is not decoded from the peer's request.  Accept
            // only an exact identity with attenuated rights.
            if let Some(target) = self.target
                && target.resource_id == resource.resource_id
            {
                return target.binding == resource.binding
                    && target.scope == resource.scope
                    && resource.rights & !target.rights == 0;
            }

            let Some(state) = self.state.as_ref() else {
                return false;
            };
            for index in 0..state.resources.len() {
                let Some(slot) = ResourceSlot::new(index as u32) else {
                    continue;
                };
                let Some(handle) = state.resources.get(slot) else {
                    continue;
                };
                let Ok(info) = naos_idl::handle_info(handle.get()) else {
                    continue;
                };
                if info.object_id == resource.resource_id
                    && info.binding == resource.binding
                    && info.scope == resource.scope
                    && resource.rights & !(info.meta_rights | info.protocol_rights) == 0
                {
                    return true;
                }
            }
            false
        }
    }

    pub struct Response {
        protocol_uuid: [u8; 16],
        revision: u64,
        method_id: u64,
        request_id: u64,
        outcome: RpcOutcome,
        payload: Vec<u8>,
        resources: Vec<EndpointResource>,
    }

    impl Response {
        pub fn success(request: &Request, payload: Vec<u8>) -> Self {
            Self {
                protocol_uuid: request.protocol_uuid,
                revision: request.revision,
                method_id: request.method_id,
                request_id: request.request_id,
                outcome: RpcOutcome::SUCCESS,
                payload,
                resources: Vec::new(),
            }
        }

        pub fn error(request: &Request, protocol_error: i64) -> Self {
            let mut response = Self::success(request, Vec::new());
            response.outcome.protocol_error = protocol_error;
            response
        }

        pub fn push_resource(&mut self, resource: EndpointResource) {
            self.resources.push(resource);
        }
    }

    #[derive(Clone, Copy, Debug, Eq, PartialEq)]
    pub enum ServerError {
        PeerClosed,
        Protocol,
        Status(sys::Status),
    }

    /// Service-facing status for a region access failure.
    ///
    /// A request that names a window outside the region is a bad argument
    /// (INVALID_ARGUMENT); a refusal the transport enforced is ACCESS_DENIED.
    /// Both transports use this mapping so an equal failure reports an equal
    /// status.
    fn region_access_status(error: crate::memory::MemoryError) -> sys::Status {
        match error {
            crate::memory::MemoryError::InvalidArgument => sys::STATUS_INVALID_ARGUMENT,
            crate::memory::MemoryError::AccessDenied => sys::STATUS_ACCESS_DENIED,
            crate::memory::MemoryError::Unsupported => sys::STATUS_NOT_SUPPORTED,
            crate::memory::MemoryError::Io => sys::STATUS_IO_ERROR,
        }
    }

    struct ServerEndpoint {
        readiness: Option<naos::Readiness>,
        endpoint: ProtocolServerEndpoint,
        target: Option<ResourceDescriptor>,
        protocol_uuid: [u8; 16],
        revision: u64,
    }

    /// Requests a service may hold before the transport refuses more.
    ///
    /// The bound is what makes the server's queue bounded instead of growing
    /// with whatever a peer submits.
    pub const DEFAULT_MAX_IN_FLIGHT: u32 = 64;

    pub struct Server {
        listener: OwnedHandle,
        endpoints: Vec<ServerEndpoint>,
        closed_endpoints: Vec<ResourceDescriptor>,
        pending_requests: VecDeque<Request>,
        request_wire: Vec<u8>,
        listener_readiness: Option<naos::Readiness>,
        readiness_dirty: bool,
        preflight_pending: bool,
        factory_uuid: [u8; 16],
        factory_revision: u64,
        admission: Admission,
    }

    impl Server {
        pub async fn publish(
            directory: &ServiceDirectory,
            uri: &str,
            descriptor: &sys::ProtocolDescriptor,
            _request_validator: RequestValidator,
        ) -> Result<Self, ServerError> {
            let listener = naos::publish_listener(directory.raw(), uri, descriptor)
                .map_err(ServerError::Status)?;
            Ok(Self {
                listener,
                endpoints: Vec::new(),
                closed_endpoints: Vec::new(),
                pending_requests: VecDeque::new(),
                request_wire: vec![0; 65_536],
                listener_readiness: None,
                readiness_dirty: true,
                // A source can become ready between registration and the
                // first selector wait. Drain it once synchronously before
                // relying on edge notifications.
                preflight_pending: true,
                factory_uuid: descriptor.uuid.bytes,
                factory_revision: descriptor.revision,
                admission: Admission::new(DEFAULT_MAX_IN_FLIGHT),
            })
        }

        /// Publish the concurrency this service can actually dispatch.
        ///
        /// A service calls this with the same value it reports in its medium
        /// or device info, so the promise and the enforced admission bound are
        /// one number.  Setting it to 1 makes the transport strictly serial,
        /// which is the honest setting for a service with a serial backend.
        pub fn set_max_in_flight(&mut self, limit: u32) {
            self.admission.set_limit(limit);
        }

        /// The currently enforced admission bound.
        pub fn max_in_flight(&self) -> u32 {
            self.admission.limit()
        }

        /// Requests admitted and not yet answered.
        pub fn in_flight(&self) -> u32 {
            self.admission.in_flight()
        }

        /// Highest in-flight count observed since the service started.
        pub fn peak_in_flight(&self) -> u32 {
            self.admission.peak_in_flight()
        }

        /// Requests refused because the service was saturated.
        pub fn refused_requests(&self) -> u64 {
            self.admission.refused()
        }

        pub fn create_endpoint(
            &mut self,
            descriptor: &sys::ProtocolDescriptor,
            binding: u32,
            scope: u64,
            rights: u64,
            _request_validator: RequestValidator,
        ) -> Result<EndpointResource, ServerError> {
            let (client, server) = naos_idl::create_endpoints_from_descriptor(descriptor).map_err(
                |error| match error {
                    CallError::Status(status) => ServerError::Status(status),
                    _ => ServerError::Protocol,
                },
            )?;
            let client_raw = client.into_raw();
            let client = unsafe { OwnedHandle::from_raw(client_raw) };
            let resource_id = naos_idl::object_id(client.get()).map_err(|error| match error {
                CallError::Status(status) => ServerError::Status(status),
                _ => ServerError::Protocol,
            })?;
            let endpoint_resource = EndpointResource {
                descriptor: ResourceDescriptor {
                    resource_id,
                    binding,
                    scope,
                    // INVOKE is the common endpoint right.  Keep the
                    // service-specific rights attenuated: adding the whole
                    // protocol right mask here would turn a read-only block
                    // lease into a writable one.
                    rights: rights
                        | sys::PROTOCOL_RIGHT_INVOKE
                        | sys::RIGHT_TRANSFER
                        | sys::RIGHT_WAIT,
                    ..ResourceDescriptor::default()
                },
                handle: Some(client),
            };
            self.endpoints.push(ServerEndpoint {
                readiness: None,
                endpoint: server,
                target: Some(endpoint_resource.descriptor),
                protocol_uuid: descriptor.uuid.bytes,
                revision: descriptor.revision,
            });
            // Register before returning the client capability.  The caller
            // may submit its first request as soon as the response resource
            // is transferred; deferring this registration until the next
            // `next()` would leave a readiness race at that boundary.
            self.refresh_readiness()?;
            self.readiness_dirty = false;
            self.preflight_pending = true;
            Ok(endpoint_resource)
        }

        pub fn memory_resource(
            &mut self,
            memory: crate::memory::MemoryObject,
            rights: u64,
        ) -> Result<EndpointResource, ServerError> {
            let handle = memory
                .into_read_only()
                .map_err(|_| ServerError::Status(sys::STATUS_IO_ERROR))?;
            let info = naos_idl::handle_info(handle.get()).map_err(|error| match error {
                CallError::Status(status) => ServerError::Status(status),
                _ => ServerError::Protocol,
            })?;
            let resource_id = naos_idl::object_id(handle.get()).map_err(|error| match error {
                CallError::Status(status) => ServerError::Status(status),
                _ => ServerError::Protocol,
            })?;
            Ok(EndpointResource {
                descriptor: ResourceDescriptor {
                    resource_id,
                    binding: sys::BINDING_MEMORY_OBJECT,
                    scope: naos_idl::memory_object::PROTOCOL_SCOPE,
                    rights,
                    view_offset: info.view_offset,
                    view_length: info.view_length,
                },
                handle: Some(handle),
            })
        }

        pub async fn next(&mut self) -> Result<Request, ServerError> {
            self.next_inner(None).await
        }

        pub async fn next_with_ready(
            &mut self,
            ready: tokio::sync::oneshot::Sender<()>,
        ) -> Result<Request, ServerError> {
            self.next_inner(Some(ready)).await
        }

        /// Return dynamic endpoint resources whose peer has closed. The
        /// resource identity is stable even if `endpoints` compacts itself.
        pub fn take_closed_resources(&mut self) -> Vec<ResourceDescriptor> {
            core::mem::take(&mut self.closed_endpoints)
        }

        async fn next_inner(
            &mut self,
            mut ready: Option<tokio::sync::oneshot::Sender<()>>,
        ) -> Result<Request, ServerError> {
            loop {
                if self.readiness_dirty {
                    self.refresh_readiness()?;
                    self.readiness_dirty = false;
                }
                if let Some(ready) = ready.take() {
                    let _ = ready.send(());
                }
                if let Some(request) = self.pending_requests.pop_front() {
                    return Ok(request);
                }
                if self.preflight_pending {
                    self.preflight_pending = false;
                    self.drain_listener()?;
                    let mut index = 0;
                    while index < self.endpoints.len() {
                        if self.drain_endpoint(index)? {
                            self.endpoints.swap_remove(index);
                            self.readiness_dirty = true;
                        } else {
                            index += 1;
                        }
                    }
                    // Accepting a listener peer installs a new endpoint. Go
                    // through refresh + preflight again so its first request
                    // cannot fall into the registration-to-wait gap.
                    if self.readiness_dirty || !self.pending_requests.is_empty() {
                        continue;
                    }
                }
                let (event_index, event) = {
                    let mut sources = Vec::with_capacity(self.endpoints.len() + 1);
                    sources.push(
                        self.listener_readiness
                            .as_ref()
                            .ok_or(ServerError::Protocol)?,
                    );
                    for endpoint in &self.endpoints {
                        sources.push(endpoint.readiness.as_ref().ok_or(ServerError::Protocol)?);
                    }
                    naos::next_readiness_refs(&sources).await.map_err(|error| {
                        log::error!("service server readiness poll failed error={error:?}");
                        ServerError::Status(sys::STATUS_IO_ERROR)
                    })?
                };
                if event_index == 0 {
                    self.drain_listener()?;
                    self.listener_readiness
                        .as_ref()
                        .ok_or(ServerError::Protocol)?
                        .clear_readiness(event);
                    continue;
                }
                let index = event_index - 1;
                if index >= self.endpoints.len() {
                    return Err(ServerError::Protocol);
                }
                let closed = self.drain_endpoint(index)?;
                self.endpoints[index]
                    .readiness
                    .as_ref()
                    .ok_or(ServerError::Protocol)?
                    .clear_readiness(event);
                if closed {
                    self.endpoints.swap_remove(index);
                    self.readiness_dirty = true;
                }
            }
        }

        fn refresh_readiness(&mut self) -> Result<(), ServerError> {
            if self.listener_readiness.is_none() {
                self.listener_readiness = Some(
                    naos::Readiness::new(self.listener.get())
                        .map_err(|_| ServerError::Status(sys::STATUS_IO_ERROR))?,
                );
            }
            for endpoint in &mut self.endpoints {
                if endpoint.readiness.is_none() {
                    endpoint.readiness = Some(
                        naos::Readiness::new(endpoint.endpoint.get())
                            .map_err(|_| ServerError::Status(sys::STATUS_IO_ERROR))?,
                    );
                }
            }
            Ok(())
        }

        fn drain_listener(&mut self) -> Result<(), ServerError> {
            loop {
                match naos::accept_listener(&self.listener) {
                    Ok(endpoint) => {
                        self.endpoints.push(ServerEndpoint {
                            readiness: None,
                            endpoint,
                            target: None,
                            protocol_uuid: self.factory_uuid,
                            revision: self.factory_revision,
                        });
                        self.readiness_dirty = true;
                        self.preflight_pending = true;
                    }
                    Err(CallError::Status(sys::STATUS_WOULD_BLOCK)) => return Ok(()),
                    Err(CallError::Status(status)) => return Err(ServerError::Status(status)),
                    Err(_) => return Err(ServerError::Protocol),
                }
            }
        }

        /// Drain an edge-triggered endpoint and return whether it was removed.
        ///
        /// Every received request is admitted before it is queued, so the
        /// queue cannot grow past `max_in_flight`.  A request that arrives
        /// while the service is saturated is refused immediately with EAGAIN:
        /// a bounded queue must tell the peer to retry, not hold a request the
        /// service has no capacity to answer.
        fn drain_endpoint(&mut self, index: usize) -> Result<bool, ServerError> {
            let target = self.endpoints[index].target;
            let protocol_uuid = self.endpoints[index].protocol_uuid;
            let revision = self.endpoints[index].revision;
            loop {
                match naos_idl::receive_request(
                    &self.endpoints[index].endpoint,
                    &mut self.request_wire,
                ) {
                    Ok(incoming) => {
                        let mut request =
                            request_from_incoming(incoming, target, protocol_uuid, revision);
                        match self.admission.try_admit() {
                            Ok(permit) => {
                                if let Some(state) = request.state.as_mut() {
                                    state._admission = Some(permit);
                                }
                                self.pending_requests.push_back(request);
                            }
                            Err(refusal) => {
                                let errno = refusal.errno();
                                log::debug!(
                                    "service admission refused method={} in_flight={} limit={} errno={}",
                                    request.method_id,
                                    self.admission.in_flight(),
                                    self.admission.limit(),
                                    errno
                                );
                                // Release this request's responder now; its
                                // resources go with the RequestState.
                                if let Some(state) = request.state.as_mut() {
                                    if let Some(mut responder) = state.responder.take() {
                                        let _ = responder.fail(FailInvocation {
                                            execution: 0,
                                            reason: 0,
                                            protocol_error: errno,
                                        });
                                    }
                                }
                            }
                        }
                    }
                    Err(CallError::Status(sys::STATUS_WOULD_BLOCK)) => {
                        return Ok(false);
                    }
                    Err(CallError::Status(sys::STATUS_PEER_CLOSED)) => {
                        if let Some(target) = self.endpoints[index].target {
                            self.closed_endpoints.push(target);
                        }
                        self.readiness_dirty = true;
                        return Ok(true);
                    }
                    Err(CallError::Status(status)) => return Err(ServerError::Status(status)),
                    Err(_) => return Err(ServerError::Protocol),
                }
            }
        }

        pub async fn respond(
            &mut self,
            request: Request,
            response: Response,
        ) -> Result<(), ServerError> {
            request.respond(response).await
        }

        /// Serve requests until the endpoint set fails.
        ///
        /// Same policy as the Linux side: a peer that went away, or a frame
        /// this service cannot accept, costs that peer rather than the service.
        /// A closed endpoint already had its capability record queued by
        /// `next()`, so the loop keeps running; administrative errors are fatal
        /// and become the exit code for the supervisor to act on.
        ///
        /// `handler` owns dispatch, ordering and the reply, and receives
        /// `&mut Server` because a service may need to mint an endpoint
        /// (`BlockDeviceFactory.acquire` does).
        pub async fn serve<H: ServeHandler>(&mut self, handler: &mut H) -> i32 {
            loop {
                let request = match self.next().await {
                    Ok(request) => request,
                    Err(ServerError::PeerClosed) | Err(ServerError::Protocol) => continue,
                    Err(error) => {
                        log::error!("service receive failed: {error:?}");
                        return 1;
                    }
                };
                handler.handle(self, request).await;
            }
        }
    }

    impl Request {
        /// Answer this request and release everything it owns.
        ///
        /// Takes `self` rather than a server borrow, which is what lets a
        /// service hand the reply to another task and keep reading: the
        /// responder and the admission permit both belong to the request.  The
        /// server's dispatch order is unchanged; only the reply's delivery
        /// moves off the service loop's critical path.
        pub async fn respond(self, response: Response) -> Result<(), ServerError> {
            let Request { state, .. } = self;
            let state = state.ok_or(ServerError::PeerClosed)?;
            let Some(mut responder) = state.responder else {
                return Ok(());
            };
            if response.outcome.protocol_error != 0
                || response.outcome.execution != 0
                || response.outcome.reason != 0
            {
                responder
                    .fail(FailInvocation {
                        execution: response.outcome.execution,
                        reason: response.outcome.reason,
                        protocol_error: response.outcome.protocol_error,
                    })
                    .map_err(ServerError::Status)?;
                return Ok(());
            }
            let mut resources = ResourceTable::new();
            for resource in response.resources {
                let handle = resource.handle.ok_or(ServerError::Protocol)?;
                resources
                    .push_move(handle)
                    .map_err(|_| ServerError::Status(sys::STATUS_RESOURCE_EXHAUSTED))?;
            }
            responder
                .reply(&response.payload, resources.as_slice())
                .map_err(ServerError::Status)?;
            resources.commit_move();
            Ok(())
        }
    }

    /// What a service does with an admitted request.
    ///
    /// Same contract as the Linux side: dispatch, ordering and the reply belong
    /// to the service, while the loop and its fault policy belong to
    /// `Server::serve`.  A closed endpoint has already had its capability record
    /// queued by `next()`, so it is not the handler's business.
    #[allow(async_fn_in_trait)]
    pub trait ServeHandler {
        async fn handle(&mut self, server: &mut Server, request: Request);
    }

    fn request_from_incoming(
        incoming: IncomingRequest<'_>,
        target: Option<ResourceDescriptor>,
        protocol_uuid: [u8; 16],
        revision: u64,
    ) -> Request {
        let resources = (0..incoming.resources.len())
            .filter_map(|index| {
                let slot = ResourceSlot::new(index as u32)?;
                let handle = incoming.resources.get(slot)?;
                let info = naos_idl::handle_info(handle.get()).ok()?;
                Some(ResourceDescriptor {
                    resource_id: info.object_id,
                    binding: info.binding,
                    scope: info.scope,
                    rights: info.meta_rights | info.protocol_rights,
                    view_offset: info.view_offset,
                    view_length: info.view_length,
                })
            })
            .collect();
        Request {
            protocol_uuid,
            revision,
            method_id: incoming.method_id,
            request_id: 0,
            target,
            payload: incoming.wire.to_vec(),
            resources,
            bulk: Vec::new(),
            state: Some(RequestState {
                responder: incoming.responder,
                resources: incoming.resources,
                _admission: None,
            }),
        }
    }
}

#[cfg(target_os = "linux")]
pub use platform::{
    EndpointResource, Request, RequestValidator, Response, ServeHandler, Server, ServerError,
    ServiceDirectory,
};

#[cfg(target_os = "linux")]
pub(crate) use platform::ProtocolContract;

#[cfg(target_os = "naos")]
pub use platform::{
    EndpointResource, Request, Response, ServeHandler, Server, ServerError, ServiceDirectory,
};
