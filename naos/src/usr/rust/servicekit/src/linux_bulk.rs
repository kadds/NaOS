//! Linux bulk-region registration for the generic UDS transport.
//!
//! The control frame carries only a [`BulkBuffer`].  A separate local UDS is
//! used once per region to pass the backing memfd with `SCM_RIGHTS`; the
//! request/response socket never copies the region contents.

use std::collections::BTreeMap;
use std::ffi::CString;
use std::fs::File;
use std::io::{self, Read, Write};
use std::os::fd::{AsRawFd, FromRawFd, RawFd};
use std::os::unix::fs::FileExt;
use std::os::unix::net::{UnixListener, UnixStream};
use std::path::{Path, PathBuf};
use std::string::ToString;
use std::sync::{Arc, Mutex, OnceLock};
use std::time::Duration;

use naos_idl::transport::{BulkBuffer, BulkDirection};

use super::{LinuxEndpoint, TransportError};

const MAGIC: &[u8; 8] = b"NAOBULK1";
const VERSION: u16 = 1;
const REGISTER_BYTES: usize = 40;
const CONTROL_BYTES: usize = 64;
const REGISTRATION_TIMEOUT: Duration = Duration::from_secs(2);
const MAX_REGIONS_PER_ENDPOINT: usize = 256;

/// Rights carried by a Linux bulk descriptor.  They model the direction
/// checks performed by the NaOS MemoryObject adapter; they are not Linux
/// security capabilities.
pub const BULK_RIGHT_READ: u64 = 1 << 0;
pub const BULK_RIGHT_WRITE: u64 = 1 << 1;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum BulkAccessError {
    InvalidDescriptor,
    MissingRegion,
    GenerationMismatch,
    OutOfRange,
    WrongDirection,
    Io,
}

struct Region {
    file: File,
    generation: u64,
    length: u64,
    registrations: u64,
    endpoint: PathBuf,
    owner: u64,
}

pub(crate) struct RegistrationServer {
    endpoint: PathBuf,
    stop: Arc<std::sync::atomic::AtomicBool>,
    thread: Option<std::thread::JoinHandle<()>>,
}

impl Drop for RegistrationServer {
    fn drop(&mut self) {
        self.stop.store(true, std::sync::atomic::Ordering::Release);
        if let Some(thread) = self.thread.take() {
            let _ = thread.join();
        }
        if let Ok(mut regions) = registry().lock() {
            regions.retain(|_, region| region.endpoint != self.endpoint);
        }
        let _ = std::fs::remove_file(registration_path(&self.endpoint));
    }
}

type Registry = Arc<Mutex<BTreeMap<u64, Region>>>;
static REGISTRY: OnceLock<Registry> = OnceLock::new();
static NEXT_REGION: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(1);

fn registry() -> Registry {
    REGISTRY
        .get_or_init(|| Arc::new(Mutex::new(BTreeMap::new())))
        .clone()
}

fn registration_path(path: &Path) -> PathBuf {
    let mut value = path.as_os_str().to_os_string();
    value.push(".bulk");
    PathBuf::from(value)
}

/// Private Linux backing for the platform-neutral `memory::MemoryObject`.
#[derive(Clone, Debug)]
pub(crate) struct LinuxMemoryObject {
    file: Arc<File>,
    region_id: u64,
    generation: u64,
    length: u64,
}

