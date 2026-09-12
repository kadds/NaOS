//! NaOS async runtime facade.
//!
//! This is the platform seam used by native service binaries. The service
//! crates never need to know that Tokio is backed by a custom Mio selector;
//! they receive an ordinary Tokio runtime and a readiness future over NaOS
//! capability handles.

use crate::memory::MemoryObject;
use crate::sys;
use alloc::boxed::Box;
use alloc::string::String;
use alloc::vec::Vec;
use core::future::Future;
use core::time::Duration;
use naos_idl::service_directory;
use naos_idl::{
    CallError, CodecError, OwnedHandle, ProtocolClientEndpoint, ProtocolServerEndpoint,
    RawHandleGuard, ReceivedResources, ResourceError, ResourceSlot, ResourceTable,
};
pub(crate) use tokio::io::naos::{Event as ReadinessEvent, Readiness};

const LISTEN_MAX_PENDING: u64 = 16;
const CHANNEL_MAX_MESSAGE_BYTES: u64 = 65536;
/// Bytes one directory listing page can carry. The records travel in the
/// caller's region rather than the control message, so this is the window the
/// listing loop grants, not the channel message budget.
const SERVICE_DIRECTORY_PAGE_BYTES: u64 = CHANNEL_MAX_MESSAGE_BYTES;

/// Stable process bootstrap handles exposed to a NaOS service after the
/// custom `std` runtime has entered the ordinary Rust `main` function.
///
/// These are borrowed capability values owned by `naos-runtime`; copying the
/// raw handle numbers here does not transfer or duplicate ownership.  The
/// snapshot is intentionally plain data so an async service future never has
/// to carry a raw stack/bootstrap pointer across an await point.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Context {
    service_directory: sys::Handle,
    stdin_stream: sys::Handle,
    stdout_stream: sys::Handle,
    stderr_stream: sys::Handle,
}

impl Context {
    pub const fn service_directory(self) -> crate::server::ServiceDirectory {
        crate::server::ServiceDirectory::from_raw(self.service_directory)
    }

    pub const fn service_directory_handle(self) -> sys::Handle {
        self.service_directory
    }

    pub const fn stdin_stream(self) -> sys::Handle {
        self.stdin_stream
    }

    pub const fn stdout_stream(self) -> sys::Handle {
        self.stdout_stream
    }

    pub const fn stderr_stream(self) -> sys::Handle {
        self.stderr_stream
    }

    /// NaOS services receive their startup contract through the runtime
    /// bootstrap.  Host-only management flags intentionally have no meaning
    /// on a normal NaOS boot.
    pub const fn has_flag(self, _flag: &str) -> bool {
        false
    }
}

pub fn monotonic_ticks() -> u64 {
    let mut clock = sys::TimeClock::default();
    if unsafe { sys::_s_clock(1, &mut clock) } != 0 || clock.tv_sec < 0 || clock.tv_nsec < 0 {
        return 0;
    }
    let seconds = match u64::try_from(clock.tv_sec) {
        Ok(value) => value,
        Err(_) => return 0,
    };
    seconds
        .checked_mul(1_000_000)
        .and_then(|value| value.checked_add(u64::try_from(clock.tv_nsec).ok()? / 1_000))
        .unwrap_or(0)
}

pub fn wait_for_completion(handle: sys::Handle, timeout_us: u64) -> bool {
    naos_runtime::wait_for_completion(handle, timeout_us)
}

pub(crate) async fn next_readiness(
    sources: &[Readiness],
) -> tokio::io::Result<(usize, ReadinessEvent)> {
    std::future::poll_fn(|cx| {
        for (index, source) in sources.iter().enumerate() {
            match source.poll_readable(cx) {
                core::task::Poll::Pending => {}
                core::task::Poll::Ready(Ok(event)) => {
                    return core::task::Poll::Ready(Ok((index, event)));
                }
                core::task::Poll::Ready(Err(error)) => return core::task::Poll::Ready(Err(error)),
            }
        }
        core::task::Poll::Pending
    })
    .await
}

