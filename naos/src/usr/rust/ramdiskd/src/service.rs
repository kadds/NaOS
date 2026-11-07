//! Transport-neutral ramdisk service.
//!
//! The service owns the same protocol state machine on Linux and NaOS. The
//! servicekit server facade supplies either UDS frames or capability-channel
//! requests, including the platform-specific MemoryObject data path.

use std::vec::Vec;

use naos_idl::ResourceSlot;
use naos_idl::block_device as device_idl;
use naos_idl::block_device_factory as factory_idl;
use naos_idl::transport::{RpcDispatchResult, RpcMethodReply, RpcOutcome};
use ramdiskd::core::{
    BLOCK_SIZE, DeviceLease, MAX_IN_FLIGHT, MAX_TRANSFER_BLOCKS, RIGHT_BLOCK_DISCARD,
    RIGHT_BLOCK_FLUSH, RIGHT_BLOCK_INSPECT, RIGHT_BLOCK_READ, RIGHT_BLOCK_WRITE, RamDisk,
};
use servicekit::Context;
use servicekit::boot;
#[cfg(target_os = "linux")]
use servicekit::boot::BootError;
use servicekit::server::{EndpointResource, Request, Response, ServeHandler, Server, ServerError};

#[cfg(target_os = "linux")]
const DEFAULT_BYTES: usize = 69_632 * 512;
const ERR_EINVAL: i64 = -22;
const ERR_EIO: i64 = -5;
const ERR_EACCES: i64 = -13;

struct DeviceLeaseBinding {
    target_id: u64,
    lease: DeviceLease,
}

/// Device ordering state: the admission ledger plus the progress signal.
///
/// Deliberately *not* part of [`Shared`]: admission and completion are decisions
/// about the device's ordering, not reads or writes of its bytes, and a waiter
/// must be able to examine them without holding the medium lock.  A plain
/// `Mutex` suffices because it is never held across an await.
struct Ordering {
    domain: std::sync::Mutex<ramdiskd::ordering::FlushDomain>,
    /// Signals that a write reached (or failed to reach) its completion point.
    ///
    /// A `watch` generation rather than a bare notify, because a waiter must not
    /// be able to miss the progress it is waiting for: a `watch` receiver
    /// remembers the version it last saw, so a completion that happens between
    /// "decide I have to wait" and "await progress" is still observed.
    progress: tokio::sync::watch::Sender<u64>,
}

impl Ordering {
    fn new() -> Self {
        let (progress, _) = tokio::sync::watch::channel(0_u64);
        Self {
            domain: std::sync::Mutex::new(ramdiskd::ordering::FlushDomain::new()),
            progress,
        }
    }

    fn with<R>(&self, body: impl FnOnce(&mut ramdiskd::ordering::FlushDomain) -> R) -> R {
        let mut domain = self
            .domain
            .lock()
            .unwrap_or_else(|error| error.into_inner());
        body(&mut domain)
    }

    /// Take the write's place in the device order.  Called before any side
    /// effect, so allocation order is admission order.
    fn admit_write(&self) -> u64 {
        self.with(ramdiskd::ordering::FlushDomain::admit_write)
    }

    fn admit_flush(&self) -> u64 {
        self.with(ramdiskd::ordering::FlushDomain::admit_flush)
    }

    fn flush_decision(&self, sequence: u64) -> ramdiskd::ordering::FlushDecision {
        self.with(|domain| domain.flush_decision(sequence))
    }

    fn oldest_outstanding(&self) -> Option<u64> {
        self.with(|domain| domain.oldest_outstanding())
    }

    /// Record where a write ended and announce the progress.
    ///
    /// The signal is what releases a `flush` waiting on this write, so it must
    /// follow the record: a woken waiter re-reads the ledger and would otherwise
    /// see the write still outstanding.
    fn complete_write(&self, sequence: u64, outcome: Result<(), i64>) {
        self.with(|domain| domain.complete_write(sequence, outcome));
        self.progress.send_modify(|generation| {
            *generation = generation.wrapping_add(1);
        });
    }

    /// A generation receiver that reports every later completion.
    ///
    /// Taken *before* the condition is examined, so a completion occurring
    /// between the check and the wait still advances the version the receiver
    /// compares against: the wait cannot miss it.
    fn progress_receiver(&self) -> tokio::sync::watch::Receiver<u64> {
        self.progress.subscribe()
    }
}

/// Medium bytes and their lease table.
///
/// `RwLock` rather than `Mutex` because observations are allowed to overlap: a
/// read takes the lock shared and runs its body on its own task, while a writer
/// takes it exclusively.  Lease bookkeeping rides the same lock so a lease
/// cannot be released while an operation holds it.
struct Shared {
    disk: RamDisk,
    devices: Vec<DeviceLeaseBinding>,
}