impl LinuxMemoryObject {
    pub fn new(length: usize) -> io::Result<Self> {
        if length == 0 {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "empty bulk region",
            ));
        }
        let name = CString::new("naos-bulk").expect("static memfd name contains no NUL byte");
        let raw = unsafe { libc::memfd_create(name.as_ptr(), libc::MFD_CLOEXEC) };
        if raw < 0 {
            return Err(io::Error::last_os_error());
        }
        let file = unsafe { File::from_raw_fd(raw) };
        file.set_len(length as u64)?;
        let counter = NEXT_REGION.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
        let region_id = (u64::from(std::process::id()) << 32) | (counter & u32::MAX as u64);
        Ok(Self {
            file: Arc::new(file),
            region_id: region_id.max(1),
            generation: 1,
            length: length as u64,
        })
    }

    /// Extent of the region in bytes.
    pub fn length(&self) -> u64 {
        self.length
    }

    pub fn descriptor(
        &self,
        offset: u64,
        length: u64,
        direction: BulkDirection,
        rights: u64,
    ) -> Result<BulkBuffer, BulkAccessError> {
        if offset
            .checked_add(length)
            .is_none_or(|end| end > self.length)
        {
            return Err(BulkAccessError::OutOfRange);
        }
        Ok(BulkBuffer::new(
            self.region_id,
            offset,
            length,
            self.generation,
            direction,
            rights,
        ))
    }

    pub fn write_at(&self, offset: u64, data: &[u8]) -> io::Result<()> {
        check_local_range(offset, data.len() as u64, self.length)?;
        let mut written = 0;
        while written < data.len() {
            let count = self.file.write_at(data, offset + written as u64)?;
            if count == 0 {
                return Err(io::Error::new(io::ErrorKind::WriteZero, "bulk write"));
            }
            written += count;
        }
        Ok(())
    }

    pub fn read_at(&self, offset: u64, data: &mut [u8]) -> io::Result<()> {
        check_local_range(offset, data.len() as u64, self.length)?;
        self.file.read_exact_at(data, offset)
    }

    /// Register the fd with the peer's bulk registry.  The acknowledgement
    /// makes registration ordered before the following RPC frame.
    pub fn register(&self, endpoint: &LinuxEndpoint) -> Result<(), TransportError> {
        let mut stream = UnixStream::connect(registration_path(endpoint.path()))
            .map_err(|_| TransportError::Io)?;
        let mut message = [0_u8; REGISTER_BYTES];
        message[..8].copy_from_slice(MAGIC);
        message[8..10].copy_from_slice(&VERSION.to_le_bytes());
        message[16..24].copy_from_slice(&self.region_id.to_le_bytes());
        message[24..32].copy_from_slice(&self.generation.to_le_bytes());
        message[32..40].copy_from_slice(&self.length.to_le_bytes());
        send_fd(&stream, &message, self.file.as_raw_fd())?;
        let mut ack = [0_u8; 1];
        stream
            .read_exact(&mut ack)
            .map_err(|_| TransportError::PeerClosed)?;
        if ack[0] == 1 {
            Ok(())
        } else {
            Err(TransportError::Bulk)
        }
    }
}

fn check_local_range(offset: u64, length: u64, capacity: u64) -> io::Result<()> {
    if offset.checked_add(length).is_none_or(|end| end > capacity) {
        Err(io::Error::new(io::ErrorKind::UnexpectedEof, "bulk range"))
    } else {
        Ok(())
    }
}

fn send_fd(stream: &UnixStream, message: &[u8], fd: RawFd) -> Result<(), TransportError> {
    let mut iov = libc::iovec {
        iov_base: message.as_ptr() as *mut libc::c_void,
        iov_len: message.len(),
    };
    let mut control = [0_usize; CONTROL_BYTES / std::mem::size_of::<usize>()];
    unsafe {
        let header = control.as_mut_ptr() as *mut libc::cmsghdr;
        (*header).cmsg_len = libc::CMSG_LEN(std::mem::size_of::<RawFd>() as u32) as _;
        (*header).cmsg_level = libc::SOL_SOCKET;
        (*header).cmsg_type = libc::SCM_RIGHTS;
        let data = libc::CMSG_DATA(header) as *mut RawFd;
        data.write(fd);
    }
    let mut message_header = unsafe { std::mem::zeroed::<libc::msghdr>() };
    message_header.msg_iov = &mut iov;
    message_header.msg_iovlen = 1;
    message_header.msg_control = control.as_mut_ptr() as *mut libc::c_void;
    message_header.msg_controllen =
        unsafe { libc::CMSG_SPACE(std::mem::size_of::<RawFd>() as u32) } as _;
    let sent = unsafe { libc::sendmsg(stream.as_raw_fd(), &message_header, 0) };
    if sent == message.len() as isize {
        Ok(())
    } else {
        Err(TransportError::Io)
    }
}