pub(crate) async fn next_readiness_refs(
    sources: &[&Readiness],
) -> tokio::io::Result<(usize, ReadinessEvent)> {
    std::future::poll_fn(|cx| {
        for (index, source) in sources.iter().enumerate() {
            match source.poll_readable(cx) {
                core::task::Poll::Pending => {}
                core::task::Poll::Ready(Ok(event)) => {
                    return core::task::Poll::Ready(Ok((index, event)));
                }
                core::task::Poll::Ready(Err(error)) => return core::task::Poll::Ready(Err(error)),
            }
        }
        core::task::Poll::Pending
    })
    .await
}

/// Persistent Tokio readiness registrations for a service's current endpoint
/// set.  The registration set is rebuilt only when the handle list changes;
/// waiting itself never creates an epoll object or re-registers every source.
pub struct ReadinessSet {
    handles: Vec<sys::Handle>,
    sources: Vec<Readiness>,
    pending_event: Option<(usize, ReadinessEvent)>,
}

impl ReadinessSet {
    pub fn new() -> Self {
        Self {
            handles: Vec::new(),
            sources: Vec::new(),
            pending_event: None,
        }
    }

    pub fn refresh(&mut self, handles: &[sys::Handle]) -> Result<(), sys::Status> {
        if self.handles.as_slice() == handles {
            return Ok(());
        }
        // Deregister the previous sources before registering a changed set.
        // Mio rejects a duplicate registration for a handle, and the old set
        // may share most of its handles with the new one.
        self.pending_event = None;
        self.sources.clear();
        self.handles.clear();
        let sources = handles
            .iter()
            .copied()
            .map(Readiness::new)
            .collect::<Result<Vec<_>, _>>()
            .map_err(|_| sys::STATUS_INVALID_ARGUMENT)?;
        self.handles.extend_from_slice(handles);
        self.sources = sources;
        Ok(())
    }

    pub async fn wait(&mut self, timeout_us: Option<u64>) -> Result<crate::ReadyEvent, sys::Status> {
        if self.sources.is_empty() {
            return Err(sys::STATUS_INVALID_ARGUMENT);
        }
        let (index, event) = match timeout_us {
            Some(timeout_us) => tokio::time::timeout(
                Duration::from_micros(timeout_us),
                next_readiness(&self.sources),
            )
            .await
            .map_err(|_| sys::STATUS_WAIT_TIMED_OUT)?
            .map_err(|_| sys::STATUS_IO_ERROR)?,
            None => next_readiness(&self.sources)
                .await
                .map_err(|_| sys::STATUS_IO_ERROR)?,
        };
        self.pending_event = Some((index, event));
        let ready = event.ready;
        Ok(crate::ReadyEvent {
            index,
            readable: ready.is_readable(),
            writable: ready.is_writable(),
            read_closed: ready.is_read_closed(),
            write_closed: ready.is_write_closed(),
            error: ready.is_error(),
        })
    }

    pub fn clear(&mut self, index: usize) {
        let Some((pending_index, event)) = self.pending_event.take() else {
            return;
        };
        if pending_index != index {
            self.pending_event = Some((pending_index, event));
            return;
        }
        self.sources[index].clear_readiness(event);
    }
}

pub fn wait_ready(
    handles: &[sys::Handle],
    timeout_us: Option<u64>,
) -> Result<crate::ReadyEvent, sys::Status> {
    let event = naos_runtime::wait_ready(handles, timeout_us)?;
    Ok(crate::ReadyEvent {
        index: event.index,
        readable: event.readable,
        writable: event.writable,
        read_closed: event.read_closed,
        write_closed: event.write_closed,
        error: event.error,
    })
}

pub async fn wait_ready_async(
    handles: &[sys::Handle],
    timeout_us: Option<u64>,
) -> Result<crate::ReadyEvent, sys::Status> {
    if handles.is_empty() {
        return Err(sys::STATUS_INVALID_ARGUMENT);
    }
    let sources = handles
        .iter()
        .copied()
        .map(Readiness::new)
        .collect::<Result<Vec<_>, _>>()
        .map_err(|_| sys::STATUS_INVALID_ARGUMENT)?;
    let (index, event) = match timeout_us {
        Some(timeout_us) => tokio::time::timeout(
            Duration::from_micros(timeout_us),
            next_readiness(&sources),
        )
        .await
        .map_err(|_| sys::STATUS_WAIT_TIMED_OUT)?
        .map_err(|_| sys::STATUS_IO_ERROR)?,
        None => next_readiness(&sources)
            .await
            .map_err(|_| sys::STATUS_IO_ERROR)?,
    };
    let ready = event.ready;
    Ok(crate::ReadyEvent {
        index,
        readable: ready.is_readable(),
        writable: ready.is_writable(),
        read_closed: ready.is_read_closed(),
        write_closed: ready.is_write_closed(),
        error: ready.is_error(),
    })
}