struct Manager {
    /// Behind an `Arc` so a guard can be *owned* and moved into the task that
    /// runs the request body.  Ownership is what lets the dispatcher take the
    /// guard in arrival order while the work itself proceeds concurrently.
    shared: std::sync::Arc<tokio::sync::RwLock<Shared>>,
    ordering: std::sync::Arc<Ordering>,
}

impl Clone for Manager {
    fn clone(&self) -> Self {
        Self {
            shared: std::sync::Arc::clone(&self.shared),
            ordering: std::sync::Arc::clone(&self.ordering),
        }
    }
}

/// Wait until this write may apply.
///
/// Writes overlap, so several may hold an admitted sequence at once; only the
/// oldest outstanding one applies.  That keeps the medium's view of writes in
/// admission order without serialising the dispatcher, and it is what lets a
/// `flush` observe a write that is still in flight.
async fn wait_write_turn(ordering: &Ordering, sequence: u64) {
    let mut progress = ordering.progress_receiver();
    loop {
        match ordering.oldest_outstanding() {
            // Our turn: nothing admitted earlier is still in flight.
            Some(oldest) if oldest == sequence => return,
            // Something earlier is still applying; wait for progress.
            Some(_) => {
                let _ = progress.changed().await;
            }
            // Our entry is gone, so the ordering no longer blocks us.  This
            // cannot happen for a write that completes exactly once, but
            // returning is safer than waiting on an already-satisfied condition.
            None => return,
        }
    }
}

/// Wait until every write admitted before `sequence` has reached its completion
/// point, or report why it cannot be satisfied.
///
/// Holding no lock while waiting is the point: the medium lock stays free, so
/// the write being waited for can still reach the device.  The receiver is taken
/// before the first decision, so a completion that happens between the decision
/// and the wait is observed rather than missed.
async fn await_flush(ordering: &Ordering, sequence: u64) -> Result<(), i64> {
    let mut progress = ordering.progress_receiver();
    loop {
        match ordering.flush_decision(sequence) {
            ramdiskd::ordering::FlushDecision::Satisfied => return Ok(()),
            // An earlier write never reached the device.  Waiting cannot restore
            // that durability, so the flush carries the errno instead.
            ramdiskd::ordering::FlushDecision::Failed { errno } => return Err(errno),
            ramdiskd::ordering::FlushDecision::Waiting { .. } => {
                let _ = progress.changed().await;
            }
        }
    }
}

impl Manager {
    fn new(disk: RamDisk) -> Self {
        Self {
            shared: std::sync::Arc::new(tokio::sync::RwLock::new(Shared {
                disk,
                devices: Vec::new(),
            })),
            ordering: std::sync::Arc::new(Ordering::new()),
        }
    }

    /// Take the shared lock, owned so it can cross a task boundary.
    ///
    /// The caller takes this in arrival order and the lock is fair, so the
    /// order in which guards are requested is the order in which they are
    /// granted.  Reads therefore overlap, while an exclusive guard waits for
    /// the reads admitted before it and blocks the reads that follow: block
    /// ordering holds without making the transport serial.
    async fn lock_shared(&self) -> tokio::sync::OwnedRwLockReadGuard<Shared> {
        std::sync::Arc::clone(&self.shared).read_owned().await
    }

    async fn lock_exclusive(&self) -> tokio::sync::OwnedRwLockWriteGuard<Shared> {
        std::sync::Arc::clone(&self.shared).write_owned().await
    }

    async fn reclaim_closed(&self, server: &mut Server) {
        let closed = server.take_closed_resources();
        if closed.is_empty() {
            return;
        }
        let mut shared = self.lock_exclusive().await;
        for resource in closed {
            if shared.disk.release(resource.resource_id) {
                shared
                    .devices
                    .retain(|device| device.target_id != resource.resource_id);
                log::debug!(
                    "released block lease resource={} after peer close",
                    resource.resource_id
                );
            }
        }
    }
}

fn fail(request: &Request, errno: i64) -> Response {
    Response::error(request, errno)
}

fn target_lease(
    shared: &Shared,
    request: &Request,
    required_rights: u64,
) -> Result<DeviceLease, i64> {
    let target = request.target.ok_or(ERR_EACCES)?;
    if !request.validate_resource(target) || target.rights & required_rights != required_rights {
        return Err(ERR_EACCES);
    }
    shared
        .devices
        .iter()
        .find(|device| device.target_id == target.resource_id)
        .map(|device| device.lease)
        .ok_or(ERR_EACCES)
}

/// Service-facing errno for a region access failure.
///
/// A window outside the region is a bad argument; a refusal the transport
/// enforced is a permission failure.  Both transports report the same
/// transport status for the same failure, so this maps identically on NaOS and
/// Linux instead of collapsing every case into EIO.
fn region_failure(error: ServerError) -> i64 {
    use servicekit::sys;
    match error {
        ServerError::Status(sys::STATUS_INVALID_ARGUMENT) => ERR_EINVAL,
        ServerError::Status(sys::STATUS_ACCESS_DENIED) => ERR_EACCES,
        _ => ERR_EIO,
    }
}