fn receive_registration(
    mut stream: UnixStream,
    registry: &Registry,
    endpoint: &Path,
) -> io::Result<()> {
    let mut message = [0_u8; REGISTER_BYTES];
    let mut iov = libc::iovec {
        iov_base: message.as_mut_ptr() as *mut libc::c_void,
        iov_len: message.len(),
    };
    let mut control = [0_usize; CONTROL_BYTES / std::mem::size_of::<usize>()];
    let mut header = unsafe { std::mem::zeroed::<libc::msghdr>() };
    header.msg_iov = &mut iov;
    header.msg_iovlen = 1;
    header.msg_control = control.as_mut_ptr() as *mut libc::c_void;
    header.msg_controllen = std::mem::size_of_val(&control);
    let received = unsafe { libc::recvmsg(stream.as_raw_fd(), &mut header, libc::MSG_WAITALL) };
    let mut received_fd = None;
    let mut current = unsafe { libc::CMSG_FIRSTHDR(&header) };
    let mut fd_count = 0usize;
    while !current.is_null() {
        let value = unsafe { &*current };
        if value.cmsg_level == libc::SOL_SOCKET && value.cmsg_type == libc::SCM_RIGHTS {
            let payload_bytes = (value.cmsg_len as usize).saturating_sub(
                unsafe { libc::CMSG_LEN(0) } as usize,
            );
            let count = payload_bytes / std::mem::size_of::<RawFd>();
            let data = unsafe { libc::CMSG_DATA(current) as *const RawFd };
            for index in 0..count {
                let raw = unsafe { data.add(index).read() };
                fd_count += 1;
                if received_fd.is_none() {
                    received_fd = Some(raw);
                } else {
                    // SCM_RIGHTS transfers ownership even when the frame is
                    // rejected.  Close every descriptor beyond the one the
                    // registration contract permits.
                    unsafe { libc::close(raw) };
                }
            }
        }
        current = unsafe { libc::CMSG_NXTHDR(&header, current) };
    }
    if received < 0 || received != REGISTER_BYTES as isize || &message[..8] != MAGIC {
        if let Some(raw) = received_fd {
            unsafe { libc::close(raw) };
        }
        let _ = stream.write_all(&[0]);
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "bulk registration",
        ));
    }
    let version = u16::from_le_bytes(message[8..10].try_into().unwrap());
    if version != VERSION {
        if let Some(raw) = received_fd {
            unsafe { libc::close(raw) };
        }
        let _ = stream.write_all(&[0]);
        return Err(io::Error::new(io::ErrorKind::InvalidData, "bulk version"));
    }
    let region_id = u64::from_le_bytes(message[16..24].try_into().unwrap());
    let generation = u64::from_le_bytes(message[24..32].try_into().unwrap());
    let length = u64::from_le_bytes(message[32..40].try_into().unwrap());
    let Some(raw) = received_fd else {
        let _ = stream.write_all(&[0]);
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "missing bulk fd",
        ));
    };
    if fd_count != 1 {
        unsafe { libc::close(raw) };
        let _ = stream.write_all(&[0]);
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "bulk registration requires exactly one fd",
        ));
    }
    let file = unsafe { File::from_raw_fd(raw) };
    if file.metadata()?.len() < length || region_id == 0 || generation == 0 {
        let _ = stream.write_all(&[0]);
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "invalid bulk region",
        ));
    }
    let Some(owner) = super::peer_pid(&stream) else {
        let _ = stream.write_all(&[0]);
        return Err(io::Error::new(io::ErrorKind::PermissionDenied, "bulk peer"));
    };
    let accepted = {
        let mut regions = registry
            .lock()
            .map_err(|_| io::Error::other("bulk registry poisoned"))?;
        let capacity_available = regions
            .values()
            .filter(|region| region.endpoint == endpoint)
            .count()
            < MAX_REGIONS_PER_ENDPOINT;
        match regions.get_mut(&region_id) {
            Some(region)
                if region.endpoint == endpoint
                    && region.owner == owner
                    && region.generation == generation
                    && region.length == length =>
            {
                region.registrations = region
                    .registrations
                    .checked_add(1)
                    .ok_or_else(|| io::Error::other("bulk registration count overflow"))?;
                true
            }
            Some(_) => false,
            None if capacity_available =>
            {
                regions.insert(
                    region_id,
                    Region {
                        file,
                        generation,
                        length,
                        registrations: 1,
                        endpoint: endpoint.to_path_buf(),
                        owner,
                    },
                );
                true
            }
            None => false,
        }
    };
    if !accepted {
        let _ = stream.write_all(&[0]);
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "bulk region identity collision",
        ));
    }
    // Do not acknowledge until the region is visible to the RPC handler.
    stream.write_all(&[1])
}