/// Move the normal process root/cwd bindings into NaOS `std` initialization.
/// Early services have invalid root/cwd handles and never call this method.
pub fn take_root_and_current() -> Option<(sys::Handle, sys::Handle)> {
    naos_runtime::with_context(|_, bootstrap| {
        (
            bootstrap.take_root_directory(),
            bootstrap.take_current_directory(),
        )
    })
}

/// An owned NaOS channel endpoint.
///
/// Channel frames are deliberately built here instead of in service
/// processes.  In particular, [`send`] consumes the resource table: MOVE
/// resources are committed only after the kernel accepts the message, and a
/// failed send leaves the table to close its owned handles normally.
pub struct Channel(OwnedHandle);

impl Channel {
    pub fn create(options: Option<&sys::ChannelOptions>) -> Result<(Self, Self), sys::Status> {
        let (left, right) = naos_idl::ChannelEndpoint::create(options)?;
        Ok((Self::from_endpoint(left), Self::from_endpoint(right)))
    }

    fn from_endpoint(endpoint: naos_idl::ChannelEndpoint) -> Self {
        let raw = endpoint.into_raw();
        // SAFETY: `ChannelEndpoint` owns a valid channel endpoint and
        // `into_raw` relinquishes that ownership exactly once.
        Self(unsafe { OwnedHandle::from_raw(raw) })
    }

    pub fn get(&self) -> sys::Handle {
        self.0.get()
    }

    pub fn into_owned(self) -> OwnedHandle {
        let raw = self.0.into_raw();
        // SAFETY: the raw endpoint was owned by `self` and was invalidated by
        // `into_raw`; ownership is transferred to the returned wrapper.
        unsafe { OwnedHandle::from_raw(raw) }
    }

    /// Send one byte payload and its capability resources.
    pub fn send(&self, bytes: &[u8], resources: ResourceTable<'_>) -> Result<(), sys::Status> {
        let frame = sys::ChannelSendFrame {
            struct_size: core::mem::size_of::<sys::ChannelSendFrame>() as u32,
            flags: 0,
            bytes: if bytes.is_empty() {
                0
            } else {
                bytes.as_ptr() as u64
            },
            byte_count: bytes.len() as u64,
            resources: if resources.is_empty() {
                0
            } else {
                resources.as_slice().as_ptr() as u64
            },
            resource_count: resources.len() as u64,
            reserved0: 0,
            reserved1: 0,
        };
        let status = unsafe { sys::_na_channel_send(self.get(), &frame) };
        if status == sys::STATUS_OK {
            resources.commit_move();
            Ok(())
        } else {
            Err(status)
        }
    }
}

