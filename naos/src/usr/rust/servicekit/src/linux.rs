//! Linux `std` + Tokio transport adapter.
//!
//! The frame format is intentionally small and boring: a length-prefixed
//! header followed by payload, opaque resource descriptors, and bulk
//! descriptors.  It is a local UDS protocol, not a network protocol.  The
//! daemon crates share this adapter and therefore do not grow `*_linux` and
//! `*_naos` copies of their state machines.

use std::borrow::ToOwned;
use std::boxed::Box;
use std::collections::BTreeMap;
use std::eprintln;
use std::io::{self, Read, Write};
use std::os::fd::AsRawFd;
use std::os::unix::fs::FileTypeExt;
use std::path::{Path, PathBuf};
use std::pin::Pin;
use std::string::String;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::vec;
use std::vec::Vec;

use core::future::{Future, Ready};
use core::time::Duration;
use naos_idl::transport::{
    BulkBuffer, BulkDirection, ResourceDescriptor, RpcOutcome, RpcRequest, RpcRequestOwned,
    RpcResponseOwned, RpcTransport, ServiceLocator,
};
use tokio::io::{AsyncRead, AsyncReadExt, AsyncWrite, AsyncWriteExt};
#[cfg(test)]
use tokio::net::UnixListener;
use tokio::net::UnixStream;

pub fn monotonic_ticks() -> u64 {
    0
}

pub fn wait_for_completion(_handle: naos_sys::Handle, _timeout_us: u64) -> bool {
    false
}

pub fn wait_ready(
    _handles: &[naos_sys::Handle],
    _timeout_us: Option<u64>,
) -> Result<crate::ReadyEvent, naos_sys::Status> {
    Err(naos_sys::STATUS_NOT_SUPPORTED)
}

pub async fn wait_ready_async(
    _handles: &[naos_sys::Handle],
    _timeout_us: Option<u64>,
) -> Result<crate::ReadyEvent, naos_sys::Status> {
    Err(naos_sys::STATUS_NOT_SUPPORTED)
}

/// Host placeholder for the NaOS Tokio readiness set.  Linux servicekit uses
/// the UDS server path, but keeping the same type available lets shared daemon
/// state compile on both targets without exposing a platform selector.
pub struct ReadinessSet;

impl ReadinessSet {
    pub fn new() -> Self {
        Self
    }

    pub fn refresh(&mut self, _handles: &[naos_sys::Handle]) -> Result<(), naos_sys::Status> {
        Ok(())
    }

    pub async fn wait(&mut self, _timeout_us: Option<u64>) -> Result<crate::ReadyEvent, naos_sys::Status> {
        Err(naos_sys::STATUS_NOT_SUPPORTED)
    }

    pub fn clear(&mut self, _index: usize) {}
}

/// Host process context exposed through the same servicekit entrypoint as
/// NaOS. The service directory is the only platform-specific value a daemon
/// needs for the common service server facade.
#[derive(Clone, Debug)]
pub struct Context {
    service_directory: crate::server::ServiceDirectory,
    flags: Vec<String>,
}

impl Context {
    pub fn service_directory(&self) -> crate::server::ServiceDirectory {
        self.service_directory.clone()
    }

    /// Return whether the service was started with an explicit test or
    /// management flag.  Ordinary daemons do not need to parse host command
    /// line arguments themselves.
    pub fn has_flag(&self, flag: &str) -> bool {
        self.flags.iter().any(|value| value == flag)
    }
}