/// Bind and serve the endpoint-specific fd-registration socket.
pub fn bind(endpoint: &Path) -> io::Result<RegistrationServer> {
    let path = registration_path(endpoint);
    let _ = std::fs::remove_file(&path);
    let listener = UnixListener::bind(path)?;
    listener.set_nonblocking(true)?;
    let registry = registry();
    let endpoint = endpoint.to_path_buf();
    let thread_endpoint = endpoint.clone();
    let stop = Arc::new(std::sync::atomic::AtomicBool::new(false));
    let thread_stop = stop.clone();
    let thread = std::thread::Builder::new()
        .name("naos-bulk-registry".to_string())
        .spawn(move || {
            while !thread_stop.load(std::sync::atomic::Ordering::Acquire) {
                match listener.accept() {
                    Ok((stream, _)) => {
                        // One endpoint has one bounded registration pump.  A
                        // peer that sends a half frame cannot consume a thread
                        // forever or prevent later registrations from being
                        // accepted.
                        let _ = stream.set_read_timeout(Some(REGISTRATION_TIMEOUT));
                        let _ = stream.set_write_timeout(Some(REGISTRATION_TIMEOUT));
                        let _ = receive_registration(stream, &registry, &thread_endpoint);
                    }
                    Err(error) if error.kind() == io::ErrorKind::WouldBlock => {
                        std::thread::sleep(Duration::from_millis(10));
                    }
                    Err(_) => break,
                }
            }
        })
        .map_err(|error| io::Error::new(io::ErrorKind::Other, error))?;
    Ok(RegistrationServer {
        endpoint,
        stop,
        thread: Some(thread),
    })
}

pub fn read_for(
    endpoint: &Path,
    owner: u64,
    descriptor: BulkBuffer,
    data: &mut [u8],
) -> Result<(), BulkAccessError> {
    let direction = descriptor.direction;
    if direction != BulkDirection::In && direction != BulkDirection::InOut {
        return Err(BulkAccessError::WrongDirection);
    }
    if !descriptor.is_valid()
        || descriptor.length != data.len() as u64
        || descriptor.rights & BULK_RIGHT_READ == 0
    {
        return Err(BulkAccessError::InvalidDescriptor);
    }
    let regions = registry();
    let regions_guard = regions.lock().map_err(|_| BulkAccessError::Io)?;
    let region = regions_guard
        .get(&descriptor.region_id)
        .ok_or(BulkAccessError::MissingRegion)?;
    if region.endpoint != endpoint || region.owner != owner {
        return Err(BulkAccessError::MissingRegion);
    }
    if region.generation != descriptor.generation {
        return Err(BulkAccessError::GenerationMismatch);
    }
    check_local_range(descriptor.offset, descriptor.length, region.length)
        .map_err(|_| BulkAccessError::OutOfRange)?;
    region
        .file
        .read_exact_at(data, descriptor.offset)
        .map_err(|_| BulkAccessError::Io)
}