/// Accept one protocol server endpoint from a persistent service listener.
///
/// The listener itself is owned by the ServiceDirectory. Each successful
/// connect creates a fresh client/server pair; this receive end owns the
/// server half until the peer closes it.
pub fn accept_listener(listener: &OwnedHandle) -> Result<ProtocolServerEndpoint, CallError> {
    let mut raw_resources = [sys::HANDLE_INVALID; naos_idl::MAX_RESOURCES];
    let mut frame = sys::ChannelReceiveFrame {
        struct_size: core::mem::size_of::<sys::ChannelReceiveFrame>() as u32,
        flags: 0,
        method_id: 0,
        bytes: 0,
        byte_capacity: 0,
        resources: raw_resources.as_mut_ptr() as u64,
        resource_capacity: naos_idl::MAX_RESOURCES as u64,
        responder: sys::HANDLE_INVALID,
        actual_bytes: 0,
        actual_resources: 0,
        required_bytes: 0,
        required_resources: 0,
        caller_pid: 0,
    };
    let status = unsafe { sys::_na_channel_receive(listener.get(), &mut frame) };
    if status != sys::STATUS_OK {
        return Err(CallError::Status(status));
    }
    if frame.actual_bytes != 0
        || frame.responder != sys::HANDLE_INVALID
        || frame.actual_resources != 1
    {
        for handle in raw_resources
            .iter()
            .take(frame.actual_resources as usize)
            .copied()
        {
            if handle != sys::HANDLE_INVALID {
                let _ = unsafe { sys::_na_handle_close(handle) };
            }
        }
        return Err(CallError::Codec(CodecError::InvalidResource));
    }
    let mut guard = RawHandleGuard::new(&raw_resources, frame.actual_resources as usize)
        .ok_or(CallError::Codec(CodecError::BoundExceeded))?;
    let mut resources = match unsafe {
        ReceivedResources::from_raw(&raw_resources[..frame.actual_resources as usize])
    } {
        Ok(resources) => {
            guard.disarm();
            resources
        }
        Err(error) => {
            // from_raw already closes rejected descriptors; disarm the
            // fallback guard to avoid a second close of the same handles.
            guard.disarm();
            return Err(CallError::Resource(error));
        }
    };
    let slot = ResourceSlot::new(0).ok_or(CallError::Resource(ResourceError::InvalidHandle))?;
    let endpoint = resources
        .take(slot)
        .ok_or(CallError::Resource(ResourceError::InvalidHandle))?;
    Ok(unsafe { ProtocolServerEndpoint::from_raw(endpoint.into_raw()) })
}

/// Publish a protocol listener through the kernel ServiceDirectory.
///
/// The returned handle is the receive end of the accept channel.  The
/// listener and protocol descriptor are owned by the ServiceDirectory after
/// a successful request; the service keeps only the receive endpoint.
pub fn publish_listener(
    service_directory_handle: sys::Handle,
    uri: &str,
    descriptor_value: &sys::ProtocolDescriptor,
) -> Result<OwnedHandle, sys::Status> {
    let mut duplicate = sys::HANDLE_INVALID;
    let status = unsafe { sys::_na_handle_duplicate(service_directory_handle, 0, &mut duplicate) };
    if status != sys::STATUS_OK || duplicate == sys::HANDLE_INVALID {
        return Err(status);
    }
    let directory = unsafe { ProtocolClientEndpoint::from_raw(duplicate) };

    let mut descriptor = sys::HANDLE_INVALID;
    let status = unsafe { sys::_na_protocol_descriptor_create(descriptor_value, &mut descriptor) };
    if status != sys::STATUS_OK || descriptor == sys::HANDLE_INVALID {
        return Err(status);
    }

    let options = sys::ChannelOptions {
        struct_size: core::mem::size_of::<sys::ChannelOptions>() as u32,
        flags: 0,
        max_messages: LISTEN_MAX_PENDING * 4,
        max_bytes: CHANNEL_MAX_MESSAGE_BYTES,
        max_resources: 64,
        reserved0: 0,
    };
    let (receive, send) = Channel::create(Some(&options))?;

    let mut resources = ResourceTable::new();
    let listener = resources
        .push_move(send.into_owned())
        .map_err(|_| sys::STATUS_RESOURCE_EXHAUSTED)?;
    let descriptor = resources
        .push_move(unsafe { OwnedHandle::from_raw(descriptor) })
        .map_err(|_| sys::STATUS_RESOURCE_EXHAUSTED)?;
    let request = service_directory::listen_request {
        max_pending: LISTEN_MAX_PENDING,
        listener,
        descriptor,
        uri,
    };
    let mut request_wire = [0_u8; 512];
    let mut invocation =
        service_directory::submit_listen(&directory, &request, resources, &mut request_wire, 0)
            .map_err(|_| sys::STATUS_IO_ERROR)?;
    if !wait_for_completion(invocation.get(), u64::MAX) {
        return Err(sys::STATUS_PEER_CLOSED);
    }
    let mut response_wire = [0_u8; 64];
    service_directory::take_listen(&mut invocation, &mut response_wire)
        .map_err(|_| sys::STATUS_IO_ERROR)?;
    Ok(receive.into_owned())
}