/// Run a daemon after servicekit has initialized its platform logger and
/// service-directory context. The daemon future returns normally so Tokio can
/// shut down its worker threads before the process exit path runs.
pub async fn run<F, Fut>(tag: &'static str, service: F) -> i64
where
    F: FnOnce(Context) -> Fut,
    Fut: Future<Output = i64>,
{
    let mut service_root = None;
    let mut log_path = None;
    let mut flags = Vec::new();
    let mut args = std::env::args().skip(1);
    while let Some(arg) = args.next() {
        match arg.as_str() {
            "--service-dir" => service_root = args.next().map(PathBuf::from),
            "--log" => log_path = args.next().map(PathBuf::from),
            _ => flags.push(arg),
        }
    }
    if let Err(error) = init_log(tag, log_path.as_deref()) {
        eprintln!("{tag}: tidy_log initialization failed: {error}");
        return 1;
    }
    let service_directory =
        service_root.map_or_else(ServiceDirectory::default, ServiceDirectory::new);
    service(Context {
        service_directory,
        flags,
    })
    .await
}

#[path = "linux_bulk.rs"]
pub mod bulk;

const MAGIC: &[u8; 8] = b"NAORPC01";
const VERSION: u16 = 1;
const REQUEST: u16 = 1;
const RESPONSE: u16 = 2;
const HEADER_BYTES: usize = 80;
const RESOURCE_BYTES: usize = 48;
const BULK_BYTES: usize = 48;
const FRAME_FLAG_TARGET: u32 = 1;
pub const MAX_FRAME_BYTES: usize = 1 << 20;
pub const MAX_RESOURCES: usize = 64;
pub const MAX_BULK_BUFFERS: usize = 64;
pub const MAX_CONNECTIONS: usize = 64;
const SESSION_OWNER_TAG: u64 = 1 << 63;

fn resource_range_is_attenuated(original: ResourceDescriptor, candidate: ResourceDescriptor) -> bool {
    let memory = candidate.binding == crate::sys::BINDING_MEMORY_OBJECT
        && candidate.scope == naos_idl::memory_object::PROTOCOL_SCOPE;
    if !memory {
        return candidate.view_offset == 0 && candidate.view_length == 0;
    }
    if original.view_length == 0 {
        return candidate.view_offset == 0 && candidate.view_length == 0;
    }
    let Some(original_end) = original.view_offset.checked_add(original.view_length) else {
        return false;
    };
    let Some(candidate_end) = candidate.view_offset.checked_add(candidate.view_length) else {
        return false;
    };
    candidate.view_length != 0
        && candidate.view_offset >= original.view_offset
        && candidate_end <= original_end
}

fn session_owner(connection_id: u64) -> u64 {
    SESSION_OWNER_TAG | connection_id
}

fn process_owner() -> u64 {
    u64::from(std::process::id())
}

/// The Linux equivalent of a NaOS endpoint owner.  A UDS connection gets one
/// session, and endpoint resources returned on that connection are registered
/// here instead of being treated as process-global integers.  This gives the
/// host transport the same lifetime and attenuation boundary as a NaOS
/// capability endpoint without pretending that a Linux integer is a kernel
/// handle.
#[derive(Clone, Debug)]
pub struct LinuxServiceSession {
    connection_id: u64,
    peer_pid: u64,
    bulk_endpoint: Arc<PathBuf>,
    resources: Arc<Mutex<BTreeMap<u64, ResourceDescriptor>>>,
    authority: LinuxResourceAuthority,
    closed_resources: Arc<Mutex<Vec<ResourceDescriptor>>>,
    lifetime: Arc<()>,
}

/// Platform-neutral public name for the transport session.
pub type ServiceSession = LinuxServiceSession;

/// Authority for resources minted by one Linux service endpoint.
///
/// A descriptor on a UDS frame is not a kernel capability: a peer can write
/// any bytes it wants.  Keep the authoritative descriptor in the server and
/// accept only the same resource identity with an equal or attenuated rights
/// set.  The random namespace makes descriptor guessing materially harder;
/// the table is still the source of truth and not the numeric id itself.
#[derive(Clone, Debug)]
pub(crate) struct LinuxResourceAuthority {
    resources: Arc<Mutex<BTreeMap<u64, LinuxResourceRecord>>>,
    next_resource: Arc<AtomicU64>,
}

#[derive(Clone, Debug)]
struct LinuxResourceRecord {
    descriptor: ResourceDescriptor,
    owners: BTreeMap<u64, u64>,
    protocol: Option<LinuxProtocolRecord>,
}

#[derive(Clone, Debug)]
struct LinuxProtocolRecord {
    uuid: [u8; 16],
    revision: u64,
    max_request_bytes: u64,
    max_response_bytes: u64,
    max_resources: u64,
    method_count: u64,
    method_bitmap: [u64; 4],
    method_rights: [u64; 256],
    protocol_rights: u64,
    request_validator: crate::server::RequestValidator,
}

impl LinuxResourceAuthority {
    pub(crate) fn new() -> Self {
        let mut seed = 0_u64;
        let random_bytes = unsafe {
            libc::getrandom(
                (&mut seed as *mut u64).cast::<libc::c_void>(),
                core::mem::size_of::<u64>(),
                0,
            )
        };
        if random_bytes != core::mem::size_of::<u64>() as isize {
            seed = u64::from(std::process::id())
                ^ std::time::SystemTime::now()
                    .duration_since(std::time::UNIX_EPOCH)
                    .map_or(0, |duration| duration.as_nanos() as u64);
        }
        Self {
            resources: Arc::new(Mutex::new(BTreeMap::new())),
            next_resource: Arc::new(AtomicU64::new(seed | 1)),
        }
    }

    #[cfg(test)]
    pub(crate) fn mint(&self, binding: u32, scope: u64, rights: u64) -> ResourceDescriptor {
        self.mint_for(0, binding, scope, rights)
    }

    pub(crate) fn mint_for(
        &self,
        owner: u64,
        binding: u32,
        scope: u64,
        rights: u64,
    ) -> ResourceDescriptor {
        loop {
            let resource_id = self.next_resource.fetch_add(2, Ordering::Relaxed) | 1;
            let resource = ResourceDescriptor {
                resource_id,
                binding,
                scope,
                rights,
                ..ResourceDescriptor::default()
            };
            if let Ok(mut resources) = self.resources.lock() {
                if resources
                    .insert(
                        resource_id,
                        LinuxResourceRecord {
                            descriptor: resource,
                            owners: BTreeMap::from([(owner, 1)]),
                            protocol: None,
                        },
                    )
                    .is_none()
                {
                    return resource;
                }
            }
        }
    }

    pub(crate) fn mint_endpoint(
        &self,
        owner: u64,
        binding: u32,
        scope: u64,
        rights: u64,
        descriptor: &naos_sys::ProtocolDescriptor,
        request_validator: crate::server::RequestValidator,
    ) -> ResourceDescriptor {
        loop {
            let resource_id = self.next_resource.fetch_add(2, Ordering::Relaxed) | 1;
            let resource = ResourceDescriptor {
                resource_id,
                binding,
                scope,
                rights,
                ..ResourceDescriptor::default()
            };
            let protocol = LinuxProtocolRecord {
                uuid: descriptor.uuid.bytes,
                revision: descriptor.revision,
                max_request_bytes: descriptor.max_request_bytes,
                max_response_bytes: descriptor.max_response_bytes,
                max_resources: descriptor.max_resources,
                method_count: descriptor.method_count,
                method_bitmap: descriptor.method_bitmap,
                method_rights: descriptor.method_rights,
                protocol_rights: descriptor.protocol_rights,
                request_validator,
            };
            if let Ok(mut resources) = self.resources.lock() {
                if resources
                    .insert(
                        resource_id,
                        LinuxResourceRecord {
                            descriptor: resource,
                            owners: BTreeMap::from([(owner, 1)]),
                            protocol: Some(protocol),
                        },
                    )
                    .is_none()
                {
                    return resource;
                }
            }
        }
    }

    pub(crate) fn validate_target_protocol(
        &self,
        target: ResourceDescriptor,
        request: &RpcRequestOwned,
    ) -> Result<(), i64> {
        let Ok(resources) = self.resources.lock() else {
            return Err(-13);
        };
        let Some(record) = resources.get(&target.resource_id) else {
            return Err(-13);
        };
        let Some(protocol) = record.protocol.as_ref() else {
            return Err(-71);
        };
        if request.protocol_uuid != protocol.uuid
            || request.revision != protocol.revision
            || request.method_id == 0
            || request.method_id > protocol.method_count
            || (protocol.max_request_bytes != 0
                && request.payload.len() as u64 > protocol.max_request_bytes)
            || (protocol.max_resources != 0 && request.resources.len() as u64 > protocol.max_resources)
        {
            return Err(-71);
        }
        let index = request.method_id as usize - 1;
        let bit = 1_u64 << (index % 64);
        if protocol.method_bitmap[index / 64] & bit == 0 {
            return Err(-71);
        }
        let required = protocol.method_rights[index];
        if target.rights & required != required || required & !protocol.protocol_rights != 0 {
            return Err(-13);
        }
        (protocol.request_validator)(request)?;
        Ok(())
    }

    pub(crate) fn target_contract(
        &self,
        target: ResourceDescriptor,
    ) -> Option<crate::server::ProtocolContract> {
        let Ok(resources) = self.resources.lock() else {
            return None;
        };
        let record = resources.get(&target.resource_id)?;
        let protocol = record.protocol.as_ref()?;
        Some(crate::server::ProtocolContract {
            uuid: protocol.uuid,
            revision: protocol.revision,
            max_request_bytes: protocol.max_request_bytes,
            max_response_bytes: protocol.max_response_bytes,
            max_resources: protocol.max_resources,
            method_count: protocol.method_count,
            method_bitmap: protocol.method_bitmap,
            method_rights: protocol.method_rights,
            protocol_rights: protocol.protocol_rights,
        })
    }

    pub(crate) fn validate(&self, resource: ResourceDescriptor) -> bool {
        let Ok(resources) = self.resources.lock() else {
            return false;
        };
        let Some(original) = resources.get(&resource.resource_id) else {
            return false;
        };
        original.descriptor.binding == resource.binding
            && original.descriptor.scope == resource.scope
            && resource.rights != 0
            && resource.rights & !original.descriptor.rights == 0
            && resource_range_is_attenuated(original.descriptor, resource)
    }

    pub(crate) fn has_owner(&self, resource_id: u64, owner: u64) -> bool {
        let Ok(resources) = self.resources.lock() else {
            return false;
        };
        resources
            .get(&resource_id)
            .is_some_and(|record| record.owners.get(&owner).copied().unwrap_or(0) != 0)
    }

    pub(crate) fn retain_owner(&self, resource_id: u64, owner: u64) -> bool {
        let Ok(mut resources) = self.resources.lock() else {
            return false;
        };
        let Some(record) = resources.get_mut(&resource_id) else {
            return false;
        };
        let count = record.owners.entry(owner).or_insert(0);
        *count = count.saturating_add(1);
        true
    }

    pub(crate) fn release_owner(&self, owner: u64, resource_id: u64) -> Option<ResourceDescriptor> {
        let Ok(mut resources) = self.resources.lock() else {
            return None;
        };
        let Some(record) = resources.get_mut(&resource_id) else {
            return None;
        };
        let Some(count) = record.owners.get_mut(&owner) else {
            return None;
        };
        if *count > 1 {
            *count -= 1;
            return None;
        }
        record.owners.remove(&owner);
        if !record.owners.is_empty() {
            return None;
        }
        let is_endpoint = record.protocol.is_some();
        let record = resources.remove(&resource_id)?;
        is_endpoint.then_some(record.descriptor)
    }

    pub(crate) fn release_owner_many(&self, owner: u64, resource_ids: &[u64]) -> Vec<ResourceDescriptor> {
        let mut closed = Vec::new();
        for resource_id in resource_ids {
            if let Some(resource) = self.release_owner(owner, *resource_id) {
                closed.push(resource);
            }
        }
        closed
    }

    /// Reclaim process-scoped owners that disappeared without running their
    /// descriptor Drop callbacks.  Connection-scoped owners are deliberately
    /// excluded: their sessions perform exact per-connection cleanup.
    pub(crate) fn reap_dead_process_owners(&self) -> Vec<ResourceDescriptor> {
        let Ok(mut resources) = self.resources.lock() else {
            return Vec::new();
        };
        let mut dead_owners = Vec::new();
        for record in resources.values() {
            for owner in record.owners.keys().copied() {
                if owner != process_owner()
                    && owner & SESSION_OWNER_TAG == 0
                    && !process_is_alive(owner)
                    && !dead_owners.contains(&owner)
                {
                    dead_owners.push(owner);
                }
            }
        }
        if dead_owners.is_empty() {
            return Vec::new();
        }
        let mut closed = Vec::new();
        let mut remove_ids = Vec::new();
        for (resource_id, record) in resources.iter_mut() {
            for owner in &dead_owners {
                record.owners.remove(owner);
            }
            if record.owners.is_empty() {
                if record.protocol.is_some() {
                    closed.push(record.descriptor);
                }
                remove_ids.push(*resource_id);
            }
        }
        for resource_id in remove_ids {
            resources.remove(&resource_id);
        }
        closed
    }

    fn retain(&self, resource: ResourceDescriptor, owner: u64) -> bool {
        let Ok(mut resources) = self.resources.lock() else {
            return false;
        };
        let Some(original) = resources.get_mut(&resource.resource_id) else {
            return false;
        };
        if original.descriptor.binding != resource.binding
            || original.descriptor.scope != resource.scope
            || resource.rights == 0
            || resource.rights & !original.descriptor.rights != 0
            || !resource_range_is_attenuated(original.descriptor, resource)
        {
            return false;
        }
        let count = original.owners.entry(owner).or_insert(0);
        *count = count.saturating_add(1);
        true
    }

    fn import(&self, resource: ResourceDescriptor, owner: u64) -> bool {
        let Ok(mut resources) = self.resources.lock() else {
            return false;
        };
        match resources.get_mut(&resource.resource_id) {
            Some(original) => {
                if original.descriptor.binding != resource.binding
                    || original.descriptor.scope != resource.scope
                    || resource.rights & !original.descriptor.rights != 0
                    || !resource_range_is_attenuated(original.descriptor, resource)
                {
                    return false;
                }
                let count = original.owners.entry(owner).or_insert(0);
                *count = count.saturating_add(1);
            }
            None => {
                resources.insert(
                    resource.resource_id,
                    LinuxResourceRecord {
                        descriptor: resource,
                        owners: BTreeMap::from([(owner, 1)]),
                        protocol: None,
                    },
                );
            }
        }
        true
    }

}

impl LinuxServiceSession {
    #[cfg(test)]
    pub(crate) fn with_authority(connection_id: u64, authority: LinuxResourceAuthority) -> Self {
        Self::with_connection(
            connection_id,
            process_owner(),
            PathBuf::new(),
            authority,
            Arc::new(Mutex::new(Vec::new())),
        )
    }

    pub(crate) fn with_connection(
        connection_id: u64,
        peer_pid: u64,
        bulk_endpoint: PathBuf,
        authority: LinuxResourceAuthority,
        closed_resources: Arc<Mutex<Vec<ResourceDescriptor>>>,
    ) -> Self {
        Self {
            connection_id,
            peer_pid,
            bulk_endpoint: Arc::new(bulk_endpoint),
            resources: Arc::new(Mutex::new(BTreeMap::new())),
            authority,
            closed_resources,
            lifetime: Arc::new(()),
        }
    }

    /// Stable identifier for diagnostics and endpoint ownership tests.
    pub fn connection_id(&self) -> u64 {
        self.connection_id
    }

    pub(crate) fn peer_pid(&self) -> u64 {
        self.peer_pid
    }

    pub(crate) fn bulk_endpoint(&self) -> &Path {
        self.bulk_endpoint.as_ref()
    }

    /// Register a resource received from a downstream service.  The resource
    /// id remains opaque on the wire, but its lifetime is now scoped to this
    /// UDS connection just like a moved endpoint capability.
    pub fn adopt_resource(&self, resource: ResourceDescriptor) -> bool {
        if !self.can_adopt_resource(resource) {
            return false;
        }
        let Ok(mut resources) = self.resources.lock() else {
            return false;
        };
        if let Some(existing) = resources.get(&resource.resource_id)
            && (existing.binding != resource.binding
                || existing.scope != resource.scope
                || resource.rights & !existing.rights != 0
                || !resource_range_is_attenuated(*existing, resource))
        {
            return false;
        }
        if !self
            .authority
            .retain(resource, session_owner(self.connection_id))
        {
            return false;
        }
        resources.insert(resource.resource_id, resource);
        true
    }

    fn can_adopt_resource(&self, resource: ResourceDescriptor) -> bool {
        if resource.resource_id == 0
            || resource.rights & crate::sys::RIGHT_TRANSFER == 0
            || !self.authority.validate(resource)
        {
            return false;
        }
        let Ok(resources) = self.resources.lock() else {
            return false;
        };
        if let Some(existing) = resources.get(&resource.resource_id) {
            return existing.binding == resource.binding
                && existing.scope == resource.scope
                && resource.rights & !existing.rights == 0
                && resource_range_is_attenuated(*existing, resource);
        }
        drop(resources);
        self.authority.has_owner(resource.resource_id, self.peer_pid)
    }

    pub(crate) fn import_external_resource(&self, resource: ResourceDescriptor) -> bool {
        if resource.resource_id == 0 || resource.rights & crate::sys::RIGHT_TRANSFER == 0 {
            return false;
        }
        let Ok(resources) = self.resources.lock() else {
            return false;
        };
        if let Some(existing) = resources.get(&resource.resource_id)
            && (existing.binding != resource.binding
                || existing.scope != resource.scope
                || resource.rights & !existing.rights != 0)
        {
            return false;
        }
        drop(resources);
        if !self
            .authority
            .import(resource, session_owner(self.connection_id))
        {
            return false;
        }
        let Ok(mut resources) = self.resources.lock() else {
            return false;
        };
        resources.insert(resource.resource_id, resource);
        true
    }

    /// Mint a fresh host endpoint resource owned by this connection.
    pub fn mint_resource(&self, binding: u32, scope: u64, rights: u64) -> ResourceDescriptor {
        let resource = self.authority.mint_for(
            session_owner(self.connection_id),
            binding,
            scope,
            rights | crate::sys::RIGHT_TRANSFER,
        );
        if let Ok(mut resources) = self.resources.lock() {
            resources.insert(resource.resource_id, resource);
        }
        resource
    }

    /// Validate a resource supplied by a client.  Rights may be attenuated,
    /// but binding, scope, and resource identity must remain unchanged.
    pub fn validate_resource(&self, resource: ResourceDescriptor) -> bool {
        if resource.rights & crate::sys::RIGHT_TRANSFER == 0 {
            return false;
        }
        let Ok(resources) = self.resources.lock() else {
            return false;
        };
        let Some(original) = resources.get(&resource.resource_id) else {
            return false;
        };
        self.authority.validate(resource)
            && original.binding == resource.binding
            && original.scope == resource.scope
            && resource.rights != 0
            && resource.rights & !original.rights == 0
            && resource_range_is_attenuated(*original, resource)
    }

    /// Release all endpoint resources when the UDS peer disconnects.
    pub(crate) fn close(&self) {
        let released = if let Ok(mut resources) = self.resources.lock() {
            let ids: Vec<u64> = resources.keys().copied().collect();
            resources.clear();
            ids
        } else {
            Vec::new()
        };
        let closed = self
            .authority
            .release_owner_many(session_owner(self.connection_id), &released);
        if let Ok(mut pending) = self.closed_resources.lock() {
            pending.extend(closed);
        }
    }

    pub(crate) fn prepare_response_resources(&self, resources: &[ResourceDescriptor]) -> bool {
        if self.peer_pid == 0 {
            return false;
        }
        let mut retained = Vec::new();
        for resource in resources {
            if !self
                .authority
                .retain_owner(resource.resource_id, self.peer_pid)
            {
                let _ = self
                    .authority
                    .release_owner_many(self.peer_pid, &retained);
                return false;
            }
            retained.push(resource.resource_id);
        }
        true
    }

    pub(crate) fn finalize_response_resources(&self, resources: &[ResourceDescriptor], delivered: bool) {
        let ids: Vec<u64> = resources.iter().map(|resource| resource.resource_id).collect();
        let mut closed = if delivered {
            Vec::new()
        } else {
            self.authority.release_owner_many(self.peer_pid, &ids)
        };
        closed.extend(self.authority.release_owner_many(process_owner(), &ids));
        if let Ok(mut pending) = self.closed_resources.lock() {
            pending.extend(closed);
        }
    }
}

impl Drop for LinuxServiceSession {
    fn drop(&mut self) {
        // The legacy session API clones this value while dispatching a
        // connection.  Only the last clone owns the connection teardown.
        if Arc::strong_count(&self.lifetime) == 1 {
            self.close();
        }
    }
}

/// Initialize the Linux process logger.  The concrete `tidy_log` backend is
/// private to servicekit's platform adapter; daemons use this interface and
/// emit records through the standard `log` facade afterwards.
pub fn init_log(tag: &'static str, path: Option<&Path>) -> io::Result<()> {
    crate::tidy_log::linux::init(tag, path)
}

/// An endpoint is just a local socket locator.  Capability-like ids carried in
/// the frame are opaque contract values and do not claim kernel capability
/// strength on Linux.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct LinuxEndpoint {
    path: Arc<PathBuf>,
    target: Option<ResourceDescriptor>,
}