/// Read path: observes the medium only, so a shared lock is enough and the body
/// may run beside other read bodies.
fn serve_read_request(
    shared: &Shared,
    request: &Request,
    value: device_idl::read_request,
) -> Result<(), i64> {
    if value.flags != 0 {
        return Err(ERR_EINVAL);
    }
    let lease = target_lease(shared, request, RIGHT_BLOCK_READ)?;
    if value.block_count == 0 || value.block_count > MAX_TRANSFER_BLOCKS {
        return Err(ERR_EINVAL);
    }
    // The region admission decision covers which rights the caller granted and
    // which direction this operation needs, so a caller that did not grant the
    // required right is denied rather than reported as a malformed request.
    if !request.validate_memory(value.buffer, false) {
        return Err(ERR_EACCES);
    }
    let bytes = value
        .block_count
        .checked_mul(BLOCK_SIZE)
        .ok_or(ERR_EINVAL)?;
    let length = usize::try_from(bytes).map_err(|_| ERR_EINVAL)?;
    request
        .with_write_memory(value.buffer, 0, length, |data| {
            shared
                .disk
                .read_blocks(lease, value.lba, value.block_count, 0, data)
        })
        .map_err(region_failure)??;
    Ok(())
}

/// A write that has been authorised and given its place in the device order.
///
/// Separating this from the application is what lets the dispatcher admit a
/// write without holding the medium, so the write can wait for its turn and a
/// `flush` can observe it while it is still in flight.
struct PreparedWrite {
    lease: DeviceLease,
    lba: u64,
    block_count: u64,
    flags: u64,
    buffer: ResourceSlot,
    length: usize,
    sequence: u64,
}

/// Decode, authorise and admit a `write`.  No side effect happens here.
fn prepare_write(
    shared: &Shared,
    ordering: &Ordering,
    request: &Request,
    value: &device_idl::write_request,
) -> Result<PreparedWrite, i64> {
    let lease = target_lease(shared, request, RIGHT_BLOCK_WRITE)?;
    if value.block_count == 0 || value.block_count > MAX_TRANSFER_BLOCKS {
        return Err(ERR_EINVAL);
    }
    if !request.validate_memory(value.buffer, true) {
        return Err(ERR_EACCES);
    }
    let bytes = value
        .block_count
        .checked_mul(BLOCK_SIZE)
        .ok_or(ERR_EINVAL)?;
    let length = usize::try_from(bytes).map_err(|_| ERR_EINVAL)?;
    // Admitted: this is the write's place in the device order, taken before any
    // side effect so a later flush can name exactly the writes it must observe.
    let sequence = ordering.admit_write();
    Ok(PreparedWrite {
        lease,
        lba: value.lba,
        block_count: value.block_count,
        flags: value.flags,
        buffer: value.buffer,
        length,
        sequence,
    })
}

/// Apply an admitted write to the medium.
///
/// Recording where it ended is the caller's step, because the medium lock must
/// be released first: a `flush` woken by the record would otherwise race for a
/// lock this write still holds.
fn apply_write(
    shared: &mut Shared,
    request: &Request,
    prepared: &PreparedWrite,
) -> Result<(), i64> {
    request
        .with_read_memory(prepared.buffer, 0, prepared.length, |data| {
            shared.disk.write_blocks(
                prepared.lease,
                prepared.lba,
                prepared.block_count,
                prepared.flags,
                data,
            )
        })
        .map_err(region_failure)
        .and_then(|result| result)
}

/// Authorise a `flush` and give it its place in the device order.
fn prepare_flush(
    shared: &Shared,
    ordering: &Ordering,
    request: &Request,
    _value: &device_idl::flush_request,
) -> Result<(DeviceLease, u64), i64> {
    if !request.payload.is_empty() || !request.resources.is_empty() || !request.bulk.is_empty() {
        return Err(ERR_EINVAL);
    }
    let lease = target_lease(shared, request, RIGHT_BLOCK_FLUSH)?;
    // A flush shares the writes' sequence space, which is what makes "the
    // writes admitted before it" a well-defined set.
    Ok((lease, ordering.admit_flush()))
}

fn service_outcome(protocol_error: i64) -> RpcOutcome {
    RpcOutcome {
        execution: 0,
        reason: 0,
        protocol_error,
    }
}

enum AdmissionPlan {
    Shared(tokio::sync::OwnedRwLockReadGuard<Shared>),
    Write(PreparedWrite),
    Flush(DeviceLease, u64),
    Exclusive(tokio::sync::OwnedRwLockWriteGuard<Shared>),
}

enum AdmittedCall {
    Factory(factory_idl::DecodedServiceKitRequest),
    Device(device_idl::DecodedServiceKitRequest),
}