/// Register an already-created service endpoint under a URI.
pub fn register_service(
    service_directory_handle: sys::Handle,
    uri: &str,
    service: OwnedHandle,
) -> Result<(), sys::Status> {
    let mut duplicate = sys::HANDLE_INVALID;
    let status = unsafe { sys::_na_handle_duplicate(service_directory_handle, 0, &mut duplicate) };
    if status != sys::STATUS_OK || duplicate == sys::HANDLE_INVALID {
        return Err(status);
    }
    let directory = unsafe { ProtocolClientEndpoint::from_raw(duplicate) };
    let mut resources = ResourceTable::new();
    let slot = resources
        .push_move(service)
        .map_err(|_| sys::STATUS_RESOURCE_EXHAUSTED)?;
    let request = service_directory::register_request { service: slot, uri };
    let mut request_wire = [0_u8; 512];
    let mut invocation =
        service_directory::submit_register(&directory, &request, resources, &mut request_wire, 0)
            .map_err(|_| sys::STATUS_IO_ERROR)?;
    if !wait_for_completion(invocation.get(), u64::MAX) {
        return Err(sys::STATUS_PEER_CLOSED);
    }
    let mut response_wire = [0_u8; 64];
    service_directory::take_register(&mut invocation, &mut response_wire)
        .map(|_| ())
        .map_err(|_| sys::STATUS_IO_ERROR)
}

/// Resolve a protocol endpoint from the process' ServiceDirectory.
///
/// Service publication is intentionally asynchronous with respect to boot:
/// a consumer may start before its provider has registered. Retry only the
/// directory's not-found result and yield between attempts; transport and
/// protocol failures remain fatal to the caller.
pub fn resolve_resource(
    service_directory: sys::Handle,
    uri: &str,
) -> Result<OwnedHandle, CallError> {
    let mut duplicate = sys::HANDLE_INVALID;
    let status = unsafe { sys::_na_handle_duplicate(service_directory, 0, &mut duplicate) };
    if status != sys::STATUS_OK || duplicate == sys::HANDLE_INVALID {
        return Err(CallError::Status(status));
    }
    let endpoint = unsafe { ProtocolClientEndpoint::from_raw(duplicate) };
    for _ in 0..200_u32 {
        let request = service_directory::resolve_request { uri };
        let mut request_wire = [0_u8; 512];
        let mut invocation = loop {
            match service_directory::submit_resolve(
                &endpoint,
                &request,
                ResourceTable::new(),
                &mut request_wire,
                0,
            ) {
                Ok(invocation) => break invocation,
                Err(CallError::Status(status)) if status == sys::STATUS_WOULD_BLOCK => {
                    unsafe { sys::_s_yield() };
                }
                Err(error) => return Err(error),
            }
        };
        if !wait_for_completion(invocation.get(), u64::MAX) {
            return Err(CallError::InvalidInvocation);
        }
        let mut response_wire = [0_u8; 512];
        match service_directory::take_resolve(&mut invocation, &mut response_wire) {
            Ok((response, mut resources)) => {
                let service = resources
                    .take(response.service)
                    .ok_or(CallError::Resource(ResourceError::InvalidHandle))?;
                return Ok(service);
            }
            Err(CallError::Outcome { protocol_error, .. })
                if protocol_error == -2 || protocol_error == 2 =>
            {
                unsafe { sys::_s_yield() };
            }
            Err(error) => return Err(error),
        }
    }
    Err(CallError::Status(sys::STATUS_WOULD_BLOCK))
}

fn resolve_resource_once(
    endpoint: &ProtocolClientEndpoint,
    uri: &str,
) -> Result<OwnedHandle, CallError> {
    let request = service_directory::resolve_request { uri };
    let mut request_wire = [0_u8; 512];
    let mut invocation = loop {
        match service_directory::submit_resolve(
            endpoint,
            &request,
            ResourceTable::new(),
            &mut request_wire,
            0,
        ) {
            Ok(invocation) => break invocation,
            Err(CallError::Status(status)) if status == sys::STATUS_WOULD_BLOCK => {
                unsafe { sys::_s_yield() };
            }
            Err(error) => return Err(error),
        }
    };

    if !wait_for_completion(invocation.get(), u64::MAX) {
        return Err(CallError::InvalidInvocation);
    }

    let mut response_wire = [0_u8; 512];
    let (response, mut resources) =
        service_directory::take_resolve(&mut invocation, &mut response_wire)?;
    resources
        .take(response.service)
        .ok_or(CallError::Resource(ResourceError::InvalidHandle))
}