/// Platform-neutral public name for a service endpoint.
pub type Endpoint = LinuxEndpoint;

pub const DEFAULT_SERVICE_DIRECTORY: &str = "/run/naos/services";

fn release_path(path: &Path) -> PathBuf {
    let mut value = path.as_os_str().to_os_string();
    value.push(".release");
    PathBuf::from(value)
}

/// Notify a provider that the last local owner of a dynamic resource was
/// dropped.  This is deliberately a private control path; resource ids are
/// still checked against the provider's authoritative table before they are
/// reclaimed.
pub(crate) fn release_resource(path: &Path, resource_id: u64) {
    let Ok(mut stream) = std::os::unix::net::UnixStream::connect(release_path(path)) else {
        return;
    };
    let _ = stream.set_write_timeout(Some(std::time::Duration::from_millis(250)));
    let _ = stream.write_all(&resource_id.to_le_bytes());
}

pub(crate) fn bind_release(
    path: &Path,
    authority: LinuxResourceAuthority,
    closed_resources: Arc<Mutex<Vec<ResourceDescriptor>>>,
) -> io::Result<ReleaseServer> {
    let release_socket_path = release_path(path);
    let _ = std::fs::remove_file(&release_socket_path);
    let listener = std::os::unix::net::UnixListener::bind(&release_socket_path)?;
    listener.set_nonblocking(true)?;
    let stop = Arc::new(AtomicBool::new(false));
    let thread_stop = stop.clone();
    let thread = std::thread::Builder::new()
        .name("naos-resource-release".to_string())
        .spawn(move || {
            while !thread_stop.load(Ordering::Acquire) {
                let dead = authority.reap_dead_process_owners();
                if let Ok(mut closed) = closed_resources.lock() {
                    closed.extend(dead);
                }
                match listener.accept() {
                    Ok((mut stream, _)) => {
                        let _ = stream.set_read_timeout(Some(std::time::Duration::from_secs(1)));
                        let mut id = [0_u8; 8];
                        if stream.read_exact(&mut id).is_err() {
                            continue;
                        }
                        let resource_id = u64::from_le_bytes(id);
                        let Some(owner) = peer_pid(&stream) else {
                            continue;
                        };
                        if let Some(descriptor) = authority.release_owner(owner, resource_id) {
                            if let Ok(mut closed) = closed_resources.lock() {
                                closed.push(descriptor);
                            }
                        }
                    }
                    Err(error) if error.kind() == io::ErrorKind::WouldBlock => {
                        std::thread::sleep(std::time::Duration::from_millis(10));
                    }
                    Err(_) => break,
                }
            }
        })
        .map_err(|error| io::Error::new(io::ErrorKind::Other, error))
        ?;
    Ok(ReleaseServer {
        path: release_path(path),
        stop,
        thread: Some(thread),
    })
}

pub(crate) struct ReleaseServer {
    path: PathBuf,
    stop: Arc<AtomicBool>,
    thread: Option<std::thread::JoinHandle<()>>,
}

impl Drop for ReleaseServer {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::Release);
        if let Some(thread) = self.thread.take() {
            let _ = thread.join();
        }
        let _ = std::fs::remove_file(&self.path);
    }
}

fn peer_pid(stream: &std::os::unix::net::UnixStream) -> Option<u64> {
    let mut credentials = libc::ucred {
        pid: 0,
        uid: 0,
        gid: 0,
    };
    let mut length = core::mem::size_of::<libc::ucred>() as libc::socklen_t;
    let status = unsafe {
        libc::getsockopt(
            stream.as_raw_fd(),
            libc::SOL_SOCKET,
            libc::SO_PEERCRED,
            (&mut credentials as *mut libc::ucred).cast(),
            &mut length,
        )
    };
    (status == 0 && length as usize >= core::mem::size_of::<libc::ucred>() && credentials.pid > 0)
        .then_some(credentials.pid as u64)
}