pub fn write_for(
    endpoint: &Path,
    owner: u64,
    descriptor: BulkBuffer,
    data: &[u8],
) -> Result<(), BulkAccessError> {
    let direction = descriptor.direction;
    if direction != BulkDirection::Out && direction != BulkDirection::InOut {
        return Err(BulkAccessError::WrongDirection);
    }
    if !descriptor.is_valid()
        || descriptor.length != data.len() as u64
        || descriptor.rights & BULK_RIGHT_WRITE == 0
    {
        return Err(BulkAccessError::InvalidDescriptor);
    }
    let regions = registry();
    let regions_guard = regions.lock().map_err(|_| BulkAccessError::Io)?;
    let region = regions_guard
        .get(&descriptor.region_id)
        .ok_or(BulkAccessError::MissingRegion)?;
    if region.endpoint != endpoint || region.owner != owner {
        return Err(BulkAccessError::MissingRegion);
    }
    if region.generation != descriptor.generation {
        return Err(BulkAccessError::GenerationMismatch);
    }
    check_local_range(descriptor.offset, descriptor.length, region.length)
        .map_err(|_| BulkAccessError::OutOfRange)?;
    let mut written = 0;
    while written < data.len() {
        let count = region
            .file
            .write_at(data, descriptor.offset + written as u64)
            .map_err(|_| BulkAccessError::Io)?;
        if count == 0 {
            return Err(BulkAccessError::Io);
        }
        written += count;
    }
    Ok(())
}

/// Check that the descriptor refers to a region whose fd was transferred to
/// this service. The caller still has to enforce the operation-specific
/// rights and direction; this function only checks identity, generation, and
/// bounds at the transport admission boundary.
pub(crate) fn is_registered_for(endpoint: &Path, owner: u64, descriptor: BulkBuffer) -> bool {
    if !descriptor.is_valid() {
        return false;
    }
    let registry = registry();
    let Ok(regions) = registry.lock() else {
        return false;
    };
    let Some(region) = regions.get(&descriptor.region_id) else {
        return false;
    };
    region.endpoint == endpoint
        && region.owner == owner
        && region.generation == descriptor.generation
        && descriptor
            .offset
            .checked_add(descriptor.length)
            .is_some_and(|end| end <= region.length)
}

/// Number of regions this process has created.  Reusing a region across a
/// sequence of operations must not increase it.
#[cfg(test)]
pub(crate) fn region_creation_count() -> u64 {
    NEXT_REGION.load(std::sync::atomic::Ordering::Relaxed)
}

/// Region accounting is process-global, so tests that create regions take this
/// lock before measuring a creation delta.
#[cfg(test)]
pub(crate) static REGION_ACCOUNTING_LOCK: std::sync::Mutex<()> = std::sync::Mutex::new(());

/// Take `REGION_ACCOUNTING_LOCK`, ignoring poisoning: one failing test must not
/// turn the others into failures.
#[cfg(test)]
pub(crate) fn lock_region_accounting() -> std::sync::MutexGuard<'static, ()> {
    REGION_ACCOUNTING_LOCK
        .lock()
        .unwrap_or_else(|error| error.into_inner())
}