fn list_prefix_once(
    endpoint: &ProtocolClientEndpoint,
    region: &MemoryObject,
    prefix: &str,
    offset: u64,
) -> Result<(u64, Vec<String>), CallError> {
    let window = region.size().unwrap_or(SERVICE_DIRECTORY_PAGE_BYTES) as usize;
    // Size the request from the actual prefix so this remains correct if the
    // kernel's URI bound is raised without forcing a large stack buffer into
    // every service.
    let mut request_wire =
        Vec::with_capacity(service_directory::LIST_PREFIX_REQUEST_HEADER_BYTES + prefix.len());
    request_wire.resize(
        service_directory::LIST_PREFIX_REQUEST_HEADER_BYTES + prefix.len(),
        0,
    );
    let mut resources = ResourceTable::new();
    let buffer = resources
        .push_duplicate(region.as_handle())
        .map_err(CallError::Resource)?;
    let request = service_directory::list_prefix_request {
        offset,
        requested_bytes: window as u64,
        prefix,
        buffer,
    };
    let mut invocation = service_directory::submit_list_prefix(
        endpoint,
        &request,
        resources,
        &mut request_wire,
        0,
    )?;

    if !wait_for_completion(invocation.get(), u64::MAX) {
        return Err(CallError::InvalidInvocation);
    }

    // The reply is the generated fixed header alone: the directory wrote its
    // records into the region this loop owns.
    let mut response_wire = [0_u8; 32];
    let response = service_directory::take_list_prefix(&mut invocation, &mut response_wire)?;
    if response.bytes > window as u64 {
        return Err(CallError::Codec(CodecError::BoundExceeded));
    }
    let mut records = Vec::new();
    if response.bytes != 0 {
        records.resize(response.bytes as usize, 0_u8);
        region
            .read_region(0, &mut records)
            .map_err(|_| CallError::Codec(CodecError::InvalidMessage))?;
    }
    let mut services = Vec::with_capacity(response.count as usize);
    if !records.is_empty() && records.last() != Some(&0) {
        return Err(CallError::Codec(CodecError::InvalidMessage));
    }
    let records = records.strip_suffix(&[0]).unwrap_or(&records);
    if !records.is_empty() {
        for record in records.split(|byte| *byte == 0) {
            if record.is_empty() {
                return Err(CallError::Codec(CodecError::InvalidMessage));
            }
            let uri = core::str::from_utf8(record)
                .map_err(|_| CallError::Codec(CodecError::InvalidUtf8))?;
            services.push(String::from(uri));
        }
    }
    if services.len() as u64 != response.count {
        return Err(CallError::Codec(CodecError::InvalidMessage));
    }
    Ok((response.next, services))
}

/// Region the directory writes one listing page into.
///
/// The directory data plane is a user-space region, so a listing owns a
/// page-sized window instead of staging records through the control message.
/// The window is thread-local and created on first use, so a boot-time
/// `wait_for_service_prefix` retry loop neither creates nor maps a fresh
/// object per attempt.  It is not shared between threads because a region is
/// not `Sync` and sharing one would serialize unrelated callers behind a lock
/// held across the invocation.
std::thread_local! {
    static LISTING_REGION: core::cell::RefCell<Option<MemoryObject>> =
        const { core::cell::RefCell::new(None) };
}

fn new_listing_region() -> Result<MemoryObject, CallError> {
    let bytes = usize::try_from(SERVICE_DIRECTORY_PAGE_BYTES)
        .map_err(|_| CallError::Status(sys::STATUS_INVALID_ARGUMENT))?;
    let region = MemoryObject::new(bytes).map_err(|_| CallError::Status(sys::STATUS_IO_ERROR))?;
    // The directory writes the records, so the mapping must be writable and
    // shared for them to be visible here.
    region
        .map_persistent(bytes, true)
        .map_err(|_| CallError::Status(sys::STATUS_IO_ERROR))?;
    Ok(region)
}