fn process_is_alive(pid: u64) -> bool {
    let Ok(pid) = i32::try_from(pid) else {
        return false;
    };
    let result = unsafe { libc::kill(pid, 0) };
    result == 0 || (result == -1 && std::io::Error::last_os_error().raw_os_error() == Some(libc::EPERM))
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum LinuxServiceDirectoryError {
    InvalidUri,
    Io(std::io::ErrorKind),
    Capacity,
    TimedOut,
}

/// Platform-neutral public name for service-directory errors.
pub type ServiceDirectoryError = LinuxServiceDirectoryError;

/// Linux implementation of the NaOS service-directory locator contract.
///
/// The default root is fixed so independently started daemons can discover
/// one another. Tests may use a different root to isolate parallel runs; the
/// URI and endpoint mapping remain unchanged.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct LinuxServiceDirectory {
    root: Arc<PathBuf>,
}

/// Platform-neutral public name for the service directory.
pub type ServiceDirectory = LinuxServiceDirectory;

impl LinuxServiceDirectory {
    pub fn new(root: impl AsRef<Path>) -> Self {
        Self {
            root: Arc::new(root.as_ref().to_path_buf()),
        }
    }

    pub fn prepare(&self) -> std::io::Result<()> {
        std::fs::create_dir_all(self.root.as_ref())
    }

    pub fn endpoint_for(&self, uri: &str) -> Result<LinuxEndpoint, LinuxServiceDirectoryError> {
        let segments = service_segments(uri, false)?;
        let mut path = self.root.as_ref().clone();
        for segment in segments {
            path.push(segment);
        }
        Ok(LinuxEndpoint::new(path))
    }

    /// Return all bound service endpoints whose canonical URI starts with
    /// `prefix`, in lexical URI order.  The caller supplies storage so a
    /// malicious or misconfigured service tree cannot cause an unbounded
    /// allocation during discovery.
    pub fn list_prefix(
        &self,
        prefix: &str,
        endpoints: &mut [LinuxEndpoint],
    ) -> Result<usize, LinuxServiceDirectoryError> {
        let segments = service_segments(prefix, true)?;
        let canonical_prefix = if segments.is_empty() {
            SERVICE_PREFIX.to_owned()
        } else {
            std::format!("{SERVICE_PREFIX}{}", segments.join("/"))
        };
        if !self.root.exists() {
            return Ok(0);
        }
        let mut matches = Vec::with_capacity(endpoints.len());
        collect_services(
            self.root.as_ref(),
            SERVICE_PREFIX,
            &canonical_prefix,
            endpoints.len(),
            &mut matches,
        )?;
        let count = matches.len();
        for (index, (_, path)) in matches.into_iter().enumerate() {
            endpoints[index] = LinuxEndpoint::new(path);
        }
        Ok(count)
    }

    /// Wait until at least one live endpoint matches the segment-bounded
    /// prefix. The directory is filesystem-backed on Linux, so publication is
    /// observed by polling the same canonical listing used by `list_prefix`.
    pub async fn wait_for_prefix(
        &self,
        prefix: &str,
        endpoints: &mut [LinuxEndpoint],
        timeout: Duration,
    ) -> Result<usize, LinuxServiceDirectoryError> {
        let result = tokio::time::timeout(timeout, async {
            loop {
                let count = self.list_prefix(prefix, endpoints)?;
                if count != 0 {
                    return Ok(count);
                }
                tokio::time::sleep(Duration::from_millis(1)).await;
            }
        })
        .await;
        result.unwrap_or(Err(LinuxServiceDirectoryError::TimedOut))
    }
}

const SERVICE_PREFIX: &str = crate::uri::SERVICE_PREFIX;
const SERVICE_ROOT: &str = crate::uri::SERVICE_ROOT;

fn service_segments(
    uri: &str,
    allow_prefix: bool,
) -> Result<Vec<&str>, LinuxServiceDirectoryError> {
    if uri.len() > 255 {
        return Err(LinuxServiceDirectoryError::InvalidUri);
    }
    let suffix = if allow_prefix && uri == SERVICE_ROOT {
        ""
    } else {
        uri.strip_prefix(SERVICE_PREFIX)
            .ok_or(LinuxServiceDirectoryError::InvalidUri)?
    };
    let trailing_slash = suffix.ends_with('/');
    if !allow_prefix && trailing_slash {
        return Err(LinuxServiceDirectoryError::InvalidUri);
    }
    let suffix = suffix.strip_suffix('/').unwrap_or(suffix);
    let segments: Vec<&str> = if suffix.is_empty() {
        Vec::new()
    } else {
        suffix.split('/').collect()
    };
    if !allow_prefix && segments.is_empty() {
        return Err(LinuxServiceDirectoryError::InvalidUri);
    }
    if segments.iter().any(|segment| {
        segment.is_empty()
            || *segment == "."
            || *segment == ".."
            || segment.contains('\0')
            || segment.bytes().any(|value| {
                !value.is_ascii_alphanumeric()
                    && value != b'-'
                    && value != b'_'
                    && value != b'.'
                    && value != b'~'
            })
    }) {
        return Err(LinuxServiceDirectoryError::InvalidUri);
    }
    Ok(segments)
}

fn collect_services(
    directory: &Path,
    uri_prefix: &str,
    filter_prefix: &str,
    capacity: usize,
    matches: &mut Vec<(String, PathBuf)>,
) -> Result<(), LinuxServiceDirectoryError> {
    let entries = std::fs::read_dir(directory)
        .map_err(|error| LinuxServiceDirectoryError::Io(error.kind()))?;
    for entry in entries {
        let entry = entry.map_err(|error| LinuxServiceDirectoryError::Io(error.kind()))?;
        let path = entry.path();
        let name = entry
            .file_name()
            .to_str()
            .ok_or(LinuxServiceDirectoryError::InvalidUri)?
            .to_owned();
        if name == "." || name == ".." || name.contains('\0') {
            continue;
        }
        let file_type = entry
            .file_type()
            .map_err(|error| LinuxServiceDirectoryError::Io(error.kind()))?;
        let uri = std::format!("{uri_prefix}{name}");
        if file_type.is_dir() {
            let child_uri_prefix = std::format!("{uri}/");
            collect_services(&path, &child_uri_prefix, filter_prefix, capacity, matches)?;
        } else if file_type.is_socket()
            && !name.ends_with(".bulk")
            && !name.ends_with(".release")
            && is_live_service(&path)
            && (filter_prefix == SERVICE_PREFIX
                || uri == filter_prefix
                || uri.starts_with(&std::format!("{filter_prefix}/")))
        {
            if matches.len() >= capacity {
                return Err(LinuxServiceDirectoryError::Capacity);
            }
            let position = matches
                .binary_search_by(|(existing, _)| existing.cmp(&uri))
                .unwrap_or_else(|position| position);
            matches.insert(position, (uri, path));
        }
    }
    Ok(())
}

fn is_live_service(path: &Path) -> bool {
    // A crashed process leaves the pathname behind. A short local connect
    // probe distinguishes that stale entry from a listener; the peer accepts
    // and immediately observes EOF, so this does not require an RPC frame or
    // a service-specific protocol.
    std::os::unix::net::UnixStream::connect(path).is_ok()
}

impl Default for LinuxServiceDirectory {
    fn default() -> Self {
        let root = std::env::var_os("NAOS_SERVICE_DIR")
            .map(PathBuf::from)
            .unwrap_or_else(|| PathBuf::from(DEFAULT_SERVICE_DIRECTORY));
        Self::new(root)
    }
}

impl ServiceLocator for LinuxServiceDirectory {
    type Endpoint = LinuxEndpoint;
    type Error = LinuxServiceDirectoryError;
    type Resolve<'a> = Ready<Result<Self::Endpoint, Self::Error>>;
    type List<'a> = Ready<Result<usize, Self::Error>>;

    fn resolve<'a>(&'a self, uri: &'a str) -> Self::Resolve<'a> {
        core::future::ready(self.endpoint_for(uri))
    }

    fn list<'a>(&'a self, prefix: &'a str, endpoints: &'a mut [Self::Endpoint]) -> Self::List<'a> {
        core::future::ready(self.list_prefix(prefix, endpoints))
    }
}

impl LinuxEndpoint {
    pub fn new(path: impl AsRef<Path>) -> Self {
        Self {
            path: Arc::new(path.as_ref().to_path_buf()),
            target: None,
        }
    }

    /// Return an endpoint that invokes a specific opaque authority. The
    /// target is encoded separately from method resource arguments.
    pub fn with_target(&self, target: ResourceDescriptor) -> Self {
        Self {
            path: self.path.clone(),
            target: Some(target),
        }
    }

    pub fn target(&self) -> Option<ResourceDescriptor> {
        self.target
    }

    pub fn without_target(&self) -> Self {
        Self {
            path: self.path.clone(),
            target: None,
        }
    }

    pub fn path(&self) -> &Path {
        self.path.as_ref()
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum TransportError {
    Io,
    PeerClosed,
    FrameTooLarge,
    Protocol,
    Bulk,
}

#[derive(Clone, Debug)]
pub struct LinuxUdsTransport {
    next_request_id: Arc<std::sync::atomic::AtomicU64>,
}

/// Platform-neutral public name for the asynchronous local transport.
pub type UdsTransport = LinuxUdsTransport;

/// A transport adapter for synchronous consumers.  The generated IDL client
/// remains the same; its future performs one blocking UDS transaction when
/// first polled.  This is intended for a dedicated blocking worker thread,
/// never for a Tokio reactor thread.
#[derive(Clone, Debug, Default)]
pub struct LinuxBlockingTransport {
    inner: LinuxUdsTransport,
}

/// Platform-neutral public name for the blocking transport.
pub type BlockingTransport = LinuxBlockingTransport;

impl LinuxBlockingTransport {
    pub fn new() -> Self {
        Self {
            inner: LinuxUdsTransport::new(),
        }
    }

    pub fn endpoint(path: impl AsRef<Path>) -> LinuxEndpoint {
        LinuxUdsTransport::endpoint(path)
    }
}

pub struct BlockingInvoke<'a> {
    transport: &'a LinuxUdsTransport,
    endpoint: &'a LinuxEndpoint,
    request: Option<RpcRequest<'a>>,
}

impl core::future::Future for BlockingInvoke<'_> {
    type Output = Result<RpcResponseOwned, TransportError>;