struct RequestAdmission {
    plan: AdmissionPlan,
    call: AdmittedCall,
}

struct RamdiskAdmission<'a> {
    manager: &'a Manager,
}

impl factory_idl::BlockDeviceFactoryServiceKitAdmission<&Request> for RamdiskAdmission<'_> {
    type Admission = AdmissionPlan;

    async fn get_info(
        &mut self,
        _context: &mut &Request,
        _request: &factory_idl::get_info_request,
    ) -> Result<Option<Self::Admission>, RpcOutcome> {
        Ok(Some(AdmissionPlan::Shared(
            self.manager.lock_shared().await,
        )))
    }

    async fn acquire(
        &mut self,
        _context: &mut &Request,
        _request: &factory_idl::acquire_request,
    ) -> Result<Option<Self::Admission>, RpcOutcome> {
        Ok(Some(AdmissionPlan::Exclusive(
            self.manager.lock_exclusive().await,
        )))
    }
}

impl device_idl::BlockDeviceServiceKitAdmission<&Request> for RamdiskAdmission<'_> {
    type Admission = AdmissionPlan;

    async fn get_info(
        &mut self,
        _context: &mut &Request,
        _request: &device_idl::get_info_request,
    ) -> Result<Option<Self::Admission>, RpcOutcome> {
        Ok(Some(AdmissionPlan::Shared(
            self.manager.lock_shared().await,
        )))
    }

    async fn read(
        &mut self,
        _context: &mut &Request,
        _request: &device_idl::read_request,
    ) -> Result<Option<Self::Admission>, RpcOutcome> {
        Ok(Some(AdmissionPlan::Shared(
            self.manager.lock_shared().await,
        )))
    }

    async fn write(
        &mut self,
        context: &mut &Request,
        request: &device_idl::write_request,
    ) -> Result<Option<Self::Admission>, RpcOutcome> {
        let prepared = {
            let shared = self.manager.lock_shared().await;
            prepare_write(&shared, &self.manager.ordering, *context, request)
        }
        .map_err(service_outcome)?;
        Ok(Some(AdmissionPlan::Write(prepared)))
    }

    async fn flush(
        &mut self,
        context: &mut &Request,
        request: &device_idl::flush_request,
    ) -> Result<Option<Self::Admission>, RpcOutcome> {
        let prepared = {
            let shared = self.manager.lock_shared().await;
            prepare_flush(&shared, &self.manager.ordering, *context, request)
        }
        .map_err(service_outcome)?;
        Ok(Some(AdmissionPlan::Flush(prepared.0, prepared.1)))
    }

    async fn discard(
        &mut self,
        _context: &mut &Request,
        _request: &device_idl::discard_request,
    ) -> Result<Option<Self::Admission>, RpcOutcome> {
        Ok(Some(AdmissionPlan::Exclusive(
            self.manager.lock_exclusive().await,
        )))
    }
}

async fn admit_request(
    manager: &Manager,
    request: &Request,
) -> Result<RequestAdmission, RpcOutcome> {
    let rpc_request = request.rpc_request();
    let mut context = request;
    let mut admission = RamdiskAdmission { manager };
    let result = if rpc_request.protocol_uuid == factory_idl::PROTOCOL_UUID {
        let decoded = factory_idl::decode_servicekit_request(&rpc_request)?;
        let plan =
            factory_idl::admit_servicekit_request(&decoded, &mut context, &mut admission).await?;
        Ok((AdmittedCall::Factory(decoded), plan))
    } else if rpc_request.protocol_uuid == device_idl::PROTOCOL_UUID {
        let decoded = device_idl::decode_servicekit_request(&rpc_request)?;
        let plan =
            device_idl::admit_servicekit_request(&decoded, &mut context, &mut admission).await?;
        Ok((AdmittedCall::Device(decoded), plan))
    } else {
        Err(service_outcome(ERR_EACCES))
    };
    let (call, plan) = result?;
    let plan = plan.ok_or_else(|| service_outcome(ERR_EIO))?;
    Ok(RequestAdmission { plan, call })
}

struct SharedProtocol<'a> {
    shared: &'a Shared,
}

impl factory_idl::BlockDeviceFactoryServiceKitHandler<&Request> for SharedProtocol<'_> {
    type Resource = EndpointResource;

    async fn get_info<'s>(
        &'s mut self,
        request: &mut &Request,
        _value: factory_idl::get_info_request,
    ) -> Result<RpcMethodReply<factory_idl::get_info_response, Self::Resource>, RpcOutcome> {
        if request.target.is_some() || !request.bulk.is_empty() {
            return Err(service_outcome(ERR_EINVAL));
        }
        Ok(RpcMethodReply::new(factory_idl::get_info_response {
            value: self.shared.disk.medium_info(),
        }))
    }

    async fn acquire<'s>(
        &'s mut self,
        _request: &mut &Request,
        _value: factory_idl::acquire_request,
    ) -> Result<RpcMethodReply<factory_idl::acquire_response, Self::Resource>, RpcOutcome> {
        Err(service_outcome(ERR_EACCES))
    }
}

