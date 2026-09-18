//! Transport-neutral ramdisk service.
//!
//! The service owns the same protocol state machine on Linux and NaOS. The
//! servicekit server facade supplies either UDS frames or capability-channel
//! requests, including the platform-specific MemoryObject data path.

use std::vec::Vec;

use naos_idl::block_device as device_idl;
use naos_idl::block_device_factory as factory_idl;
use naos_idl::{CodecError, ResourceSlot};
use naos_idl::transport::RpcRequestOwned;
use ramdiskd::core::{
    DeviceLease, RamDisk, BLOCK_SIZE, MAX_IN_FLIGHT, MAX_TRANSFER_BLOCKS, RIGHT_BLOCK_DISCARD,
    RIGHT_BLOCK_FLUSH, RIGHT_BLOCK_INSPECT, RIGHT_BLOCK_READ, RIGHT_BLOCK_WRITE,
};
use servicekit::boot;
#[cfg(target_os = "linux")]
use servicekit::boot::BootError;
use servicekit::server::{Request, Response, ServeHandler, Server, ServerError};
use servicekit::Context;

#[cfg(target_os = "linux")]
const DEFAULT_BYTES: usize = 69_632 * 512;
const WIRE_BYTES: usize = 65_536;
const ERR_EINVAL: i64 = -22;
const ERR_EIO: i64 = -5;
const ERR_EACCES: i64 = -13;

struct DeviceLeaseBinding {
    target_id: u64,
    lease: DeviceLease,
}

fn validate_factory_request(request: &RpcRequestOwned) -> Result<(), i64> {
    match request.method_id {
        factory_idl::METHOD_GET_INFO => {
            let value = factory_idl::decode_get_info_request(&request.payload).map_err(|_| ERR_EINVAL)?;
            factory_idl::validate_get_info_request_transport_resources(&value, &request.resources)
                .map_err(|_| ERR_EINVAL)
        }
        factory_idl::METHOD_ACQUIRE => {
            let value = factory_idl::decode_acquire_request(&request.payload).map_err(|_| ERR_EINVAL)?;
            factory_idl::validate_acquire_request_transport_resources(&value, &request.resources)
                .map_err(|_| ERR_EINVAL)
        }
        _ => Err(-38),
    }
}