/// Return the current service URIs below a segment-bounded prefix.
///
/// The returned values are locators, not capabilities. Callers still resolve
/// an individual URI when they need an endpoint, which preserves the normal
/// capability transfer and one-shot semantics of the directory.
pub fn list_service_uris(
    service_directory: sys::Handle,
    prefix: &str,
) -> Result<Vec<String>, CallError> {
    let mut duplicate = sys::HANDLE_INVALID;
    let status = unsafe { sys::_na_handle_duplicate(service_directory, 0, &mut duplicate) };
    if status != sys::STATUS_OK || duplicate == sys::HANDLE_INVALID {
        return Err(CallError::Status(status));
    }
    let endpoint = unsafe { ProtocolClientEndpoint::from_raw(duplicate) };
    LISTING_REGION.with(|cell| {
        let mut slot = cell.borrow_mut();
        if slot.is_none() {
            *slot = Some(new_listing_region()?);
        }
        let Some(region) = slot.as_ref() else {
            return Err(CallError::Status(sys::STATUS_IO_ERROR));
        };
        let mut offset = 0;
        let mut services = Vec::new();
        loop {
            let (next, mut page) = list_prefix_once(&endpoint, region, prefix, offset)?;
            services.append(&mut page);
            if next == offset {
                return Ok(services);
            }
            if next < offset {
                return Err(CallError::Codec(CodecError::InvalidMessage));
            }
            offset = next;
        }
    })
}

/// Wait until at least one service is registered below `prefix`, then return
/// the matching URI snapshot. A prefix does not imply a fixed number of
/// providers, so the completion condition is intentionally "one or more";
/// callers that need a particular provider should use [`resolve_until`].
pub async fn wait_for_service_prefix(
    service_directory: sys::Handle,
    prefix: &str,
    timeout: Duration,
) -> Result<Vec<String>, CallError> {
    let result = tokio::time::timeout(timeout, async {
        loop {
            let services = list_service_uris(service_directory, prefix)?;
            if !services.is_empty() {
                return Ok(services);
            }
            tokio::time::sleep(Duration::from_millis(1)).await;
        }
    })
    .await;

    match result {
        Ok(result) => result,
        Err(_) => Err(CallError::Status(sys::STATUS_WAIT_TIMED_OUT)),
    }
}

/// Resolve a service while it is being brought up by another early service.
///
/// The directory intentionally reports a normal not-found protocol outcome
/// until the provider registers its URI.  This helper waits asynchronously
/// between attempts and applies one caller-owned deadline; it does not encode
/// a retry count tied to scheduler speed or boot timing.
pub async fn resolve_until(
    service_directory: sys::Handle,
    uri: &str,
    timeout: Duration,
) -> Result<ProtocolClientEndpoint, CallError> {
    let mut duplicate = sys::HANDLE_INVALID;
    let status = unsafe { sys::_na_handle_duplicate(service_directory, 0, &mut duplicate) };
    if status != sys::STATUS_OK || duplicate == sys::HANDLE_INVALID {
        return Err(CallError::Status(status));
    }
    let endpoint = unsafe { ProtocolClientEndpoint::from_raw(duplicate) };

    let result = tokio::time::timeout(timeout, async {
        loop {
            match resolve_resource_once(&endpoint, uri) {
                Ok(resource) => {
                    return Ok(unsafe { ProtocolClientEndpoint::from_raw(resource.into_raw()) });
                }
                Err(CallError::Outcome { protocol_error, .. })
                    if protocol_error == -2 || protocol_error == 2 =>
                {
                    tokio::time::sleep(Duration::from_millis(1)).await;
                }
                Err(error) => return Err(error),
            }
        }
    })
    .await;

    match result {
        Ok(result) => result,
        Err(_) => Err(CallError::Status(sys::STATUS_WAIT_TIMED_OUT)),
    }
}

/// Resolve a ServiceDirectory entry expected to be a protocol endpoint.
pub fn resolve_service(
    service_directory: sys::Handle,
    uri: &str,
) -> Result<ProtocolClientEndpoint, CallError> {
    let resource = resolve_resource(service_directory, uri)?;
    Ok(unsafe { ProtocolClientEndpoint::from_raw(resource.into_raw()) })
}