impl device_idl::BlockDeviceServiceKitHandler<&Request> for SharedProtocol<'_> {
    type Resource = EndpointResource;

    async fn get_info<'s>(
        &'s mut self,
        request: &mut &Request,
        _value: device_idl::get_info_request,
    ) -> Result<RpcMethodReply<device_idl::get_info_response, Self::Resource>, RpcOutcome> {
        if !request.bulk.is_empty() {
            return Err(service_outcome(ERR_EINVAL));
        }
        let lease =
            target_lease(self.shared, request, RIGHT_BLOCK_INSPECT).map_err(service_outcome)?;
        Ok(RpcMethodReply::new(device_idl::get_info_response {
            value: self.shared.disk.block_info(lease),
        }))
    }

    async fn read<'s>(
        &'s mut self,
        request: &mut &Request,
        value: device_idl::read_request,
    ) -> Result<RpcMethodReply<device_idl::read_response, Self::Resource>, RpcOutcome> {
        serve_read_request(self.shared, request, value).map_err(service_outcome)?;
        Ok(RpcMethodReply::new(device_idl::read_response {}))
    }

    async fn write<'s>(
        &'s mut self,
        _request: &mut &Request,
        _value: device_idl::write_request,
    ) -> Result<RpcMethodReply<device_idl::write_response, Self::Resource>, RpcOutcome> {
        Err(service_outcome(ERR_EACCES))
    }

    async fn flush<'s>(
        &'s mut self,
        _request: &mut &Request,
        _value: device_idl::flush_request,
    ) -> Result<RpcMethodReply<device_idl::flush_response, Self::Resource>, RpcOutcome> {
        Err(service_outcome(ERR_EACCES))
    }

    async fn discard<'s>(
        &'s mut self,
        _request: &mut &Request,
        _value: device_idl::discard_request,
    ) -> Result<RpcMethodReply<device_idl::discard_response, Self::Resource>, RpcOutcome> {
        Err(service_outcome(ERR_EACCES))
    }
}

struct ExclusiveContext<'a> {
    request: &'a Request,
    server: &'a mut Server,
}

struct ExclusiveProtocol<'a> {
    shared: &'a mut Shared,
}

impl factory_idl::BlockDeviceFactoryServiceKitHandler<ExclusiveContext<'_>>
    for ExclusiveProtocol<'_>
{
    type Resource = EndpointResource;

    async fn get_info<'s>(
        &'s mut self,
        context: &mut ExclusiveContext<'_>,
        _value: factory_idl::get_info_request,
    ) -> Result<RpcMethodReply<factory_idl::get_info_response, Self::Resource>, RpcOutcome> {
        if context.request.target.is_some() || !context.request.bulk.is_empty() {
            return Err(service_outcome(ERR_EINVAL));
        }
        Ok(RpcMethodReply::new(factory_idl::get_info_response {
            value: self.shared.disk.medium_info(),
        }))
    }

    async fn acquire<'s>(
        &'s mut self,
        context: &mut ExclusiveContext<'_>,
        value: factory_idl::acquire_request,
    ) -> Result<RpcMethodReply<factory_idl::acquire_response, Self::Resource>, RpcOutcome> {
        if context.request.target.is_some()
            || !context.request.resources.is_empty()
            || !context.request.bulk.is_empty()
        {
            return Err(service_outcome(ERR_EINVAL));
        }
        let lease = self
            .shared
            .disk
            .acquire(value.start_lba, value.block_count, value.flags)
            .map_err(service_outcome)?;
        let resource = context
            .server
            .create_endpoint(
                &device_idl::protocol_descriptor(),
                0,
                device_idl::PROTOCOL_SCOPE,
                RIGHT_BLOCK_INSPECT
                    | RIGHT_BLOCK_READ
                    | if lease.read_only {
                        0
                    } else {
                        RIGHT_BLOCK_WRITE | RIGHT_BLOCK_FLUSH | RIGHT_BLOCK_DISCARD
                    },
                device_idl::validate_request_transport_resources,
            )
            .map_err(|_| service_outcome(ERR_EIO))?;
        self.shared.devices.push(DeviceLeaseBinding {
            target_id: resource.descriptor().resource_id,
            lease,
        });
        let mut reply = RpcMethodReply::new(factory_idl::acquire_response {
            device: ResourceSlot::new(0).expect("resource slot zero is valid"),
        });
        reply.resources.push(resource);
        Ok(reply)
    }
}

impl device_idl::BlockDeviceServiceKitHandler<ExclusiveContext<'_>> for ExclusiveProtocol<'_> {
    type Resource = EndpointResource;