    fn poll(
        mut self: Pin<&mut Self>,
        _context: &mut core::task::Context<'_>,
    ) -> core::task::Poll<Self::Output> {
        let request = self.request.take();
        let result = request.map_or(Err(TransportError::PeerClosed), |request| {
            self.transport.invoke_blocking(self.endpoint, request)
        });
        core::task::Poll::Ready(result)
    }
}

impl RpcTransport for LinuxBlockingTransport {
    type Endpoint = LinuxEndpoint;
    type Error = TransportError;
    type Response = RpcResponseOwned;
    type Invoke<'a> = BlockingInvoke<'a>;

    fn invoke<'a>(
        &'a self,
        endpoint: &'a Self::Endpoint,
        request: RpcRequest<'a>,
    ) -> Self::Invoke<'a> {
        BlockingInvoke {
            transport: &self.inner,
            endpoint,
            request: Some(request),
        }
    }
}

impl Default for LinuxUdsTransport {
    fn default() -> Self {
        Self::new()
    }
}

impl LinuxUdsTransport {
    pub fn new() -> Self {
        Self {
            next_request_id: Arc::new(std::sync::atomic::AtomicU64::new(1)),
        }
    }

    pub fn endpoint(path: impl AsRef<Path>) -> LinuxEndpoint {
        LinuxEndpoint::new(path)
    }

    fn assign_request_id(&self, requested: u64) -> u64 {
        if requested != 0 {
            requested
        } else {
            self.next_request_id
                .fetch_add(1, std::sync::atomic::Ordering::Relaxed)
        }
    }

    /// Tokio-facing invocation.  This inherent method exposes a concrete
    /// `Send` future for multi-connection servers; the trait implementation
    /// below retains the platform-neutral `RpcTransport` surface.
    pub async fn invoke_async<'a>(
        &'a self,
        endpoint: &'a LinuxEndpoint,
        request: RpcRequest<'a>,
    ) -> Result<RpcResponseOwned, TransportError> {
        let request = owned_request(
            self.assign_request_id(request.request_id),
            request,
            endpoint.target(),
        )?;
        let mut stream = UnixStream::connect(endpoint.path())
            .await
            .map_err(|_| TransportError::Io)?;
        write_frame_async(&mut stream, &request).await?;
        let response = read_response_async(&mut stream).await?;
        if response.request_id != request.request_id {
            return Err(TransportError::Protocol);
        }
        Ok(response)
    }

    /// Synchronous counterpart used only inside `spawn_blocking` for the
    /// existing synchronous FAT library.  It shares exactly the same frame
    /// encoder/decoder as the Tokio path.
    pub fn invoke_blocking(
        &self,
        endpoint: &LinuxEndpoint,
        request: RpcRequest<'_>,
    ) -> Result<RpcResponseOwned, TransportError> {
        let request = owned_request(
            self.assign_request_id(request.request_id),
            request,
            endpoint.target(),
        )?;
        let mut stream = std::os::unix::net::UnixStream::connect(endpoint.path())
            .map_err(|_| TransportError::Io)?;
        write_frame_blocking(&mut stream, &request)?;
        let response = read_response_blocking(&mut stream)?;
        if response.request_id != request.request_id {
            return Err(TransportError::Protocol);
        }
        Ok(response)
    }
}

impl RpcTransport for LinuxUdsTransport {
    type Endpoint = LinuxEndpoint;
    type Error = TransportError;
    type Response = RpcResponseOwned;
    type Invoke<'a> = Pin<
        Box<
            dyn core::future::Future<Output = Result<RpcResponseOwned, TransportError>> + Send + 'a,
        >,
    >;

    fn invoke<'a>(
        &'a self,
        endpoint: &'a Self::Endpoint,
        request: RpcRequest<'a>,
    ) -> Self::Invoke<'a> {
        Box::pin(self.invoke_async(endpoint, request))
    }
}

fn owned_request(
    request_id: u64,
    request: RpcRequest<'_>,
    endpoint_target: Option<ResourceDescriptor>,
) -> Result<RpcRequestOwned, TransportError> {
    if request.payload.len() > MAX_FRAME_BYTES
        || request.resources.len() > MAX_RESOURCES
        || request.bulk.len() > MAX_BULK_BUFFERS
        || request.bulk.iter().any(|bulk| !bulk.is_valid())
    {
        return Err(TransportError::FrameTooLarge);
    }
    Ok(RpcRequestOwned {
        protocol_uuid: request.protocol_uuid,
        revision: request.revision,
        method_id: request.method_id,
        request_id,
        target: endpoint_target.or(request.target),
        payload: request.payload.to_vec(),
        resources: request.resources.to_vec(),
        bulk: request.bulk.to_vec(),
    })
}

fn put_u16(out: &mut Vec<u8>, value: u16) {
    out.extend_from_slice(&value.to_le_bytes());
}

fn put_u32(out: &mut Vec<u8>, value: u32) {
    out.extend_from_slice(&value.to_le_bytes());
}

fn put_u64(out: &mut Vec<u8>, value: u64) {
    out.extend_from_slice(&value.to_le_bytes());
}

fn put_i64(out: &mut Vec<u8>, value: i64) {
    out.extend_from_slice(&value.to_le_bytes());
}

fn get_u16(bytes: &[u8], cursor: &mut usize) -> Result<u16, TransportError> {
    let end = cursor.checked_add(2).ok_or(TransportError::Protocol)?;
    let value = u16::from_le_bytes(
        bytes
            .get(*cursor..end)
            .ok_or(TransportError::Protocol)?
            .try_into()
            .unwrap(),
    );
    *cursor = end;
    Ok(value)
}

fn get_u32(bytes: &[u8], cursor: &mut usize) -> Result<u32, TransportError> {
    let end = cursor.checked_add(4).ok_or(TransportError::Protocol)?;
    let value = u32::from_le_bytes(
        bytes
            .get(*cursor..end)
            .ok_or(TransportError::Protocol)?
            .try_into()
            .unwrap(),
    );
    *cursor = end;
    Ok(value)
}

fn get_u64(bytes: &[u8], cursor: &mut usize) -> Result<u64, TransportError> {
    let end = cursor.checked_add(8).ok_or(TransportError::Protocol)?;
    let value = u64::from_le_bytes(
        bytes
            .get(*cursor..end)
            .ok_or(TransportError::Protocol)?
            .try_into()
            .unwrap(),
    );
    *cursor = end;
    Ok(value)
}

fn get_i64(bytes: &[u8], cursor: &mut usize) -> Result<i64, TransportError> {
    Ok(get_u64(bytes, cursor)? as i64)
}

fn encode_frame(
    kind: u16,
    response: Option<&RpcResponseOwned>,
    request: Option<&RpcRequestOwned>,
) -> Result<Vec<u8>, TransportError> {
    let (uuid, revision, method_id, request_id, target, payload, resources, bulk, outcome) =
        match (response, request) {
            (Some(response), None) => (
                response.protocol_uuid,
                response.revision,
                response.method_id,
                response.request_id,
                response.target,
                response.payload.as_slice(),
                response.resources.as_slice(),
                response.bulk.as_slice(),
                response.outcome,
            ),
            (None, Some(request)) => (
                request.protocol_uuid,
                request.revision,
                request.method_id,
                request.request_id,
                request.target,
                request.payload.as_slice(),
                request.resources.as_slice(),
                request.bulk.as_slice(),
                RpcOutcome::SUCCESS,
            ),
            _ => return Err(TransportError::Protocol),
        };
    if resources.len() > MAX_RESOURCES || bulk.len() > MAX_BULK_BUFFERS {
        return Err(TransportError::FrameTooLarge);
    }
    let descriptors = resources
        .len()
        .checked_mul(RESOURCE_BYTES)
        .and_then(|value| value.checked_add(bulk.len().checked_mul(BULK_BYTES)?))
        .ok_or(TransportError::FrameTooLarge)?;
    let body_bytes = HEADER_BYTES
        .checked_add(if target.is_some() { RESOURCE_BYTES } else { 0 })
        .and_then(|value| value.checked_add(payload.len()))
        .and_then(|value| value.checked_add(descriptors))
        .ok_or(TransportError::FrameTooLarge)?;
    if body_bytes > MAX_FRAME_BYTES {
        return Err(TransportError::FrameTooLarge);
    }
    let mut body = Vec::with_capacity(body_bytes + 4);
    body.extend_from_slice(MAGIC);
    put_u16(&mut body, VERSION);
    put_u16(&mut body, kind);
    body.extend_from_slice(&uuid);
    put_u64(&mut body, revision);
    put_u64(&mut body, method_id);
    put_u64(&mut body, request_id);
    put_u32(&mut body, payload.len() as u32);
    put_u16(&mut body, resources.len() as u16);
    put_u16(&mut body, bulk.len() as u16);
    put_u32(
        &mut body,
        if target.is_some() {
            FRAME_FLAG_TARGET
        } else {
            0
        },
    );
    put_i64(&mut body, outcome.protocol_error);
    put_u32(&mut body, outcome.execution);
    put_u32(&mut body, outcome.reason);
    if let Some(target) = target {
        put_resource(&mut body, target);
    }
    body.extend_from_slice(payload);
    for resource in resources {
        put_resource(&mut body, *resource);
    }
    for descriptor in bulk {
        put_u64(&mut body, descriptor.region_id);
        put_u64(&mut body, descriptor.offset);
        put_u64(&mut body, descriptor.length);
        put_u64(&mut body, descriptor.generation);
        body.push(descriptor.direction as u8);
        body.extend_from_slice(&[0; 7]);
        put_u64(&mut body, descriptor.rights);
    }
    let mut frame = Vec::with_capacity(body.len() + 4);
    put_u32(&mut frame, body.len() as u32);
    frame.extend_from_slice(&body);
    Ok(frame)
}