/// Release a region after its invocation has completed.  A client registers
/// a fresh region for each in-flight operation, so this also bounds the
/// server's memfd table instead of retaining every peer's fd forever. Repeated
/// registrations are reference-counted because a caller may legitimately
/// share one region across concurrent invocations.
pub fn release_for(endpoint: &Path, owner: u64, descriptors: &[BulkBuffer]) {
    if let Ok(mut regions) = registry().lock() {
        for descriptor in descriptors {
            let remove = match regions.get_mut(&descriptor.region_id) {
                Some(region)
                    if region.endpoint == endpoint
                        && region.owner == owner
                        && region.generation == descriptor.generation =>
                {
                    region.registrations = region.registrations.saturating_sub(1);
                    region.registrations == 0
                }
                _ => false,
            };
            if remove {
                regions.remove(&descriptor.region_id);
            }
        }
    }
}

#[cfg(test)]
pub fn read(descriptor: BulkBuffer, data: &mut [u8]) -> Result<(), BulkAccessError> {
    let endpoint = registry()
        .lock()
        .ok()
        .and_then(|regions| regions.get(&descriptor.region_id).map(|region| region.endpoint.clone()))
        .ok_or(BulkAccessError::MissingRegion)?;
    let owner = registry()
        .lock()
        .ok()
        .and_then(|regions| regions.get(&descriptor.region_id).map(|region| region.owner))
        .ok_or(BulkAccessError::MissingRegion)?;
    read_for(&endpoint, owner, descriptor, data)
}

#[cfg(test)]
pub fn write(descriptor: BulkBuffer, data: &[u8]) -> Result<(), BulkAccessError> {
    let endpoint = registry()
        .lock()
        .ok()
        .and_then(|regions| regions.get(&descriptor.region_id).map(|region| region.endpoint.clone()))
        .ok_or(BulkAccessError::MissingRegion)?;
    let owner = registry()
        .lock()
        .ok()
        .and_then(|regions| regions.get(&descriptor.region_id).map(|region| region.owner))
        .ok_or(BulkAccessError::MissingRegion)?;
    write_for(&endpoint, owner, descriptor, data)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn regions_cannot_be_accessed_through_another_endpoint() {
        let root = std::env::temp_dir().join(std::format!(
            "naos-servicekit-bulk-scope-{}",
            std::process::id()
        ));
        std::fs::create_dir_all(&root).unwrap();
        let file_path = root.join("region");
        let file = std::fs::OpenOptions::new()
            .create(true)
            .truncate(true)
            .read(true)
            .write(true)
            .open(&file_path)
            .unwrap();
        file.set_len(4).unwrap();
        let region_id = NEXT_REGION.fetch_add(1, std::sync::atomic::Ordering::Relaxed) | 1;
        let endpoint_a = root.join("a.sock");
        let endpoint_b = root.join("b.sock");
        registry().lock().unwrap().insert(
            region_id,
            Region {
                file,
                generation: 1,
                length: 4,
                registrations: 1,
                endpoint: endpoint_a.clone(),
                owner: std::process::id() as u64,
            },
        );
        let descriptor = BulkBuffer::new(
            region_id,
            0,
            4,
            1,
            BulkDirection::InOut,
            BULK_RIGHT_READ | BULK_RIGHT_WRITE,
        );
        let owner = std::process::id() as u64;
        write_for(&endpoint_a, owner, descriptor, &[1, 2, 3, 4]).unwrap();
        let mut data = [0; 4];
        assert_eq!(
            read_for(&endpoint_b, owner, descriptor, &mut data),
            Err(BulkAccessError::MissingRegion)
        );
        assert_eq!(
            write_for(&endpoint_b, owner, descriptor, &[4, 3, 2, 1]),
            Err(BulkAccessError::MissingRegion)
        );
        assert_eq!(
            read_for(&endpoint_a, owner + 1, descriptor, &mut data),
            Err(BulkAccessError::MissingRegion)
        );
        release_for(&endpoint_b, owner, &[descriptor]);
        assert!(is_registered_for(&endpoint_a, owner, descriptor));
        release_for(&endpoint_a, owner, &[descriptor]);
        assert!(!is_registered_for(&endpoint_a, owner, descriptor));
        let _ = std::fs::remove_dir_all(root);
    }
}