    async fn get_info<'s>(
        &'s mut self,
        context: &mut ExclusiveContext<'_>,
        _value: device_idl::get_info_request,
    ) -> Result<RpcMethodReply<device_idl::get_info_response, Self::Resource>, RpcOutcome> {
        if !context.request.bulk.is_empty() {
            return Err(service_outcome(ERR_EINVAL));
        }
        let lease = target_lease(self.shared, context.request, RIGHT_BLOCK_INSPECT)
            .map_err(service_outcome)?;
        Ok(RpcMethodReply::new(device_idl::get_info_response {
            value: self.shared.disk.block_info(lease),
        }))
    }

    async fn read<'s>(
        &'s mut self,
        context: &mut ExclusiveContext<'_>,
        value: device_idl::read_request,
    ) -> Result<RpcMethodReply<device_idl::read_response, Self::Resource>, RpcOutcome> {
        serve_read_request(self.shared, context.request, value).map_err(service_outcome)?;
        Ok(RpcMethodReply::new(device_idl::read_response {}))
    }

    async fn write<'s>(
        &'s mut self,
        _context: &mut ExclusiveContext<'_>,
        _value: device_idl::write_request,
    ) -> Result<RpcMethodReply<device_idl::write_response, Self::Resource>, RpcOutcome> {
        Err(service_outcome(ERR_EACCES))
    }

    async fn flush<'s>(
        &'s mut self,
        _context: &mut ExclusiveContext<'_>,
        _value: device_idl::flush_request,
    ) -> Result<RpcMethodReply<device_idl::flush_response, Self::Resource>, RpcOutcome> {
        Err(service_outcome(ERR_EACCES))
    }

    async fn discard<'s>(
        &'s mut self,
        context: &mut ExclusiveContext<'_>,
        value: device_idl::discard_request,
    ) -> Result<RpcMethodReply<device_idl::discard_response, Self::Resource>, RpcOutcome> {
        if !context.request.resources.is_empty() || !context.request.bulk.is_empty() {
            return Err(service_outcome(ERR_EINVAL));
        }
        let lease = target_lease(self.shared, context.request, RIGHT_BLOCK_DISCARD)
            .map_err(service_outcome)?;
        self.shared
            .disk
            .discard(lease, value.lba, value.block_count)
            .map_err(service_outcome)?;
        Ok(RpcMethodReply::new(device_idl::discard_response {}))
    }
}

async fn dispatch_shared(
    shared: &Shared,
    request: &Request,
    call: AdmittedCall,
) -> naos_idl::transport::RpcDispatchResult<EndpointResource> {
    let mut context = request;
    let mut handler = SharedProtocol { shared };
    match call {
        AdmittedCall::Factory(decoded) => {
            factory_idl::dispatch_servicekit_request(decoded, &mut context, &mut handler).await
        }
        AdmittedCall::Device(decoded) => {
            device_idl::dispatch_servicekit_request(decoded, &mut context, &mut handler).await
        }
    }
}

async fn dispatch_exclusive(
    shared: &mut Shared,
    server: &mut Server,
    request: &Request,
    call: AdmittedCall,
) -> naos_idl::transport::RpcDispatchResult<EndpointResource> {
    let mut context = ExclusiveContext { request, server };
    let mut handler = ExclusiveProtocol { shared };
    match call {
        AdmittedCall::Factory(decoded) => {
            factory_idl::dispatch_servicekit_request(decoded, &mut context, &mut handler).await
        }
        AdmittedCall::Device(decoded) => {
            device_idl::dispatch_servicekit_request(decoded, &mut context, &mut handler).await
        }
    }
}