fn put_resource(out: &mut Vec<u8>, resource: ResourceDescriptor) {
    put_u64(out, resource.resource_id);
    put_u32(out, resource.binding);
    put_u32(out, 0);
    put_u64(out, resource.scope);
    put_u64(out, resource.rights);
    put_u64(out, resource.view_offset);
    put_u64(out, resource.view_length);
}

fn get_resource(bytes: &[u8], cursor: &mut usize) -> Result<ResourceDescriptor, TransportError> {
    let resource_id = get_u64(bytes, cursor)?;
    let binding = get_u32(bytes, cursor)?;
    let _reserved = get_u32(bytes, cursor)?;
    let scope = get_u64(bytes, cursor)?;
    let rights = get_u64(bytes, cursor)?;
    let view_offset = get_u64(bytes, cursor)?;
    let view_length = get_u64(bytes, cursor)?;
    if resource_id == 0 {
        return Err(TransportError::Protocol);
    }
    Ok(ResourceDescriptor {
        resource_id,
        binding,
        scope,
        rights,
        view_offset,
        view_length,
        ..ResourceDescriptor::default()
    })
}

fn decode_frame(body: &[u8], expected_kind: u16) -> Result<RpcResponseOwned, TransportError> {
    if body.len() < HEADER_BYTES || &body[..8] != MAGIC {
        return Err(TransportError::Protocol);
    }
    let mut cursor = 8;
    if get_u16(body, &mut cursor)? != VERSION || get_u16(body, &mut cursor)? != expected_kind {
        return Err(TransportError::Protocol);
    }
    let uuid: [u8; 16] = body
        .get(cursor..cursor + 16)
        .ok_or(TransportError::Protocol)?
        .try_into()
        .unwrap();
    cursor += 16;
    let revision = get_u64(body, &mut cursor)?;
    let method_id = get_u64(body, &mut cursor)?;
    let request_id = get_u64(body, &mut cursor)?;
    let payload_len = get_u32(body, &mut cursor)? as usize;
    let resource_count = get_u16(body, &mut cursor)? as usize;
    let bulk_count = get_u16(body, &mut cursor)? as usize;
    let flags = get_u32(body, &mut cursor)?;
    let protocol_error = get_i64(body, &mut cursor)?;
    let execution = get_u32(body, &mut cursor)?;
    let reason = get_u32(body, &mut cursor)?;
    if flags & !FRAME_FLAG_TARGET != 0 {
        return Err(TransportError::Protocol);
    }
    let target = if flags & FRAME_FLAG_TARGET != 0 {
        Some(get_resource(body, &mut cursor)?)
    } else {
        None
    };
    if resource_count > MAX_RESOURCES || bulk_count > MAX_BULK_BUFFERS {
        return Err(TransportError::Protocol);
    }
    let payload_end = cursor
        .checked_add(payload_len)
        .ok_or(TransportError::Protocol)?;
    if payload_end > body.len() {
        return Err(TransportError::Protocol);
    }
    let payload = body[cursor..payload_end].to_vec();
    cursor = payload_end;
    let descriptors = resource_count
        .checked_mul(RESOURCE_BYTES)
        .and_then(|value| value.checked_add(bulk_count.checked_mul(BULK_BYTES)?))
        .ok_or(TransportError::Protocol)?;
    if cursor.checked_add(descriptors) != Some(body.len()) {
        return Err(TransportError::Protocol);
    }
    let mut resources = Vec::with_capacity(resource_count);
    for _ in 0..resource_count {
        resources.push(get_resource(body, &mut cursor)?);
    }
    let mut bulk = Vec::with_capacity(bulk_count);
    for _ in 0..bulk_count {
        let region_id = get_u64(body, &mut cursor)?;
        let offset = get_u64(body, &mut cursor)?;
        let length = get_u64(body, &mut cursor)?;
        let generation = get_u64(body, &mut cursor)?;
        let direction = match body.get(cursor).copied() {
            Some(1) => BulkDirection::In,
            Some(2) => BulkDirection::Out,
            Some(3) => BulkDirection::InOut,
            _ => return Err(TransportError::Protocol),
        };
        cursor += 8;
        let rights = get_u64(body, &mut cursor)?;
        let descriptor = BulkBuffer::new(region_id, offset, length, generation, direction, rights);
        if !descriptor.is_valid() {
            return Err(TransportError::Protocol);
        }
        bulk.push(descriptor);
    }
    Ok(RpcResponseOwned {
        protocol_uuid: uuid,
        revision,
        method_id,
        request_id,
        target,
        outcome: RpcOutcome {
            execution,
            reason,
            protocol_error,
        },
        payload,
        resources,
        bulk,
    })
}

pub(crate) async fn read_frame_async<S: AsyncRead + Unpin>(
    stream: &mut S,
) -> Result<Vec<u8>, TransportError> {
    let length = stream
        .read_u32_le()
        .await
        .map_err(|_| TransportError::PeerClosed)? as usize;
    if length > MAX_FRAME_BYTES {
        return Err(TransportError::FrameTooLarge);
    }
    let mut body = vec![0; length];
    stream
        .read_exact(&mut body)
        .await
        .map_err(|_| TransportError::PeerClosed)?;
    Ok(body)
}

async fn write_frame_async<S: AsyncWrite + Unpin>(
    stream: &mut S,
    request: &RpcRequestOwned,
) -> Result<(), TransportError> {
    let frame = encode_frame(REQUEST, None, Some(request))?;
    stream
        .write_all(&frame)
        .await
        .map_err(|_| TransportError::Io)
}

#[cfg(test)]
async fn write_response_async<S: AsyncWrite + Unpin>(
    stream: &mut S,
    response: &RpcResponseOwned,
) -> Result<(), TransportError> {
    let frame = encode_frame(RESPONSE, Some(response), None)?;
    stream
        .write_all(&frame)
        .await
        .map_err(|_| TransportError::Io)
}

async fn read_response_async<S: AsyncRead + Unpin>(
    stream: &mut S,
) -> Result<RpcResponseOwned, TransportError> {
    decode_frame(&read_frame_async(stream).await?, RESPONSE)
}

fn write_frame_blocking(
    stream: &mut std::os::unix::net::UnixStream,
    request: &RpcRequestOwned,
) -> Result<(), TransportError> {
    stream
        .write_all(&encode_frame(REQUEST, None, Some(request))?)
        .map_err(|_| TransportError::Io)
}

fn read_response_blocking(
    stream: &mut std::os::unix::net::UnixStream,
) -> Result<RpcResponseOwned, TransportError> {
    let mut prefix = [0; 4];
    stream
        .read_exact(&mut prefix)
        .map_err(|_| TransportError::PeerClosed)?;
    let length = u32::from_le_bytes(prefix) as usize;
    if length > MAX_FRAME_BYTES {
        return Err(TransportError::FrameTooLarge);
    }
    let mut body = vec![0; length];
    stream
        .read_exact(&mut body)
        .map_err(|_| TransportError::PeerClosed)?;
    decode_frame(&body, RESPONSE)
}

/// Bind a local-only UDS and dispatch each connection on Tokio.  A connection
/// is serial by design, preserving the NaoIDL default endpoint execution
/// model; separate connections may run concurrently.
#[cfg(test)]
pub(crate) async fn serve<F, Fut>(endpoint: LinuxEndpoint, handler: F) -> io::Result<()>
where
    F: Fn(RpcRequestOwned) -> Fut + Clone + Send + Sync + 'static,
    Fut: core::future::Future<Output = RpcResponseOwned> + Send + 'static,
{
    serve_with_session(endpoint, move |request, _session| handler(request)).await
}