fn connect_service_once(
    directory: &ProtocolClientEndpoint,
    uri: &str,
    expected_uuid: [u8; 16],
    requested_rights: u64,
    requested_revision: u64,
    requested_features: u64,
) -> Result<ProtocolClientEndpoint, CallError> {
    let request = service_directory::connect_request {
        expected_uuid,
        requested_rights,
        requested_revision,
        requested_features,
        uri,
    };
    // The connect request is a fixed header plus the URI, so size the wire
    // from the actual value instead of a fixed channel-budget buffer.
    let mut request_wire = Vec::new();
    request_wire.resize(
        service_directory::CONNECT_REQUEST_HEADER_BYTES + uri.len(),
        0_u8,
    );
    let mut invocation = service_directory::submit_connect(
        directory,
        &request,
        ResourceTable::new(),
        &mut request_wire,
        0,
    )?;
    if !wait_for_completion(invocation.get(), u64::MAX) {
        return Err(CallError::InvalidInvocation);
    }
    let mut response_wire = [0_u8; 128];
    let (response, mut resources) =
        service_directory::take_connect(&mut invocation, &mut response_wire)?;
    let client = resources
        .take(response.client)
        .ok_or(CallError::Resource(ResourceError::InvalidHandle))?;
    Ok(unsafe { ProtocolClientEndpoint::from_raw(client.into_raw()) })
}

/// Connect to a persistent protocol listener while the provider is starting.
/// The ServiceDirectory keeps the listener registered; every successful call
/// returns a new single-owner client endpoint.
pub async fn connect_until(
    service_directory: sys::Handle,
    uri: &str,
    expected_uuid: [u8; 16],
    requested_rights: u64,
    requested_revision: u64,
    requested_features: u64,
    timeout: Duration,
) -> Result<ProtocolClientEndpoint, CallError> {
    let mut duplicate = sys::HANDLE_INVALID;
    let status = unsafe { sys::_na_handle_duplicate(service_directory, 0, &mut duplicate) };
    if status != sys::STATUS_OK || duplicate == sys::HANDLE_INVALID {
        return Err(CallError::Status(status));
    }
    let endpoint = unsafe { ProtocolClientEndpoint::from_raw(duplicate) };
    let result = tokio::time::timeout(timeout, async {
        loop {
            match connect_service_once(
                &endpoint,
                uri,
                expected_uuid,
                requested_rights,
                requested_revision,
                requested_features,
            ) {
                Ok(resource) => return Ok(resource),
                Err(CallError::Outcome { protocol_error, .. })
                    if protocol_error == -2 || protocol_error == 2 =>
                {
                    tokio::time::sleep(Duration::from_millis(1)).await;
                }
                Err(error) => return Err(error),
            }
        }
    })
    .await;
    match result {
        Ok(result) => result,
        Err(_) => Err(CallError::Status(sys::STATUS_WAIT_TIMED_OUT)),
    }
}

/// Run a NaOS service future from the compiler-generated `#[tokio::main]`
/// function.
///
/// The NaOS runtime has already parsed the initial stack and installed the
/// It installs the logger before polling the future. The service itself gets
/// bootstrap handles through [`context`], never through raw `stack` or
/// `bootstrap` parameters.
pub async fn run<F, Fut>(tag: &'static str, service: F) -> i64
where
    F: FnOnce(Context) -> Fut,
    Fut: Future<Output = i64>,
{
    let context = naos_runtime::with_context(|_, bootstrap| {
        crate::tidy_log::init(tag, bootstrap.stdout_stream().raw());
        // NaOS std has no host stderr panic sink.  Install the service logger
        // as the panic hook so a Rust abort remains diagnosable instead of
        // looking like an unexplained process exit with status 134.
        std::panic::set_hook(Box::new(|info| {
            log::error!("service panic: {info}");
        }));
        Context {
            service_directory: bootstrap.service_directory().raw(),
            stdin_stream: bootstrap.stdin_stream().raw(),
            stdout_stream: bootstrap.stdout_stream().raw(),
            stderr_stream: bootstrap.stderr_stream().raw(),
        }
    });
    let Some(context) = context else {
        return 1;
    };
    service(context).await
}