/// Handle a request that only observes the medium.
///
/// Reachable methods are exactly the ones `lock_kind` routes here, so this
/// never mutates state and never needs the server.
async fn service(context: Context) -> i64 {
    let directory = context.service_directory();
    let mut server = match Server::publish(
        &directory,
        servicekit::uri::BLOCK_RAMDISK,
        &factory_idl::protocol_descriptor(),
        factory_idl::validate_request_transport_resources,
    )
    .await
    {
        Ok(server) => server,
        Err(error) => {
            log::error!("publish userland block service failed: {error:?}");
            return 1;
        }
    };
    // One number for the promise and the enforcement: the medium advertises it
    // and the transport refuses beyond it, so a client can size a pipeline
    // against a depth the service will actually honour.
    server.set_max_in_flight(u32::try_from(MAX_IN_FLIGHT).unwrap_or(1));
    #[cfg(target_os = "naos")]
    let disk = {
        let image = match boot::open_module(&context, servicekit::uri::BOOT_ROOT_IMAGE) {
            Ok(image) => image,
            Err(error) => {
                log::error!("prepared root image unavailable: {error:?}");
                return 1;
            }
        };
        let size = match image.size() {
            Some(size) => match usize::try_from(size) {
                Ok(size) => size,
                Err(_) => {
                    log::error!("prepared root image size is not representable");
                    return 1;
                }
            },
            None => {
                log::error!("prepared root image size is unavailable");
                return 1;
            }
        };
        match RamDisk::from_memory(image.into_handle(), size, false) {
            Some(disk) => disk,
            None => {
                log::error!("prepared root image is not block-aligned or is empty");
                return 1;
            }
        }
    };

    #[cfg(target_os = "linux")]
    let disk = match boot::read_module(&context, servicekit::uri::BOOT_ROOT_IMAGE) {
        Ok(image) => match RamDisk::from_bytes(image, false) {
            Some(disk) => disk,
            None => {
                log::error!("prepared root image is not block-aligned or is empty");
                return 1;
            }
        },
        Err(BootError::Unavailable) => {
            // Linux's host service runner has no Multiboot image.  Keep the
            // in-memory fixture for its transport tests; NaOS mounted-root
            // boot requires the published prepared image above.
            RamDisk::new(DEFAULT_BYTES, false)
        }
        Err(error) => {
            log::error!("prepared root image unavailable: {error:?}");
            return 1;
        }
    };
    let medium = disk.medium_info();
    log::info!(
        "userland ramdisk ready id={} blocks={} logical={} max_in_flight={}",
        medium.medium_id,
        medium.total_block_count,
        medium.logical_block_bytes,
        medium.max_in_flight
    );
    log::info!(
        "block service admission bound={} in_flight={} refused={}",
        server.max_in_flight(),
        server.in_flight(),
        server.refused_requests()
    );
    log::info!(
        "block service published at {}",
        servicekit::uri::BLOCK_RAMDISK
    );
    log::info!("ready: userland block manager");

    let manager = Manager::new(disk);
    let mut handler = BlockHandler { manager };
    // The loop, admission and peer-fault policy come from servicekit; the
    // handler adds only what is specific to block I/O.
    let status = server.serve(&mut handler).await;
    status as i64
}

/// ramdiskd's per-request work.
struct BlockHandler {
    manager: Manager,
}

impl ServeHandler for BlockHandler {
    async fn handle(&mut self, server: &mut Server, request: Request) {
        self.manager.reclaim_closed(server).await;
        let admission = match admit_request(&self.manager, &request).await {
            Ok(admission) => admission,
            Err(outcome) => {
                let dispatched = RpcDispatchResult {
                    method_id: request.method_id,
                    outcome,
                    payload: Vec::new(),
                    resources: Vec::new(),
                };
                if let Err(error) = request.respond_dispatch(dispatched).await {
                    log::debug!("block service admission response failed: {error:?}");
                }
                self.manager.reclaim_closed(server).await;
                return;
            }
        };

        let RequestAdmission { plan, call } = admission;
        match plan {
            AdmissionPlan::Shared(guard) => {
                // The body owns its guard and its own reply, so observations
                // overlap and each is answered as soon as it is ready.  The
                // guard was taken in arrival order, so this cannot reorder
                // against a mutation.  Admission bounds how many may be
                // outstanding.
                tokio::spawn(async move {
                    let dispatched = dispatch_shared(&guard, &request, call).await;
                    if let Err(error) = request.respond_dispatch(dispatched).await {
                        log::debug!("block service response failed: {error:?}");
                    }
                });
            }
            AdmissionPlan::Write(prepared) => {
                drop(call);
                // The typed admission hook authorised the write and reserved
                // its place in device order before the request became a task.
                let manager = self.manager.clone();
                tokio::spawn(async move {
                    // Apply only when this is the oldest outstanding write, so
                    // the medium sees writes in admission order even though
                    // several are in flight.
                    wait_write_turn(&manager.ordering, prepared.sequence).await;
                    let outcome = {
                        let mut shared = manager.lock_exclusive().await;
                        apply_write(&mut shared, &request, &prepared)
                    };
                    // Record and announce only after the medium lock is released,
                    // so a waiter woken here cannot then block on the lock this
                    // write still holds.
                    manager.ordering.complete_write(prepared.sequence, outcome);
                    let response = match outcome {
                        Ok(()) => Response::success(&request, Vec::new()),
                        Err(errno) => fail(&request, errno),
                    };
                    if let Err(error) = request.respond(response).await {
                        log::debug!("block service response failed: {error:?}");
                    }
                });
            }
            AdmissionPlan::Flush(lease, sequence) => {
                drop(call);
                // Wait for the writes admitted before this flush, then act on
                // the medium alone.  The wait holds no medium lock, which is
                // what lets the write it is waiting for make progress.
                let verdict = await_flush(&self.manager.ordering, sequence).await;
                let outcome = match verdict {
                    Ok(()) => {
                        let mut shared = self.manager.lock_exclusive().await;
                        shared.disk.flush(lease)
                    }
                    Err(errno) => Err(errno),
                };
                let response = match outcome {
                    Ok(()) => Response::success(&request, Vec::new()),
                    Err(errno) => fail(&request, errno),
                };
                if let Err(error) = server.respond(request, response).await {
                    log::debug!("block service response failed: {error:?}");
                }
            }
            AdmissionPlan::Exclusive(mut guard) => {
                // A lease-state change must not overtake reads that are already
                // running, so it waits for the exclusive lock; because the reads
                // took their guards in arrival order, acquiring here both
                // follows them and precedes any later read.
                let dispatched = dispatch_exclusive(&mut guard, server, &request, call).await;
                drop(guard);
                if let Err(error) = request.respond_dispatch(dispatched).await {
                    log::debug!("block service response failed: {error:?}");
                }
            }
        }
        self.manager.reclaim_closed(server).await;
    }
}