/// Bind a local service and attach an endpoint session to every UDS
/// connection. The session is the host-side equivalent of the NaOS endpoint
/// object table: resources returned by a service can be adopted into the
/// connection, and later requests can validate that they belong to that
/// connection with no global integer-handle assumption.
#[cfg(test)]
pub(crate) async fn serve_with_session<F, Fut>(endpoint: LinuxEndpoint, handler: F) -> io::Result<()>
where
    F: Fn(RpcRequestOwned, LinuxServiceSession) -> Fut + Clone + Send + Sync + 'static,
    Fut: core::future::Future<Output = RpcResponseOwned> + Send + 'static,
{
    let path = endpoint.path().to_path_buf();
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent)?;
    }
    let _bulk_server = bulk::bind(&path)?;
    let _ = std::fs::remove_file(&path);
    let listener = UnixListener::bind(&path)?;
    let connections = Arc::new(tokio::sync::Semaphore::new(MAX_CONNECTIONS));
    let next_session = Arc::new(AtomicU64::new(1));
    let authority = LinuxResourceAuthority::new();
    loop {
        let (mut stream, _) = listener.accept().await?;
        let Ok(permit) = connections.clone().try_acquire_owned() else {
            // A full connection budget is deliberate backpressure: close the
            // new peer instead of creating an unbounded Tokio task set.
            drop(stream);
            continue;
        };
        let handler = handler.clone();
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
            Arc::new(Mutex::new(Vec::new())),
        );
        let connection_path = path.clone();
        tokio::spawn(async move {
            let _permit = permit;
            let mut first_frame = true;
            loop {
                let body = match if first_frame {
                    first_frame = false;
                    match tokio::time::timeout(Duration::from_secs(2), read_frame_async(&mut stream)).await {
                        Ok(result) => result,
                        Err(_) => Err(TransportError::PeerClosed),
                    }
                } else {
                    read_frame_async(&mut stream).await
                } {
                    Ok(body) => body,
                    Err(_) => break,
                };
                let request = match decode_request(&body) {
                    Ok(request) => request,
                    Err(_) => break,
                };
                // A descriptor is admitted only if this service's authority
                // issued it (or an existing session already owns an
                // attenuated form).  The UDS frame itself is never allowed to
                // create authority by merely naming a resource_id.
                let target_admissible = request
                    .target
                    .map_or(true, |target| session.can_adopt_resource(target));
                let resources_admissible = request
                    .resources
                    .iter()
                    .copied()
                    .all(|resource| session.can_adopt_resource(resource));
                if !target_admissible || !resources_admissible {
                    let response = RpcResponseOwned {
                        protocol_uuid: request.protocol_uuid,
                        revision: request.revision,
                        method_id: request.method_id,
                        request_id: request.request_id,
                        target: None,
                        outcome: RpcOutcome {
                            execution: 0,
                            reason: 0,
                            protocol_error: -13,
                        },
                        payload: Vec::new(),
                        resources: Vec::new(),
                        bulk: Vec::new(),
                    };
                    let write_result = write_response_async(&mut stream, &response).await;
                    bulk::release_for(&connection_path, session.peer_pid(), &request.bulk);
                    if write_result.is_err() {
                        break;
                    }
                    continue;
                }
                if let Some(target) = request.target {
                    let _ = session.adopt_resource(target);
                }
                for resource in request.resources.iter().copied() {
                    let _ = session.adopt_resource(resource);
                }
                let bulk = request.bulk.clone();
                let response = handler(request, session.clone()).await;
                let write_result = write_response_async(&mut stream, &response).await;
                bulk::release_for(&connection_path, session.peer_pid(), &bulk);
                if write_result.is_err() {
                    break;
                }
            }
            session.close();
        });
    }
}

pub(crate) fn decode_request(body: &[u8]) -> Result<RpcRequestOwned, TransportError> {
    let response = decode_frame(body, REQUEST)?;
    Ok(RpcRequestOwned {
        protocol_uuid: response.protocol_uuid,
        revision: response.revision,
        method_id: response.method_id,
        request_id: response.request_id,
        target: response.target,
        payload: response.payload,
        resources: response.resources,
        bulk: response.bulk,
    })
}

pub(crate) fn encode_response(response: &RpcResponseOwned) -> Result<Vec<u8>, TransportError> {
    encode_frame(RESPONSE, Some(response), None)
}

#[cfg(test)]
mod tests {
    use super::*;
    use naos_sys as sys;
    use std::time::Duration;

    fn request() -> RpcRequestOwned {
        RpcRequestOwned {
            protocol_uuid: [7; 16],
            revision: 4,
            method_id: 9,
            request_id: 11,
            target: Some(ResourceDescriptor {
                resource_id: 88,
                binding: 3,
                scope: 4,
                rights: 5,
                view_offset: 3,
                view_length: 17,
                ..ResourceDescriptor::default()
            }),
            payload: vec![1, 2, 3],
            resources: vec![ResourceDescriptor {
                resource_id: 99,
                binding: 1,
                scope: 2,
                rights: 3,
                view_offset: 11,
                view_length: 13,
                ..ResourceDescriptor::default()
            }],
            bulk: vec![BulkBuffer::new(5, 0, 4096, 1, BulkDirection::InOut, 3)],
        }
    }

    #[test]
    fn frame_round_trip_preserves_contract_metadata() {
        let request = request();
        let frame = encode_frame(REQUEST, None, Some(&request)).unwrap();
        let decoded = decode_request(&frame[4..]).unwrap();
        assert_eq!(decoded, request);
    }

    #[test]
    fn endpoint_resources_are_scoped_and_can_only_be_attenuated() {
        let first = LinuxServiceSession::with_authority(1, LinuxResourceAuthority::new());
        let second = LinuxServiceSession::with_authority(2, LinuxResourceAuthority::new());
        assert!(!first.adopt_resource(ResourceDescriptor {
            resource_id: 88,
            binding: 3,
            scope: 18,
            rights: 0b111,
            ..ResourceDescriptor::default()
        }));
        let resource = first.mint_resource(0, 18, 0b111);

        assert!(first.validate_resource(resource));
        assert!(first.validate_resource(ResourceDescriptor {
            rights: 0b011,
            ..resource
        }));
        assert!(!first.validate_resource(ResourceDescriptor {
            scope: 19,
            ..resource
        }));
        assert!(!first.validate_resource(ResourceDescriptor {
            rights: 0b1_000,
            ..resource
        }));
        assert!(!second.validate_resource(resource));
        assert!(!second.adopt_resource(resource));

        first.close();
        assert!(!first.validate_resource(resource));
    }

    #[test]
    fn shared_authority_reclaims_resources_after_the_last_session_closes() {
        let authority = LinuxResourceAuthority::new();
        let first = LinuxServiceSession::with_authority(1, authority.clone());
        let second = LinuxServiceSession::with_authority(2, authority.clone());
        let resource = first.mint_resource(0, 18, 0b111);

        assert!(authority.retain_owner(resource.resource_id, process_owner()));
        assert!(second.adopt_resource(resource));
        first.close();
        assert!(second.validate_resource(resource));
        second.close();
        assert!(authority.release_owner(process_owner(), resource.resource_id).is_none());
        assert!(!authority.validate(resource));
    }

    #[test]
    fn release_requires_the_owner_and_preserves_other_owners() {
        let authority = LinuxResourceAuthority::new();
        let resource = authority.mint_for(100, 3, 7, 0b111);
        assert!(authority.retain_owner(resource.resource_id, 200));

        assert!(authority.release_owner(999, resource.resource_id).is_none());
        assert!(authority.validate(resource));
        assert!(authority.release_owner(100, resource.resource_id).is_none());
        assert!(authority.validate(resource));
        assert!(authority.release_owner(200, resource.resource_id).is_none());
        assert!(!authority.validate(resource));
    }

    #[test]
    fn issued_resources_are_authoritative_and_can_only_be_attenuated() {
        let authority = LinuxResourceAuthority::new();
        let resource = authority.mint(3, 7, 0b111);

        assert!(authority.validate(resource));
        assert!(authority.validate(ResourceDescriptor {
            rights: 0b011,
            ..resource
        }));
        assert!(!authority.validate(ResourceDescriptor {
            resource_id: resource.resource_id.wrapping_add(1),
            ..resource
        }));
        assert!(!authority.validate(ResourceDescriptor {
            binding: 4,
            ..resource
        }));
        assert!(!authority.validate(ResourceDescriptor {
            rights: 0b1_000,
            ..resource
        }));
    }

    #[test]
    fn memory_view_ranges_can_only_be_attenuated() {
        let original = ResourceDescriptor {
            resource_id: 7,
            binding: sys::BINDING_MEMORY_OBJECT,
            scope: naos_idl::memory_object::PROTOCOL_SCOPE,
            rights: sys::RIGHT_TRANSFER | sys::MEMORY_RIGHT_MAP,
            view_offset: 0,
            view_length: 4 * 1024 * 1024,
        };
        assert!(resource_range_is_attenuated(
            original,
            ResourceDescriptor {
                view_offset: 1 * 1024 * 1024,
                view_length: 1 * 1024 * 1024,
                ..original
            }
        ));
        assert!(!resource_range_is_attenuated(
            original,
            ResourceDescriptor {
                view_offset: 3 * 1024 * 1024,
                view_length: 2 * 1024 * 1024,
                ..original
            }
        ));
        assert!(!resource_range_is_attenuated(
            original,
            ResourceDescriptor {
                view_offset: u64::MAX,
                view_length: 1,
                ..original
            }
        ));
    }

    #[test]
    fn invalid_bulk_arithmetic_is_rejected_at_admission() {
        let request = RpcRequest {
            protocol_uuid: [0; 16],
            revision: 1,
            method_id: 1,
            request_id: 1,
            target: None,
            payload: &[],
            resources: &[],
            bulk: &[BulkBuffer::new(1, u64::MAX, 2, 1, BulkDirection::In, 1)],
        };
        assert_eq!(
            owned_request(1, request, None),
            Err(TransportError::FrameTooLarge)
        );
    }