fn validate_device_request(request: &RpcRequestOwned) -> Result<(), i64> {
    match request.method_id {
        device_idl::METHOD_GET_INFO => {
            let value = device_idl::decode_get_info_request(&request.payload).map_err(|_| ERR_EINVAL)?;
            device_idl::validate_get_info_request_transport_resources(&value, &request.resources)
                .map_err(|_| ERR_EINVAL)
        }
        device_idl::METHOD_READ => {
            let value = device_idl::decode_read_request(&request.payload).map_err(|_| ERR_EINVAL)?;
            device_idl::validate_read_request_transport_resources(&value, &request.resources)
                .map_err(|_| ERR_EINVAL)
        }
        device_idl::METHOD_WRITE => {
            let value = device_idl::decode_write_request(&request.payload).map_err(|_| ERR_EINVAL)?;
            device_idl::validate_write_request_transport_resources(&value, &request.resources)
                .map_err(|_| ERR_EINVAL)
        }
        device_idl::METHOD_FLUSH => {
            let value = device_idl::decode_flush_request(&request.payload).map_err(|_| ERR_EINVAL)?;
            device_idl::validate_flush_request_transport_resources(&value, &request.resources)
                .map_err(|_| ERR_EINVAL)
        }
        device_idl::METHOD_DISCARD => {
            let value = device_idl::decode_discard_request(&request.payload).map_err(|_| ERR_EINVAL)?;
            device_idl::validate_discard_request_transport_resources(&value, &request.resources)
                .map_err(|_| ERR_EINVAL)
        }
        _ => Err(-38),
    }
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

/// Which lock a request needs.
///
/// Derived from the protocol and method, never from runtime state, so the
/// dispatcher can take the lock before any work starts and the acquisition
/// order is the arrival order.
enum LockKind {
    /// Observes the medium only; may run alongside other readers.
    Shared,
    /// Applies bytes: overlaps other writes but applies in admission order, so
    /// a `flush` admitted around it can wait for it on the ordering ledger.
    Write,
    /// Waits for the writes admitted before it, then acts on the medium alone.
    Flush,
    /// Reads or changes lease state under the medium lock for its whole
    /// duration; runs alone.
    Exclusive,
}

fn lock_kind(request: &Request) -> LockKind {
    if request.protocol_uuid == factory_idl::PROTOCOL_UUID {
        return if request.method_id == factory_idl::METHOD_ACQUIRE {
            LockKind::Exclusive
        } else {
            LockKind::Shared
        };
    }
    match request.method_id {
        device_idl::METHOD_GET_INFO | device_idl::METHOD_READ => LockKind::Shared,
        device_idl::METHOD_WRITE => LockKind::Write,
        device_idl::METHOD_FLUSH => LockKind::Flush,
        _ => LockKind::Exclusive,
    }
}

/// Whether a request mints an endpoint and therefore needs the server half.
fn needs_server(request: &Request) -> bool {
    request.protocol_uuid == factory_idl::PROTOCOL_UUID
        && request.method_id == factory_idl::METHOD_ACQUIRE
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

fn encoded(encode: impl FnOnce(&mut [u8]) -> Result<usize, CodecError>) -> Vec<u8> {
    let mut wire = vec![0; WIRE_BYTES];
    let length = encode(&mut wire).expect("generated response fits protocol limit");
    wire.truncate(length);
    wire
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
fn serve_read_request(shared: &Shared, request: &Request) -> Result<(), i64> {
    let value = device_idl::decode_read_request(&request.payload).map_err(|_| ERR_EINVAL)?;
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
    let bytes = value.block_count.checked_mul(BLOCK_SIZE).ok_or(ERR_EINVAL)?;
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
) -> Result<PreparedWrite, i64> {
    let value = device_idl::decode_write_request(&request.payload).map_err(|_| ERR_EINVAL)?;
    let lease = target_lease(shared, request, RIGHT_BLOCK_WRITE)?;
    if value.block_count == 0 || value.block_count > MAX_TRANSFER_BLOCKS {
        return Err(ERR_EINVAL);
    }
    if !request.validate_memory(value.buffer, true) {
        return Err(ERR_EACCES);
    }
    let bytes = value.block_count.checked_mul(BLOCK_SIZE).ok_or(ERR_EINVAL)?;
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
fn apply_write(shared: &mut Shared, request: &Request, prepared: &PreparedWrite) -> Result<(), i64> {
    request
        .with_read_memory(
            prepared.buffer,
            0,
            prepared.length,
            |data| {
                shared.disk.write_blocks(
                    prepared.lease,
                    prepared.lba,
                    prepared.block_count,
                    prepared.flags,
                    data,
                )
            },
        )
        .map_err(region_failure)
        .and_then(|result| result)
}

/// Authorise a `flush` and give it its place in the device order.
fn prepare_flush(
    shared: &Shared,
    ordering: &Ordering,
    request: &Request,
) -> Result<(DeviceLease, u64), i64> {
    if !request.payload.is_empty() || !request.resources.is_empty() || !request.bulk.is_empty() {
        return Err(ERR_EINVAL);
    }
    let lease = target_lease(shared, request, RIGHT_BLOCK_FLUSH)?;
    // A flush shares the writes' sequence space, which is what makes "the
    // writes admitted before it" a well-defined set.
    Ok((lease, ordering.admit_flush()))
}

/// Handle a request that only observes the medium.
///
/// Reachable methods are exactly the ones `lock_kind` routes here, so this
/// never mutates state and never needs the server.
fn dispatch_shared(shared: &Shared, request: &Request) -> Response {
    let expected_revision = if request.protocol_uuid == factory_idl::PROTOCOL_UUID {
        factory_idl::PROTOCOL_REVISION
    } else if request.protocol_uuid == device_idl::PROTOCOL_UUID {
        device_idl::PROTOCOL_REVISION
    } else {
        return fail(request, ERR_EACCES);
    };
    if request.revision != expected_revision {
        return fail(request, -71);
    }
    if request.protocol_uuid == factory_idl::PROTOCOL_UUID {
        if request.target.is_some() || !request.resources.is_empty() || !request.bulk.is_empty() {
            return fail(request, ERR_EINVAL);
        }
        return match request.method_id {
            factory_idl::METHOD_GET_INFO => {
                if !request.payload.is_empty() {
                    return fail(request, ERR_EINVAL);
                }
                let value = factory_idl::get_info_response {
                    value: shared.disk.medium_info(),
                };
                Response::success(
                    request,
                    encoded(|wire| factory_idl::encode_get_info_response(&value, wire)),
                )
            }
            _ => fail(request, -38),
        };
    }

    if request.protocol_uuid != device_idl::PROTOCOL_UUID || request.target.is_none() {
        return fail(request, ERR_EACCES);
    }
    match request.method_id {
        device_idl::METHOD_GET_INFO => {
            if !request.payload.is_empty()
                || !request.resources.is_empty()
                || !request.bulk.is_empty()
            {
                return fail(request, ERR_EINVAL);
            }
            let lease = match target_lease(shared, request, RIGHT_BLOCK_INSPECT) {
                Ok(lease) => lease,
                Err(errno) => return fail(request, errno),
            };
            let value = device_idl::get_info_response {
                value: shared.disk.block_info(lease),
            };
            Response::success(
                request,
                encoded(|wire| device_idl::encode_get_info_response(&value, wire)),
            )
        }
        device_idl::METHOD_READ => match serve_read_request(shared, request) {
            Ok(()) => Response::success(request, Vec::new()),
            Err(errno) => fail(request, errno),
        },
        // Unreachable through `lock_kind`.  Kept explicit so a method added to
        // the schema alone cannot silently land on a shared lock.
        _ => fail(request, ERR_EACCES),
    }
}

/// Handle a request that may change lease state or the medium.
///
/// `server` is needed only to mint an endpoint (`acquire`); the byte-level
/// mutations do not need it.  Every call runs under the exclusive lock, so the
/// order in which the dispatcher takes that lock is the order these observe.
fn dispatch_exclusive(
    shared: &mut Shared,
    server: Option<&mut Server>,
    request: &Request,
) -> Response {
    let expected_revision = if request.protocol_uuid == factory_idl::PROTOCOL_UUID {
        factory_idl::PROTOCOL_REVISION
    } else if request.protocol_uuid == device_idl::PROTOCOL_UUID {
        device_idl::PROTOCOL_REVISION
    } else {
        return fail(request, ERR_EACCES);
    };
    if request.revision != expected_revision {
        return fail(request, -71);
    }
    if request.protocol_uuid == factory_idl::PROTOCOL_UUID {
        if request.target.is_some() || !request.resources.is_empty() || !request.bulk.is_empty() {
            return fail(request, ERR_EINVAL);
        }
        return match request.method_id {
            factory_idl::METHOD_GET_INFO => {
                if !request.payload.is_empty() {
                    return fail(request, ERR_EINVAL);
                }
                let value = factory_idl::get_info_response {
                    value: shared.disk.medium_info(),
                };
                Response::success(
                    request,
                    encoded(|wire| factory_idl::encode_get_info_response(&value, wire)),
                )
            }
            factory_idl::METHOD_ACQUIRE => {
                let Ok(value) = factory_idl::decode_acquire_request(&request.payload) else {
                    return fail(request, ERR_EINVAL);
                };
                let Some(server) = server else {
                    return fail(request, ERR_EIO);
                };
                let lease =
                    match shared
                        .disk
                        .acquire(value.start_lba, value.block_count, value.flags)
                    {
                        Ok(lease) => lease,
                        Err(errno) => return fail(request, errno),
                    };
                let resource = match server.create_endpoint(
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
                    validate_device_request,
                ) {
                    Ok(resource) => resource,
                    Err(_) => return fail(request, ERR_EIO),
                };
                shared.devices.push(DeviceLeaseBinding {
                    target_id: resource.descriptor().resource_id,
                    lease,
                });
                let slot = ResourceSlot::new(0).expect("resource slot zero is valid");
                let value = factory_idl::acquire_response { device: slot };
                let mut response = Response::success(
                    request,
                    encoded(|wire| factory_idl::encode_acquire_response(&value, wire)),
                );
                response.push_resource(resource);
                response
            }
            _ => fail(request, -38),
        };
    }

    if request.protocol_uuid != device_idl::PROTOCOL_UUID || request.target.is_none() {
        return fail(request, ERR_EACCES);
    }
    match request.method_id {
        device_idl::METHOD_GET_INFO => {
            if !request.payload.is_empty()
                || !request.resources.is_empty()
                || !request.bulk.is_empty()
            {
                return fail(request, ERR_EINVAL);
            }
            let lease = match target_lease(shared, request, RIGHT_BLOCK_INSPECT) {
                Ok(lease) => lease,
                Err(errno) => return fail(request, errno),
            };
            let value = device_idl::get_info_response {
                value: shared.disk.block_info(lease),
            };
            Response::success(
                request,
                encoded(|wire| device_idl::encode_get_info_response(&value, wire)),
            )
        }
        device_idl::METHOD_READ => match serve_read_request(shared, request) {
            Ok(()) => Response::success(request, Vec::new()),
            Err(errno) => fail(request, errno),
        },
        device_idl::METHOD_WRITE | device_idl::METHOD_FLUSH => {
            // Routed to the overlapped write and ordering-aware flush paths,
            // which need the ledger before they take the medium; reaching them
            // here would apply a write outside the device order.
            fail(request, ERR_EACCES)
        }
        device_idl::METHOD_DISCARD => {
            let value = match device_idl::decode_discard_request(&request.payload) {
                Ok(value) => value,
                Err(_) => return fail(request, ERR_EINVAL),
            };
            if !request.resources.is_empty() || !request.bulk.is_empty() {
                return fail(request, ERR_EINVAL);
            }
            let lease = match target_lease(shared, request, RIGHT_BLOCK_DISCARD) {
                Ok(lease) => lease,
                Err(errno) => return fail(request, errno),
            };
            match shared.disk.discard(lease, value.lba, value.block_count) {
                Ok(()) => Response::success(request, Vec::new()),
                Err(errno) => fail(request, errno),
            }
        }
        _ => fail(request, -38),
    }
}

async fn service(context: Context) -> i64 {
    let directory = context.service_directory();
    let mut server = match Server::publish(
        &directory,
        servicekit::uri::BLOCK_RAMDISK,
        &factory_idl::protocol_descriptor(),
        validate_factory_request,
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
        match lock_kind(&request) {
            LockKind::Shared => {
                // The body owns its guard and its own reply, so observations
                // overlap and each is answered as soon as it is ready.  The
                // guard was taken in arrival order, so this cannot reorder
                // against a mutation.  Admission bounds how many may be
                // outstanding.
                let guard = self.manager.lock_shared().await;
                tokio::spawn(async move {
                    let response = dispatch_shared(&guard, &request);
                    if let Err(error) = request.respond(response).await {
                        log::debug!("block service response failed: {error:?}");
                    }
                });
            }
            LockKind::Write => {
                // Authorise and admit before any side effect, holding only a
                // share of the medium: the application below needs the medium
                // alone and may have to wait for its turn, which it cannot do
                // while the dispatcher holds the exclusive guard.
                let prepared = {
                    let shared = self.manager.lock_shared().await;
                    prepare_write(&shared, &self.manager.ordering, &request)
                };
                let prepared = match prepared {
                    Ok(prepared) => prepared,
                    Err(errno) => {
                        let response = fail(&request, errno);
                        if let Err(error) = server.respond(request, response).await {
                            log::debug!("block service response failed: {error:?}");
                        }
                        self.manager.reclaim_closed(server).await;
                        return;
                    }
                };
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
            LockKind::Flush => {
                let prepared = {
                    let shared = self.manager.lock_shared().await;
                    prepare_flush(&shared, &self.manager.ordering, &request)
                };
                let (lease, sequence) = match prepared {
                    Ok(prepared) => prepared,
                    Err(errno) => {
                        let response = fail(&request, errno);
                        if let Err(error) = server.respond(request, response).await {
                            log::debug!("block service response failed: {error:?}");
                        }
                        self.manager.reclaim_closed(server).await;
                        return;
                    }
                };
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
            LockKind::Exclusive => {
                // A lease-state change must not overtake reads that are already
                // running, so it waits for the exclusive lock; because the reads
                // took their guards in arrival order, acquiring here both
                // follows them and precedes any later read.
                let mut guard = self.manager.lock_exclusive().await;
                let server_arg = if needs_server(&request) {
                    Some(&mut *server)
                } else {
                    None
                };
                let response = dispatch_exclusive(&mut guard, server_arg, &request);
                drop(guard);
                if let Err(error) = server.respond(request, response).await {
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
            region_failure(ServerError::Status(servicekit::sys::STATUS_INVALID_ARGUMENT)),
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