pub async fn run(context: Context) -> i64 {
    service(context).await
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The data-plane acceptance distinguishes a denied region from a
    /// malformed one, so the errno mapping must not collapse both into EIO.
    #[test]
    fn region_failures_keep_their_service_errno() {
        assert_eq!(
            region_failure(ServerError::Status(
                servicekit::sys::STATUS_INVALID_ARGUMENT
            )),
            ERR_EINVAL
        );
        assert_eq!(
            region_failure(ServerError::Status(servicekit::sys::STATUS_ACCESS_DENIED)),
            ERR_EACCES
        );
        assert_eq!(region_failure(ServerError::Protocol), ERR_EIO);
        assert_eq!(region_failure(ServerError::PeerClosed), ERR_EIO);
    }

    /// A flush admitted while a write is still in flight must block until that
    /// write lands.
    ///
    /// This is the property the admission sequence exists for, and it is only
    /// observable once writes overlap: with a serial dispatch the flush could
    /// never be admitted while a write was outstanding.  The timing assertion is
    /// the evidence that the wait actually happened rather than the flush
    /// returning on a ledger that was already satisfied.
    #[tokio::test]
    async fn flush_waits_for_a_write_that_is_still_in_flight() {
        let ordering = std::sync::Arc::new(Ordering::new());
        let write = ordering.admit_write();
        let flush = ordering.admit_flush();
        // Admitted but not applied: the flush must not be satisfiable yet.
        assert!(matches!(
            ordering.flush_decision(flush),
            ramdiskd::ordering::FlushDecision::Waiting { .. }
        ));

        let writer = std::sync::Arc::clone(&ordering);
        tokio::spawn(async move {
            tokio::time::sleep(std::time::Duration::from_millis(60)).await;
            writer.complete_write(write, Ok(()));
        });

        let started = std::time::Instant::now();
        await_flush(&ordering, flush)
            .await
            .expect("the flush is satisfiable once the write lands");
        assert!(
            started.elapsed() >= std::time::Duration::from_millis(40),
            "flush returned before the write it must observe had landed"
        );
    }

    /// An earlier write that never reached the device cannot be waited for, so
    /// the flush must report its errno instead of claiming durability -- and it
    /// must not wait, because no completion is coming.
    #[tokio::test]
    async fn flush_reports_a_failed_earlier_write_without_waiting() {
        let ordering = Ordering::new();
        let write = ordering.admit_write();
        ordering.complete_write(write, Err(-19)); // ENODEV
        let flush = ordering.admit_flush();
        let started = std::time::Instant::now();
        assert_eq!(await_flush(&ordering, flush).await, Err(-19));
        assert!(
            started.elapsed() < std::time::Duration::from_millis(200),
            "a terminally failed write must not be waited for"
        );
    }

    /// Overlapping writes must still reach the medium in admission order, which
    /// is what the turn gate provides: a write may apply only once it is the
    /// oldest outstanding one.
    #[tokio::test]
    async fn a_write_waits_for_the_write_admitted_before_it() {
        let ordering = std::sync::Arc::new(Ordering::new());
        let first = ordering.admit_write();
        let second = ordering.admit_write();
        assert_eq!(ordering.oldest_outstanding(), Some(first));

        let writer = std::sync::Arc::clone(&ordering);
        tokio::spawn(async move {
            tokio::time::sleep(std::time::Duration::from_millis(60)).await;
            writer.complete_write(first, Ok(()));
        });

        let started = std::time::Instant::now();
        wait_write_turn(&ordering, second).await;
        assert!(
            started.elapsed() >= std::time::Duration::from_millis(40),
            "the later write applied before the earlier one completed"
        );
        assert_eq!(ordering.oldest_outstanding(), Some(second));
    }

    /// A flush admitted after every write already landed must not wait at all.
    #[tokio::test]
    async fn flush_does_not_wait_when_nothing_precedes_it() {
        let ordering = Ordering::new();
        let write = ordering.admit_write();
        ordering.complete_write(write, Ok(()));
        let flush = ordering.admit_flush();
        assert_eq!(await_flush(&ordering, flush).await, Ok(()));
    }
}