    #[test]
    fn service_directory_maps_hierarchical_uris_and_lists_prefixes() {
        let root = std::env::temp_dir().join(std::format!(
            "naos-servicekit-services-{}",
            std::process::id()
        ));
        let names = ["block/ramdiskd/0", "block/nvmed/0", "fs/exfat/0"];
        let mut listeners = Vec::new();
        for name in names {
            let path = root.join(name);
            std::fs::create_dir_all(path.parent().unwrap()).unwrap();
            listeners.push(std::os::unix::net::UnixListener::bind(path).unwrap());
        }
        let bulk_sidecar = root.join("block/ramdiskd/0.bulk");
        listeners.push(std::os::unix::net::UnixListener::bind(bulk_sidecar).unwrap());
        let stale_path = root.join("block/stale/0");
        std::fs::create_dir_all(stale_path.parent().unwrap()).unwrap();
        let stale_listener = std::os::unix::net::UnixListener::bind(stale_path).unwrap();
        drop(stale_listener);

        let services = LinuxServiceDirectory::new(&root);
        assert_eq!(
            services.endpoint_for(crate::uri::FS_EXFAT).unwrap().path(),
            root.join("fs/exfat/0").as_path()
        );
        let mut endpoints = [
            LinuxEndpoint::new("unused-0"),
            LinuxEndpoint::new("unused-1"),
        ];
        let count = services
            .list_prefix(crate::uri::BLOCK_PREFIX, &mut endpoints)
            .unwrap();
        assert_eq!(count, 2);
        assert_eq!(endpoints[0].path(), root.join("block/nvmed/0").as_path());
        assert_eq!(endpoints[1].path(), root.join("block/ramdiskd/0").as_path());

        let mut one_endpoint = [LinuxEndpoint::new("unused")];
        assert_eq!(
            services.list_prefix(crate::uri::BLOCK_PREFIX, &mut one_endpoint),
            Err(LinuxServiceDirectoryError::Capacity)
        );
        drop(listeners);
        let _ = std::fs::remove_dir_all(root);
    }

    #[test]
    fn reused_region_keeps_memory_create_and_mapping_counts_flat() {
        // One region, many sequential transfers: the steady state must not
        // create a region or re-register it per operation.  This is the
        // transport half of the block-data-plane acceptance condition; the
        // daemon half is `RemoteBlockIo` holding one region for the device's
        // whole lifetime.
        let root = std::env::temp_dir().join(std::format!(
            "naos-servicekit-region-reuse-{}-{}",
            std::process::id(),
            std::sync::atomic::AtomicU64::new(1).fetch_add(1, std::sync::atomic::Ordering::Relaxed)
        ));
        let socket = root.join("rpc.sock");
        std::fs::create_dir_all(&root).unwrap();
        let server_socket = socket.clone();
        let server_task = std::thread::spawn(move || {
            let runtime = tokio::runtime::Builder::new_current_thread()
                .enable_io()
                .enable_time()
                .build()
                .unwrap();
            runtime.block_on(async move {
                let handler = |request: RpcRequestOwned| async move {
                    let descriptor = request.bulk[0];
                    let mut data = vec![0; descriptor.length as usize];
                    bulk::read(descriptor, &mut data).unwrap();
                    for byte in data.iter_mut() {
                        *byte = byte.wrapping_add(1);
                    }
                    bulk::write(descriptor, &data).unwrap();
                    RpcResponseOwned::success(&request, Vec::new())
                };
                let _ = serve(LinuxEndpoint::new(server_socket), handler).await;
            });
        });

        let endpoint = LinuxUdsTransport::endpoint(&socket);
        let accounting = bulk::lock_region_accounting();
        let before = bulk::region_creation_count();
        let memory = crate::memory::MemoryObject::new(4096).unwrap();
        let created = bulk::region_creation_count();
        assert_eq!(created, before + 1, "client did not create one region");
        let original_region_id = memory
            .descriptor(0, 4, BulkDirection::InOut, bulk::BULK_RIGHT_READ)
            .unwrap()
            .region_id;
        for _ in 0..200 {
            if socket.exists() {
                break;
            }
            std::thread::sleep(Duration::from_millis(2));
        }
        assert!(socket.exists(), "region-reuse RPC socket was not bound");

        let operations = 8u64;
        let mut expected: Vec<u8> = (0..32).map(|value| value as u8).collect();
        let mut attempt = 0;
        let mut done = 0u64;
        while done < operations {
            match memory.register(&endpoint) {
                Ok(()) => {
                    let descriptor = memory
                        .descriptor(
                            0,
                            expected.len() as u64,
                            BulkDirection::InOut,
                            bulk::BULK_RIGHT_READ | bulk::BULK_RIGHT_WRITE,
                        )
                        .unwrap();
                    memory.write_at(0, &expected).unwrap();
                    let request = RpcRequest {
                        protocol_uuid: [9; 16],
                        revision: 1,
                        method_id: 1,
                        request_id: 0,
                        target: None,
                        payload: &[],
                        resources: &[],
                        bulk: std::slice::from_ref(&descriptor),
                    };
                    let runtime = tokio::runtime::Builder::new_current_thread()
                        .enable_io()
                        .enable_time()
                        .build()
                        .unwrap();
                    let response =
                        runtime.block_on(LinuxUdsTransport::new().invoke_async(&endpoint, request));
                    assert!(response.is_ok(), "reused-region RPC failed: {response:?}");
                    for byte in expected.iter_mut() {
                        *byte = byte.wrapping_add(1);
                    }
                    let mut observed = vec![0; expected.len()];
                    memory.read_at(0, &mut observed).unwrap();
                    assert_eq!(observed, expected);
                    done += 1;
                }
                Err(_) => {
                    attempt += 1;
                    assert!(attempt < 400, "region registration never succeeded");
                    std::thread::sleep(Duration::from_millis(2));
                }
            }
        }

        // Eight operations ran through one region: the client created exactly
        // one, and the same region identity carried every transfer.  A
        // per-operation region would have made the creation count grow with
        // the operation count.  (The Linux transport does re-send the
        // SCM_RIGHTS registration per request because its peer releases the
        // region when the response leaves; that is a registration, not a
        // MEMORY_CREATE, and it is not a client-side mapping.)
        assert_eq!(
            bulk::region_creation_count(),
            created,
            "region was recreated during the operation loop"
        );
        let descriptor = memory
            .descriptor(0, 4, BulkDirection::InOut, bulk::BULK_RIGHT_READ)
            .unwrap();
        assert_eq!(
            descriptor.region_id, original_region_id,
            "the transfer used a different region"
        );
        drop(accounting);

        let _ = std::fs::remove_file(&socket);
        let _ = std::fs::remove_file(socket.with_extension("sock.bulk"));
        let _ = std::fs::remove_dir(&root);
        drop(server_task);
    }

    #[test]
    fn scm_rights_bulk_round_trip_does_not_use_the_rpc_payload() {
        let root = std::env::temp_dir().join(std::format!(
            "naos-servicekit-bulk-{}-{}",
            std::process::id(),
            std::sync::atomic::AtomicU64::new(1).fetch_add(1, std::sync::atomic::Ordering::Relaxed)
        ));
        let socket = root.join("rpc.sock");
        std::fs::create_dir_all(&root).unwrap();
        let server_socket = socket.clone();
        std::thread::spawn(move || {
            let runtime = tokio::runtime::Builder::new_current_thread()
                .enable_io()
                .enable_time()
                .build()
                .unwrap();
            runtime.block_on(async move {
                let handler = |request: RpcRequestOwned| async move {
                    let descriptor = request.bulk[0];
                    let mut data = vec![0; descriptor.length as usize];
                    bulk::read(descriptor, &mut data).unwrap();
                    data.reverse();
                    bulk::write(descriptor, &data).unwrap();
                    RpcResponseOwned::success(&request, Vec::new())
                };
                let _ = serve(LinuxEndpoint::new(server_socket), handler).await;
            });
        });

        let endpoint = LinuxUdsTransport::endpoint(&socket);
        let _accounting = bulk::lock_region_accounting();
        let memory = crate::memory::MemoryObject::new(128 * 1024).unwrap();
        let input: Vec<u8> = (0..128 * 1024).map(|value| value as u8).collect();
        memory.write_at(0, &input).unwrap();
        for _ in 0..200 {
            if socket.exists() {
                break;
            }
            std::thread::sleep(Duration::from_millis(2));
        }
        assert!(
            socket.exists(),
            "memory-object test RPC socket was not bound"
        );
        let mut registered = false;
        for _ in 0..200 {
            if memory.register(&endpoint).is_ok() {
                registered = true;
                break;
            }
            std::thread::sleep(Duration::from_millis(2));
        }
        assert!(registered, "memory object registration failed");
        // Two in-flight calls may share a region. The peer must retain the
        // registration until both response paths have released it.
        assert!(memory.register(&endpoint).is_ok());
        let descriptor = memory
            .descriptor(
                0,
                input.len() as u64,
                BulkDirection::InOut,
                bulk::BULK_RIGHT_READ | bulk::BULK_RIGHT_WRITE,
            )
            .unwrap();
        let request = RpcRequest {
            protocol_uuid: [9; 16],
            revision: 1,
            method_id: 1,
            request_id: 0,
            target: None,
            payload: &[],
            resources: &[],
            bulk: std::slice::from_ref(&descriptor),
        };
        let runtime = tokio::runtime::Builder::new_current_thread()
            .enable_io()
            .enable_time()
            .build()
            .unwrap();
        let second_request = RpcRequest {
            protocol_uuid: [9; 16],
            revision: 1,
            method_id: 1,
            request_id: 0,
            target: None,
            payload: &[],
            resources: &[],
            bulk: std::slice::from_ref(&descriptor),
        };
        let response =
            runtime.block_on(LinuxUdsTransport::new().invoke_async(&endpoint, second_request));
        assert!(response.is_ok(), "bulk RPC failed: {response:?}");
        let mut output = vec![0; input.len()];
        memory.read_at(0, &mut output).unwrap();
        let expected: Vec<u8> = input.into_iter().rev().collect();
        assert_eq!(output, expected);
        let response = runtime.block_on(LinuxUdsTransport::new().invoke_async(&endpoint, request));
        assert!(
            response.is_ok(),
            "second shared-region RPC failed: {response:?}"
        );
        let mut output = vec![0; descriptor.length as usize];
        memory.read_at(0, &mut output).unwrap();
        assert_eq!(output, expected.into_iter().rev().collect::<Vec<_>>());
        let _ = std::fs::remove_file(&socket);
        let _ = std::fs::remove_file(socket.with_extension("sock.bulk"));
        let _ = std::fs::remove_dir(&root);
    }
}
