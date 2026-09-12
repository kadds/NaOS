//! Worker-side MountControl and application-facing FAT data plane.
//!
//! Open descriptions retain a path and offset, not a `fatfs::File<'_>`.
//! Operations reopen the path, keeping this single-threaded server free of
//! self-referential lifetimes while preserving POSIX sequential offsets.

extern crate alloc;

use crate::block::BlockClient;
use crate::core::{
    CREATE_DIRECTORY, FatService, OPEN_APPEND, OPEN_CREATE, OPEN_DIRECTORY, OPEN_EXCL, OPEN_READ,
    OPEN_TRUNC, OPEN_WRITE, file_stat, stat_value,
};
use crate::errno::{Errno, FsError};
use crate::worker::{FatWorker, NodeKind};
use alloc::{rc::Rc, string::String, vec, vec::Vec};
use naos_idl::directory::{self, DirectoryHandler};
use naos_idl::file::{self, FileHandler};
use naos_idl::{
    CallError, CodecError, Encoder, FailInvocation, Invocation, MAX_RESOURCES, MethodReply,
    OwnedHandle, ProtocolClientEndpoint, ProtocolServerEndpoint, ReceivedResources, ResourceSlot,
    ResourceTable, receive_request,
};
use naos_sys as sys;

pub const PROTOCOL_UUID: [u8; 16] = [
    43, 158, 60, 46, 140, 125, 79, 177, 158, 33, 76, 75, 14, 10, 16, 38,
];
pub const PROTOCOL_SCOPE: u64 = 21;
pub const PROTOCOL_RIGHTS: u64 = (1 << 22) | 1;
pub const METHOD_BIND_ROOT: u64 = 1;
pub const METHOD_BIND_NODE: u64 = 2;
pub const METHOD_SYNC: u64 = 3;
pub const METHOD_PREPARE_UNMOUNT: u64 = 4;
pub const METHOD_SHUTDOWN: u64 = 5;
pub const METHOD_LOOKUP_TARGET: u64 = 6;

const BIND_NODE_HEADER: usize = 28;
const LOOKUP_REQUEST_HEADER: usize = 24;
const MAX_WIRE_BYTES: usize = 65_536;
const LOOKUP_NOFOLLOW: u64 = 1;
const OPEN_CHROOT: u64 = 1 << 63;

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct NodeKey {
    pub node_id: u64,
    pub generation: u64,
}
impl NodeKey {
    const ZERO: Self = Self {
        node_id: 0,
        generation: 0,
    };

    fn encode(self, e: &mut naos_idl::Encoder<'_>) -> Result<(), CodecError> {
        e.put_u64(self.node_id)?;
        e.put_u64(self.generation)
    }
    fn decode(d: &mut naos_idl::Decoder<'_>) -> Result<Self, CodecError> {
        Ok(Self {
            node_id: d.get_u64()?,
            generation: d.get_u64()?,
        })
    }
}
fn decode_bind(w: &[u8]) -> Result<(NodeKey, u64, ResourceSlot), CodecError> {
    if w.len() != BIND_NODE_HEADER {
        return Err(CodecError::InvalidMessage);
    }
    let mut d = naos_idl::Decoder::new(w);
    let node = NodeKey::decode(&mut d)?;
    let flags = d.get_u64()?;
    let slot = ResourceSlot::new(d.get_u32()?).ok_or(CodecError::InvalidResource)?;
    Ok((node, flags, slot))
}
fn decode_lookup(w: &[u8]) -> Result<&[u8], CodecError> {
    if w.len() < LOOKUP_REQUEST_HEADER {
        return Err(CodecError::Truncated);
    }
    let mut d = naos_idl::Decoder::new(w);
    let _ = d.get_u64()?;
    let _ = d.get_u32()?;
    let _ = d.get_u32()?;
    let size = d.get_u64()? as usize;
    let end = LOOKUP_REQUEST_HEADER
        .checked_add(size)
        .ok_or(CodecError::Overflow)?;
    if size > 4095 {
        return Err(CodecError::BoundExceeded);
    }
    if end != w.len() {
        return Err(CodecError::InvalidMessage);
    }
    Ok(&w[LOOKUP_REQUEST_HEADER..end])
}
fn last_component(p: &[u8]) -> &[u8] {
    let p = p.strip_suffix(&[0]).unwrap_or(p);
    p.rsplit(|b| *b == b'/')
        .find(|x| !x.is_empty())
        .unwrap_or(b"/")
}
fn fail(e: FsError) -> FailInvocation {
    FailInvocation::domain(-(e.errno.to_i32() as i64))
}

/// Region handle for a request the generated dispatcher already validated
/// (slot 0 binding/scope/rights/disposition); `None` means the request
/// arrived without its mandatory bulk reference.
fn region_handle(region: Option<sys::Handle>) -> Result<sys::Handle, FailInvocation> {
    region.ok_or_else(|| fail(FsError::new(Errno::EInval)))
}

/// Raw slot-0 region handle of a request that carries a bulk payload.
fn request_region(incoming: &naos_idl::IncomingRequest<'_>) -> Option<sys::Handle> {
    ResourceSlot::new(0)
        .and_then(|slot| incoming.resources.get(slot))
        .map(OwnedHandle::get)
}

/// Bytes to move through a request region: the caller must grant a window at
/// least as large as the requested transfer, and the transfer must fit a
/// host-side length before any mapping is attempted.
fn transfer_bytes(size: u64) -> Result<usize, FailInvocation> {
    if size > 65_536 {
        return Err(fail(FsError::new(Errno::EInval)));
    }
    usize::try_from(size).map_err(|_| fail(FsError::new(Errno::EInval)))
}

/// Validate the fixed-size iovec description before mapping the request
/// region.  The region is the concatenation of the individual iovecs, so a
/// malformed layout must not make the worker read or write past the granted
/// window.
fn iov_segment_count(layout: &file::IOVLayout, size: u64) -> Result<usize, FailInvocation> {
    let count = usize::try_from(layout.segment_count)
        .map_err(|_| fail(FsError::new(Errno::EInval)))?;
    if count > layout.lengths.len() {
        return Err(fail(FsError::new(Errno::EInval)));
    }
    let total = layout.lengths[..count]
        .iter()
        .try_fold(0_u64, |total, length| total.checked_add(*length))
        .ok_or_else(|| fail(FsError::new(Errno::EInval)))?;
    if total != size {
        return Err(fail(FsError::new(Errno::EInval)));
    }
    Ok(count)
}

fn strip_nul(p: &[u8]) -> &[u8] {
    let mut n = p.len();
    while n > 0 && p[n - 1] == 0 {
        n -= 1;
    }
    &p[..n]
}
fn join(base: &str, child: &[u8], confined: bool) -> Result<String, FailInvocation> {
    let child =
        core::str::from_utf8(strip_nul(child)).map_err(|_| fail(FsError::new(Errno::EInval)))?;
    if child.starts_with('/') && !confined {
        Ok(child.into())
    } else if child.starts_with('/') {
        let child = child.trim_start_matches('/');
        if child.is_empty() {
            Ok(base.into())
        } else if base == "/" || base.is_empty() {
            Ok(alloc::format!("/{child}"))
        } else {
            Ok(alloc::format!("{base}/{child}"))
        }
    } else if base == "/" || base.is_empty() {
        Ok(alloc::format!("/{child}"))
    } else {
        Ok(alloc::format!("{base}/{child}"))
    }
}
fn absolute(p: &str) -> String {
    if p.is_empty() {
        "/".into()
    } else if p.starts_with('/') {
        p.into()
    } else {
        alloc::format!("/{p}")
    }
}

fn call_failure(error: CallError) -> FailInvocation {
    match error {
        CallError::Outcome { protocol_error, .. } if protocol_error != 0 => {
            FailInvocation::domain(protocol_error)
        }
        CallError::Status(sys::STATUS_PEER_CLOSED) | CallError::InvalidHandle => {
            fail(FsError::new(Errno::ENodev))
        }
        _ => fail(FsError::new(Errno::EIo)),
    }
}

/// Resolve an absolute path through the vfsd NamespaceBinding held alongside
/// a worker Directory. Relative paths remain local to the FAT root; absolute
/// paths must re-enter the system namespace so `/etc` cannot accidentally mean
/// `<mount-root>/etc` for an application using the `/data` endpoint.
fn resolve_absolute(
    namespace: &ProtocolClientEndpoint,
    path: &[u8],
    open_flags: u64,
) -> Result<OwnedHandle, FailInvocation> {
    if path.is_empty() || path[0] != b'/' || path.len() > 4095 {
        return Err(fail(FsError::new(Errno::EInval)));
    }
    let mut request_wire = vec![0_u8; 24 + path.len()];
    let mut encoder = Encoder::new(&mut request_wire);
    encoder
        .put_u64(open_flags)
        .and_then(|_| encoder.put_u32(8))
        .and_then(|_| encoder.put_u32(0))
        .and_then(|_| encoder.put_u64(path.len() as u64))
        .and_then(|_| encoder.put_bounded_bytes(path, 4095))
        .map_err(|_| fail(FsError::new(Errno::EInval)))?;
    let request_bytes = encoder.written();
    let frame = sys::SubmitFrame {
        struct_size: core::mem::size_of::<sys::SubmitFrame>() as u32,
        method_id: 1,
        request: request_wire.as_ptr() as u64,
        request_bytes: request_bytes as u64,
        operation_budget: 0,
        ..sys::SubmitFrame::default()
    };
    let mut raw_invocation = sys::HANDLE_INVALID;
    let status = unsafe { sys::_na_invoke_submit(namespace.get(), &frame, &mut raw_invocation) };
    if status != sys::STATUS_OK || raw_invocation == sys::HANDLE_INVALID {
        return Err(call_failure(CallError::Status(status)));
    }
    let mut invocation = unsafe { Invocation::from_raw(raw_invocation) };
    if !servicekit::wait_for_completion(invocation.get(), u64::MAX) {
        return Err(fail(FsError::new(Errno::EIo)));
    }
    let mut response_wire = [0_u8; 4];
    let mut raw_resources = [sys::HANDLE_INVALID; MAX_RESOURCES];
    let mut result = sys::ResultFrame {
        struct_size: core::mem::size_of::<sys::ResultFrame>() as u32,
        bytes: response_wire.as_mut_ptr() as u64,
        byte_capacity: response_wire.len() as u64,
        resources: raw_resources.as_mut_ptr() as u64,
        resource_capacity: MAX_RESOURCES as u64,
        ..sys::ResultFrame::default()
    };
    let status = unsafe { sys::_na_invocation_take_result(invocation.get(), &mut result) };
    if status != sys::STATUS_OK {
        return Err(call_failure(CallError::Status(status)));
    }
    invocation.mark_completed();
    if result.method_id != 1
        || result.actual_bytes != 4
        || result.actual_resources != 1
        || result.execution_outcome != 0
        || result.protocol_error != 0
    {
        for handle in raw_resources.iter().take(result.actual_resources as usize) {
            if *handle != sys::HANDLE_INVALID {
                let _ = unsafe { sys::_na_handle_close(*handle) };
            }
        }
        return Err(if result.protocol_error != 0 {
            FailInvocation::domain(result.protocol_error)
        } else {
            fail(FsError::new(Errno::EIo))
        });
    }
    let mut resources = unsafe { ReceivedResources::from_raw(&raw_resources[..1]) }
        .map_err(|_| fail(FsError::new(Errno::EIo)))?;
    let slot = u32::from_le_bytes(response_wire);
    let slot = ResourceSlot::new(slot).ok_or_else(|| fail(FsError::new(Errno::EIo)))?;
    resources
        .take(slot)
        .ok_or_else(|| fail(FsError::new(Errno::EIo)))
}

pub struct DirectoryBinding {
    pub endpoint: ProtocolServerEndpoint,
    pub path: String,
    pub namespace: Option<Rc<naos_idl::ProtocolClientEndpoint>>,
    /// When set, absolute child paths are interpreted below this binding's
    /// path.  This is the endpoint-local root installed by CHROOT.
    pub confined: bool,
    /// The root/parent anchor is held by vfsd itself and does not count as an
    /// application reference during unmount admission.
    pub anchor: bool,
    /// Backing store for the inline `path` reply, which borrows from the
    /// binding for the duration of the dispatch.  It never carries a bulk
    /// payload: the wire bounds a path at 4096 bytes.
    scratch: Vec<u8>,
}
struct FileBinding {
    endpoint: ProtocolServerEndpoint,
    description: u64,
}

pub struct MountControlServer<D: BlockClient + Clone> {
    control: ProtocolServerEndpoint,
    service: Option<FatService<D>>,
    directories: Vec<DirectoryBinding>,
    files: Vec<FileBinding>,
    pending_directories: Vec<DirectoryBinding>,
    pending_files: Vec<FileBinding>,
    /// Successful renames are applied to all open descriptions after the
    /// current endpoint finishes dispatch.  FAT handles are reopened by
    /// path for each operation, so keeping this path index coherent is what
    /// preserves POSIX "open fd survives rename" semantics.
    pending_renames: Vec<(String, String)>,
    /// vfsd's post-commit NamespaceBinding.  It is retained separately from
    /// DirectoryBinding::namespace because absolute paths on this worker must
    /// remain local to the mounted FAT root, while mutations still need the
    /// binding's reservation authority.
    mutation_namespace: Option<Rc<naos_idl::ProtocolClientEndpoint>>,
    backend_generation: u64,
    request_wire: Vec<u8>,
    reply_wire: Vec<u8>,
    next_generation: u64,
    stopped: bool,
}

impl<D: BlockClient + Clone> MountControlServer<D> {
    /// # Safety `raw` must be the uniquely owned MountControl server end.
    pub unsafe fn from_raw(raw: sys::Handle) -> Self {
        Self {
            control: unsafe { ProtocolServerEndpoint::from_raw(raw) },
            service: None,
            directories: Vec::new(),
            files: Vec::new(),
            pending_directories: Vec::new(),
            pending_files: Vec::new(),
            pending_renames: Vec::new(),
            mutation_namespace: None,
            backend_generation: 0,
            request_wire: vec![0; MAX_WIRE_BYTES],
            reply_wire: vec![0; MAX_WIRE_BYTES],
            next_generation: 1,
            stopped: false,
        }
    }
    pub fn install_worker(&mut self, worker: FatWorker<D>) {
        self.service = Some(FatService::new(worker));
    }

    /// Attach a Directory server end accepted from the public filesystem
    /// listener. The listener is intentionally separate from MountControl:
    /// vfsd owns the namespace route, while this server owns the worker's
    /// data-plane endpoints after the mount ticket commits.
    pub fn add_directory_endpoint(&mut self, endpoint: ProtocolServerEndpoint) {
        self.pending_directories.push(DirectoryBinding {
            endpoint,
            path: "/".into(),
            namespace: None,
            confined: false,
            anchor: false,
            scratch: Vec::new(),
        });
    }
    pub fn is_stopped(&self) -> bool {
        self.stopped
    }

    /// Return every endpoint that can make [`serve_once`] do work. The Tokio
    /// NaOS adapter registers this snapshot with Mio; `serve_once` itself then
    /// remains a non-blocking dispatch pass.
    pub fn wait_handles(&self) -> Vec<sys::Handle> {
        let mut handles = Vec::with_capacity(1 + self.directories.len() + self.files.len());
        handles.push(self.control.get());
        handles.extend(
            self.directories
                .iter()
                .map(|binding| binding.endpoint.get()),
        );
        handles.extend(self.files.iter().map(|binding| binding.endpoint.get()));
        handles
    }
    pub fn take_worker(&mut self) -> Option<FatWorker<D>> {
        self.service.take().map(FatService::into_worker)
    }
    pub fn serve_once(&mut self) -> Result<bool, sys::Status> {
        if self.stopped {
            return Ok(false);
        }
        let mut did = false;
        let mut wire = core::mem::take(&mut self.request_wire);
        match receive_request(&self.control, &mut wire) {
            Ok(i) => {
                did = true;
                self.serve_control(i)?;
            }
            Err(naos_idl::CallError::Status(s)) if s == sys::STATUS_WOULD_BLOCK => {}
            Err(naos_idl::CallError::Status(s)) if s == sys::STATUS_PEER_CLOSED => {
                self.stopped = true
            }
            Err(_) => {}
        }
        self.request_wire = wire;
        self.apply_pending_renames();

        // Observe closed File endpoints before servicing Directory metadata
        // operations. A caller may close a file and immediately unlink its
        // pathname; polling directories first would see the stale open
        // description and incorrectly return EBUSY for one dispatch pass.
        let mut i = 0;
        while i < self.files.len() {
            self.apply_pending_renames();
            let mut w = core::mem::take(&mut self.request_wire);
            let r = receive_request(&self.files[i].endpoint, &mut w);
            match r {
                Ok(incoming) => {
                    did = true;
                    let region = request_region(&incoming);
                    let Some(worker) = self.service.as_mut() else {
                        return Err(sys::STATUS_PEER_CLOSED);
                    };
                    let state = &mut self.files[i];
                    let mut h = FileContext {
                        service: worker,
                        state,
                        next_generation: &mut self.next_generation,
                        region,
                    };
                    let _ = file::dispatch(&mut h, incoming, &mut self.reply_wire);
                    i += 1;
                }
                Err(naos_idl::CallError::Status(s)) if s == sys::STATUS_WOULD_BLOCK => i += 1,
                Err(_) => {
                    let description = self.files[i].description;
                    self.files.swap_remove(i);
                    if let Some(service) = self.service.as_mut() {
                        service.remove_description(description);
                    }
                }
            }
            self.request_wire = w;
        }

        let mut i = 0;
        while i < self.directories.len() {
            self.apply_pending_renames();
            let mut w = core::mem::take(&mut self.request_wire);
            let r = receive_request(&self.directories[i].endpoint, &mut w);
            match r {
                Ok(incoming) => {
                    did = true;
                    let method_id = incoming.method_id;
                    if incoming.method_id == directory::METHOD_RENAME_AT
                        || incoming.method_id == directory::METHOD_LINK_AT
                    {
                        self.serve_pair_method(i, incoming);
                        i += 1;
                        self.request_wire = w;
                        continue;
                    }
                    let Some(worker) = self.service.as_mut() else {
                        return Err(sys::STATUS_PEER_CLOSED);
                    };
                    let region = request_region(&incoming);
                    let state = &mut self.directories[i];
                    let mut h = DirectoryContext {
                        service: worker,
                        state,
                        pending: &mut self.pending_directories,
                        pending_files: &mut self.pending_files,
                        renames: &mut self.pending_renames,
                        mutation_namespace: self.mutation_namespace.clone(),
                        backend_generation: self.backend_generation,
                        region,
                    };
                    if let Err(error) = directory::dispatch(&mut h, incoming, &mut self.reply_wire) {
                        log::error!("directory dispatch failed method={} error={error:?}", method_id);
                    }
                    i += 1;
                }
                Err(naos_idl::CallError::Status(s)) if s == sys::STATUS_WOULD_BLOCK => i += 1,
                Err(_) => {
                    self.directories.swap_remove(i);
                }
            }
            self.request_wire = w;
        }
        self.directories.append(&mut self.pending_directories);
        self.files.append(&mut self.pending_files);
        self.apply_pending_renames();
        Ok(did)
    }

    /// Serve the revision-2 two-dirfd mutation methods.  The moved
    /// `new_parent` endpoint is a capability, not a stable raw handle, so the
    /// worker resolves its binding path by issuing a re-entrant `path` call
    /// and pumping its own directory endpoints until that call completes.
    /// This keeps cross-directory rename within one FAT worker atomic while
    /// preserving the MOVE disposition contract.
    fn serve_pair_method(&mut self, index: usize, mut incoming: naos_idl::IncomingRequest<'_>) {
        let Some(mut responder) = incoming.responder.take() else {
            return;
        };
        if incoming.method_id == directory::METHOD_LINK_AT {
            let _ = responder.fail(fail(FsError::new(Errno::EOpNotSupp)));
            return;
        }
        let request = match directory::decode_rename_at_request(incoming.wire) {
            Ok(request) => request,
            Err(_) => {
                let _ = responder.fail(FailInvocation::protocol_violation());
                return;
            }
        };
        if directory::validate_rename_at_request_received_resources(&request, &incoming.resources)
            .is_err()
            || request.flags != 0
        {
            let _ = responder.fail(fail(FsError::new(Errno::EInval)));
            return;
        }
        let Some(parent) = incoming.resources.take(request.new_parent) else {
            let _ = responder.fail(fail(FsError::new(Errno::EInval)));
            return;
        };
        let parent = unsafe { ProtocolClientEndpoint::from_raw(parent.into_raw()) };
        let parent_path = match self.probe_directory_path(&parent) {
            Ok(path) => path,
            Err(error) => {
                let _ = responder.fail(error);
                return;
            }
        };
        let old_base = self.directories[index].path.clone();
        let old_path = match join(&old_base, request.first, self.directories[index].confined) {
            Ok(path) => path,
            Err(error) => {
                let _ = responder.fail(error);
                return;
            }
        };
        let new_path = match join(&parent_path, request.second, false) {
            Ok(path) => path,
            Err(error) => {
                let _ = responder.fail(error);
                return;
            }
        };
        let Some(worker) = self.service.as_ref() else {
            let _ = responder.fail(fail(FsError::new(Errno::EIo)));
            return;
        };
        let old_side = match lookup_mutation_side(
            worker,
            &old_path,
            self.backend_generation,
            true,
        ) {
            Ok(side) => side,
            Err(error) => {
                let _ = responder.fail(fail(error));
                return;
            }
        };
        let new_side = match lookup_mutation_side(
            worker,
            &new_path,
            self.backend_generation,
            false,
        ) {
            Ok(side) => side,
            Err(error) => {
                let _ = responder.fail(fail(error));
                return;
            }
        };
        let old_target = old_side.target.unwrap_or(NodeKey::ZERO);
        let mutation = with_mutation(
            self.mutation_namespace.as_deref(),
            crate::namespace_binding::OP_RENAME,
            old_side.parent,
            old_target,
            new_side.parent,
            new_side.target.unwrap_or(NodeKey::ZERO),
            &old_side.name,
            &new_side.name,
            || worker.rename(&old_path, &new_path),
        );
        if let Err(error) = mutation {
            let _ = responder.fail(error);
            return;
        }
        self.pending_renames.push((old_path, new_path));
        let mut response_wire = [0_u8; 64];
        let written = directory::encode_rename_at_response(
            &directory::rename_at_response {},
            &mut response_wire,
        )
        .unwrap_or(0);
        let _ = responder.reply(&response_wire[..written], &[]);
    }

    fn probe_directory_path(
        &mut self,
        endpoint: &ProtocolClientEndpoint,
    ) -> Result<String, FailInvocation> {
        let mut request_wire = [0_u8; 32];
        let mut invocation = directory::submit_path(
            endpoint,
            &directory::path_request {},
            ResourceTable::new(),
            &mut request_wire,
            0,
        )
        .map_err(call_failure)?;
        let mut response_wire = [0_u8; MAX_WIRE_BYTES];
        for _ in 0..128 {
            match directory::take_path(&mut invocation, &mut response_wire) {
                Ok(response) => {
                    let value = core::str::from_utf8(response.path)
                        .map_err(|_| fail(FsError::new(Errno::EInval)))?;
                    return Ok(String::from(value));
                }
                Err(naos_idl::CallError::Status(sys::STATUS_WOULD_BLOCK)) => {}
                Err(error) => return Err(call_failure(error)),
            }
            let mut progressed = false;
            let count = self.directories.len();
            for index in 0..count {
                let mut wire = vec![0_u8; MAX_WIRE_BYTES];
                match receive_request(&self.directories[index].endpoint, &mut wire) {
                    Ok(request) => {
                        progressed = true;
                        let method_id = request.method_id;
                        if request.method_id == directory::METHOD_RENAME_AT
                            || request.method_id == directory::METHOD_LINK_AT
                        {
                            self.serve_pair_method(index, request);
                        } else {
                            let Some(worker) = self.service.as_mut() else {
                                return Err(fail(FsError::new(Errno::EIo)));
                            };
                            let region = request_region(&request);
                            let state = &mut self.directories[index];
                            let mut handler = DirectoryContext {
                                service: worker,
                                state,
                                pending: &mut self.pending_directories,
                                pending_files: &mut self.pending_files,
                                renames: &mut self.pending_renames,
                                mutation_namespace: self.mutation_namespace.clone(),
                                backend_generation: self.backend_generation,
                                region,
                            };
                            if let Err(error) =
                                directory::dispatch(&mut handler, request, &mut self.reply_wire)
                            {
                                log::error!(
                                    "directory dispatch failed method={} error={error:?}",
                                    method_id
                                );
                            }
                        }
                    }
                    Err(naos_idl::CallError::Status(s)) if s == sys::STATUS_WOULD_BLOCK => {}
                    Err(_) => {}
                }
            }
            if !progressed {
                unsafe { sys::_s_yield() };
            }
        }
        Err(fail(FsError::new(Errno::EIo)))
    }

    fn apply_pending_renames(&mut self) {
        for (old, new) in self.pending_renames.drain(..) {
            for binding in &mut self.directories {
                rewrite_binding_path(&mut binding.path, &old, &new);
            }
            for binding in &mut self.pending_directories {
                rewrite_binding_path(&mut binding.path, &old, &new);
            }
            if let Some(service) = self.service.as_mut() {
                service.rewrite_open_paths(&old, &new);
            }
        }
    }
    fn serve_control(
        &mut self,
        mut incoming: naos_idl::IncomingRequest<'_>,
    ) -> Result<(), sys::Status> {
        let Some(mut responder) = incoming.responder.take() else {
            return Ok(());
        };
        match incoming.method_id {
            METHOD_BIND_ROOT => self.bind_directory(&mut responder, "/".into(), None, true),
            METHOD_BIND_NODE => {
                let (node, flags, slot) = match decode_bind(incoming.wire) {
                    Ok(x) => x,
                    Err(_) => return responder.fail(FailInvocation::protocol_violation()),
                };
                if flags != 0
                    || node.node_id == 0
                    || node.generation == 0
                    || incoming.resources.len() != 1
                    || slot.index() != 0
                {
                    return responder.fail(fail(FsError::new(Errno::EInval)));
                }
                if naos_idl::validate_received_resource(
                    &incoming.resources,
                    slot,
                    sys::BINDING_CLIENT_END,
                    18,
                    sys::RIGHT_TRANSFER,
                    (1 << 21) | 1,
                )
                .is_err()
                {
                    return responder.fail(fail(FsError::new(Errno::EInval)));
                }
                let Some(ns) = incoming.resources.take(slot).map(|resource| {
                    let raw = resource.into_raw();
                    Rc::new(unsafe { naos_idl::ProtocolClientEndpoint::from_raw(raw) })
                }) else {
                    return responder.fail(fail(FsError::new(Errno::EInval)));
                };
                self.backend_generation = node.generation;
                self.mutation_namespace = Some(ns);
                // The moved NamespaceBinding is the vfsd-issued proof that
                // this worker root may be published.  The returned Directory
                // is the mounted filesystem's own root, however: absolute
                // application paths must stay inside that root rather than
                // being sent back to vfsd's placeholder system tree.
                self.bind_directory(&mut responder, "/".into(), None, true)
            }
            METHOD_SYNC => {
                let Some(worker) = self.service.as_ref() else {
                    return responder.fail(fail(FsError::new(Errno::EIo)));
                };
                worker.sync(true).map_err(|_| sys::STATUS_IO_ERROR)?;
                responder.reply(&[], &[])
            }
            METHOD_PREPARE_UNMOUNT => {
                if self.directories.iter().any(|directory| !directory.anchor)
                    || !self.files.is_empty()
                    || !self.pending_directories.is_empty()
                    || !self.pending_files.is_empty()
                {
                    responder.fail(fail(FsError::new(Errno::EBusy)))
                } else {
                    responder.reply(&[], &[])
                }
            }
            METHOD_SHUTDOWN => {
                let result = responder.reply(&[], &[]);
                if result.is_ok() {
                    self.stopped = true;
                }
                result
            }
            METHOD_LOOKUP_TARGET => {
                let p = match decode_lookup(incoming.wire) {
                    Ok(x) => x,
                    Err(_) => return responder.fail(FailInvocation::protocol_violation()),
                };
                let path = match core::str::from_utf8(strip_nul(p)) {
                    Ok(path) => path,
                    Err(_) => return responder.fail(fail(FsError::new(Errno::EInval))),
                };
                let Some(worker) = self.service.as_ref() else {
                    return responder.fail(fail(FsError::new(Errno::EIo)));
                };
                let side = match lookup_mutation_side(
                    worker,
                    path,
                    self.backend_generation,
                    true,
                ) {
                    Ok(side) => side,
                    Err(error) => return responder.fail(fail(error)),
                };
                let mut e = naos_idl::Encoder::new(&mut self.reply_wire);
                let target = side.target.unwrap_or(NodeKey::ZERO);
                if side.parent.encode(&mut e)
                    .and_then(|_| target.encode(&mut e))
                    .and_then(|_| e.put_u64(side.name.len() as u64))
                    .and_then(|_| e.put_bounded_bytes(&side.name, 255))
                    .is_err()
                {
                    return responder.fail(FailInvocation::domain(-5));
                }
                let written = e.written();
                drop(e);
                responder.reply(&self.reply_wire[..written], &[])
            }
            _ => responder.fail(FailInvocation::unsupported()),
        }
        .map(|_| ())
    }
    fn bind_directory(
        &mut self,
        responder: &mut naos_idl::ResponderHandle,
        path: String,
        namespace: Option<Rc<naos_idl::ProtocolClientEndpoint>>,
        anchor: bool,
    ) -> Result<(), sys::Status> {
        let (client, server) =
            directory::create_endpoints(None).map_err(|_| sys::STATUS_RESOURCE_EXHAUSTED)?;
        let client = unsafe { OwnedHandle::from_raw(client.into_raw()) };
        let mut t = ResourceTable::new();
        let slot = t
            .push_move(client)
            .map_err(|_| sys::STATUS_RESOURCE_EXHAUSTED)?;
        self.pending_directories.push(DirectoryBinding {
            endpoint: server,
            path,
            namespace,
            confined: false,
            anchor,
            scratch: Vec::new(),
        });
        let response = slot.index().to_le_bytes();
        responder.reply(&response, t.as_slice())?;
        t.commit_move();
        Ok(())
    }
}

struct DirectoryContext<'a, D: BlockClient + Clone> {
    service: &'a mut FatService<D>,
    state: &'a mut DirectoryBinding,
    pending: &'a mut Vec<DirectoryBinding>,
    pending_files: &'a mut Vec<FileBinding>,
    renames: &'a mut Vec<(String, String)>,
    mutation_namespace: Option<Rc<naos_idl::ProtocolClientEndpoint>>,
    backend_generation: u64,
    /// Slot-0 region handle of the request being dispatched, already checked
    /// by the generated binding/scope/rights/disposition validation.
    region: Option<sys::Handle>,
}

fn rewrite_binding_path(path: &mut String, old: &str, new: &str) {
    let is_descendant = path.len() > old.len()
        && path.starts_with(old)
        && path.as_bytes().get(old.len()) == Some(&b'/');
    if path == old || is_descendant {
        let suffix = String::from(&path[old.len()..]);
        path.clear();
        path.push_str(new);
        path.push_str(&suffix);
    }
}

struct MutationSide {
    parent: NodeKey,
    target: Option<NodeKey>,
    name: Vec<u8>,
}

/// Split a worker-local path without changing its semantics. The parent is
/// looked up independently so begin_mutation never reserves a guessed node.
fn mutation_path_parts(path: &str) -> Result<(String, Vec<u8>), FsError> {
    let relative = path.trim_start_matches('/');
    let relative = relative.strip_suffix('/').unwrap_or(relative);
    if relative.is_empty()
        || relative
            .split('/')
            .any(|component| component.is_empty() || component == "." || component == "..")
    {
        return Err(FsError::new(Errno::EInval));
    }
    let (parent, name) = match relative.rsplit_once('/') {
        Some((parent, name)) => (parent, name),
        None => ("", relative),
    };
    if name.is_empty() || name.len() > 255 {
        return Err(FsError::new(Errno::ENameTooLong));
    }
    let parent = if parent.is_empty() {
        String::from("/")
    } else {
        alloc::format!("/{parent}")
    };
    Ok((parent, name.as_bytes().to_vec()))
}

fn lookup_mutation_side<D: BlockClient + Clone>(
    service: &FatService<D>,
    path: &str,
    generation: u64,
    target_required: bool,
) -> Result<MutationSide, FsError> {
    let (parent_path, name) = mutation_path_parts(path)?;
    let parent_stat = service.lookup(&parent_path)?;
    if parent_stat.kind != NodeKind::Dir {
        return Err(FsError::new(Errno::ENotDir));
    }
    let target = match service.lookup(path) {
        Ok(stat) => Some(NodeKey {
            node_id: stat.ino,
            generation,
        }),
        Err(error) if error.errno == Errno::ENoent && !target_required => None,
        Err(error) => return Err(error),
    };
    Ok(MutationSide {
        parent: NodeKey {
            node_id: parent_stat.ino,
            generation,
        },
        target,
        name,
    })
}

/// Run one worker-local metadata operation under the vfsd reservation. The
/// None case is reserved for host fixture servers, which do not have a
/// kernel capability carrying NamespaceBinding.
fn with_mutation<T>(
    namespace: Option<&ProtocolClientEndpoint>,
    operation: u32,
    old_parent: NodeKey,
    old_target: NodeKey,
    new_parent: NodeKey,
    new_target: NodeKey,
    old_name: &[u8],
    new_name: &[u8],
    apply: impl FnOnce() -> Result<T, FsError>,
) -> Result<T, FailInvocation> {
    let ticket = match namespace {
        Some(namespace) => Some(
            crate::namespace_binding::begin_mutation(
                namespace,
                operation,
                old_parent,
                old_target,
                new_parent,
                new_target,
                old_name,
                new_name,
            )
            .map_err(|error| {
                log::error!(
                    "mutation reservation request failed operation={} error={error:?}",
                    operation
                );
                call_failure(error)
            })?,
        ),
        None => None,
    };

    match apply() {
        Ok(value) => {
            if let Some(ticket) = ticket {
                crate::namespace_binding::commit_or_reconcile(&ticket).map_err(call_failure)?;
            }
            Ok(value)
        }
        Err(error) => {
            if let Some(ticket) = ticket {
                if let Err(abort_error) = crate::namespace_binding::abort(&ticket) {
                    log::error!(
                        "mutation apply failed errno={} and abort failed error={abort_error:?}",
                        error.errno.to_i32()
                    );
                    return Err(call_failure(abort_error));
                }
            }
            log::error!("mutation apply failed errno={}", error.errno.to_i32());
            Err(fail(error))
        }
    }
}

impl<D: BlockClient + Clone> DirectoryContext<'_, D> {
    fn join_path(&self, child: &[u8]) -> Result<String, FailInvocation> {
        join(&self.state.path, child, self.state.confined)
    }

    fn mutation_namespace(&self) -> Option<&ProtocolClientEndpoint> {
        self.mutation_namespace.as_deref()
    }

    fn new_side(&self, path: &str) -> Result<MutationSide, FailInvocation> {
        lookup_mutation_side(self.service, path, self.backend_generation, false).map_err(|error| {
            log::error!(
                "mutation new-side lookup failed path={} errno={}",
                path,
                error.errno.to_i32()
            );
            fail(error)
        })
    }

    fn old_side(&self, path: &str) -> Result<MutationSide, FailInvocation> {
        lookup_mutation_side(self.service, path, self.backend_generation, true).map_err(|error| {
            log::error!(
                "mutation old-side lookup failed path={} errno={}",
                path,
                error.errno.to_i32()
            );
            fail(error)
        })
    }

    fn open_dir(
        &mut self,
        path: String,
        confined: bool,
    ) -> Result<(ResourceSlot, ResourceTable<'static>), FailInvocation> {
        let (c, s) =
            directory::create_endpoints(None).map_err(|_| fail(FsError::new(Errno::EIo)))?;
        let c = unsafe { OwnedHandle::from_raw(c.into_raw()) };
        let mut t = ResourceTable::new();
        let slot = t.push_move(c).map_err(|_| fail(FsError::new(Errno::EIo)))?;
        self.pending.push(DirectoryBinding {
            endpoint: s,
            path,
            namespace: self.state.namespace.clone(),
            confined,
            anchor: false,
            scratch: Vec::new(),
        });
        Ok((slot, t))
    }
    fn open_file(
        &mut self,
        path: String,
        flags: u64,
    ) -> Result<(ResourceSlot, ResourceTable<'static>), FailInvocation> {
        let (c, s) = file::create_endpoints(None).map_err(|_| fail(FsError::new(Errno::EIo)))?;
        let c = unsafe { OwnedHandle::from_raw(c.into_raw()) };
        let mut t = ResourceTable::new();
        let slot = t.push_move(c).map_err(|_| fail(FsError::new(Errno::EIo)))?;
        let description = self.service.open_description(path, flags);
        self.pending_files.push(FileBinding {
            endpoint: s,
            description,
        });
        Ok((slot, t))
    }
}
impl<D: BlockClient + Clone> DirectoryHandler for DirectoryContext<'_, D> {
    fn open<'s>(
        &'s mut self,
        r: directory::open_request<'_>,
    ) -> Result<MethodReply<'s, directory::open_response>, FailInvocation> {
        let raw_path = strip_nul(r.path);
        let chroot = r.flags & OPEN_CHROOT != 0;
        if raw_path.len() > 1
            && raw_path[0] == b'/'
            && self.state.namespace.is_some()
            && !self.state.confined
            && !chroot
        {
            let namespace = self.state.namespace.as_ref().expect("checked above");
            let object = resolve_absolute(namespace, raw_path, r.flags)?;
            let mut resources = ResourceTable::new();
            let slot = resources
                .push_move(object)
                .map_err(|_| fail(FsError::new(Errno::EIo)))?;
            return Ok(MethodReply::with_resources(
                directory::open_response { object: slot },
                resources,
            ));
        }
        let p = self.join_path(r.path).map_err(|error| {
            log::error!("directory open path join failed base={} errno={:?}", self.state.path, error);
            error
        })?;
        let n = match self.service.lookup(&p) {
            Ok(n) => n,
            Err(e) if e.errno == Errno::ENoent && r.flags & OPEN_CREATE != 0 => {
                if r.flags & OPEN_DIRECTORY != 0 {
                    return Err(fail(e));
                }
                let side = self.new_side(&p)?;
                with_mutation(
                    self.mutation_namespace(),
                    crate::namespace_binding::OP_CREATE,
                    NodeKey::ZERO,
                    NodeKey::ZERO,
                    side.parent,
                    NodeKey::ZERO,
                    &[],
                    &side.name,
                    || self.service.create_file(&p).map(|_| ()),
                )?;
                match self.service.lookup(&p) {
                    Ok(value) => value,
                    Err(error) => {
                        log::error!(
                            "directory open created path lookup failed path={} errno={}",
                            p,
                            error.errno.to_i32()
                        );
                        return Err(fail(error));
                    }
                }
            }
            Err(e) => {
                log::error!("directory open lookup failed path={} errno={:?}", p, e.errno);
                return Err(fail(e));
            }
        };
        if r.flags & OPEN_CREATE != 0 && r.mode & OPEN_EXCL != 0 {
            return Err(fail(FsError::new(Errno::EExist)));
        }
        if n.kind == NodeKind::Dir {
            if r.flags & OPEN_TRUNC != 0 {
                return Err(fail(FsError::new(Errno::EIsDir)));
            }
            let (slot, t) = self.open_dir(absolute(&p), self.state.confined || chroot)?;
            Ok(MethodReply::with_resources(
                directory::open_response { object: slot },
                t,
            ))
        } else {
            if r.flags & OPEN_DIRECTORY != 0 {
                return Err(fail(FsError::new(Errno::ENotDir)));
            }
            if r.flags & OPEN_TRUNC != 0 {
                self.service.truncate(&p).map_err(fail)?;
            }
            let (slot, t) = self.open_file(absolute(&p), r.mode)?;
            Ok(MethodReply::with_resources(
                directory::open_response { object: slot },
                t,
            ))
        }
    }
    fn list<'s>(
        &'s mut self,
        r: directory::list_request,
    ) -> Result<MethodReply<'s, directory::list_response>, FailInvocation> {
        // `requested_bytes == 0` means "fill the maximum page"; the granted
        // window still bounds the write.  A zero-byte window is a legal empty
        // page and must not install a zero-length mapping.
        let window = if r.requested_bytes == 0 {
            65_536
        } else {
            r.requested_bytes
        };
        let window = usize::try_from(window).map_err(|_| fail(FsError::new(Errno::EInval)))?;
        if window == 0 {
            return Ok(MethodReply::new(directory::list_response {
                next: r.offset,
                count: 0,
                bytes: 0,
            }));
        }
        let handle = region_handle(self.region)?;
        let (next, count, bytes) = servicekit::memory::with_region_write(
            handle,
            0,
            window,
            |out| self.service.list(&self.state.path, r.offset, r.requested_bytes, out),
        )
        .map_err(|_| fail(FsError::new(Errno::EInval)))?
        .map_err(fail)?;
        Ok(MethodReply::new(directory::list_response {
            next,
            count,
            bytes,
        }))
    }
    fn stat<'s>(
        &'s mut self,
        _: directory::stat_request,
    ) -> Result<MethodReply<'s, directory::stat_response>, FailInvocation> {
        let n = self.service.lookup(&self.state.path).map_err(fail)?;
        Ok(MethodReply::new(directory::stat_response {
            value: stat_value(n, self.service.device_id()),
        }))
    }
    fn create<'s>(
        &'s mut self,
        r: directory::create_request<'_>,
    ) -> Result<MethodReply<'s, directory::create_response>, FailInvocation> {
        let p = self.join_path(r.path)?;
        let side = self.new_side(&p)?;
        let operation = if r.flags & CREATE_DIRECTORY != 0 {
            crate::namespace_binding::OP_MKDIR
        } else {
            crate::namespace_binding::OP_CREATE
        };
        with_mutation(
            self.mutation_namespace(),
            operation,
            NodeKey::ZERO,
            NodeKey::ZERO,
            side.parent,
            NodeKey::ZERO,
            &[],
            &side.name,
            || {
                if r.flags & CREATE_DIRECTORY != 0 {
                    self.service.mkdir(&p)
                } else {
                    self.service.create_file(&p).map(|_| ())
                }
            },
        )?;
        Ok(MethodReply::new(directory::create_response {}))
    }
    fn remove<'s>(
        &'s mut self,
        r: directory::remove_request<'_>,
    ) -> Result<MethodReply<'s, directory::remove_response>, FailInvocation> {
        let p = self.join_path(r.path)?;
        // FAT has no inode-based reopen API. Refuse unlink while a live File
        // endpoint still names this object instead of returning success and
        // making the open description fail on its next I/O.
        if r.flags & CREATE_DIRECTORY == 0 && self.service.has_open_path(&p) {
            return Err(fail(FsError::new(Errno::EBusy)));
        }
        let side = self.old_side(&p)?;
        let operation = if r.flags & CREATE_DIRECTORY != 0 {
            crate::namespace_binding::OP_RMDIR
        } else {
            crate::namespace_binding::OP_UNLINK
        };
        with_mutation(
            self.mutation_namespace(),
            operation,
            side.parent,
            side.target.unwrap_or(NodeKey::ZERO),
            NodeKey::ZERO,
            NodeKey::ZERO,
            &side.name,
            &[],
            || {
                if r.flags & CREATE_DIRECTORY != 0 {
                    self.service.rmdir(&p)
                } else {
                    self.service.unlink(&p)
                }
            },
        )?;
        Ok(MethodReply::new(directory::remove_response {}))
    }
    fn path<'s>(
        &'s mut self,
        _: directory::path_request,
    ) -> Result<MethodReply<'s, directory::path_response<'s>>, FailInvocation> {
        self.state.scratch = self.state.path.as_bytes().to_vec();
        Ok(MethodReply::new(directory::path_response {
            path: &self.state.scratch,
        }))
    }
    fn access<'s>(
        &'s mut self,
        r: directory::access_request<'_>,
    ) -> Result<MethodReply<'s, directory::access_response>, FailInvocation> {
        let p = self.join_path(r.path)?;
        let mode = u32::try_from(r.mode).map_err(|_| fail(FsError::new(Errno::EInval)))?;
        self.service.access(&p, mode).map_err(fail)?;
        Ok(MethodReply::new(directory::access_response {}))
    }
    fn rename<'s>(
        &'s mut self,
        r: directory::rename_request<'_>,
    ) -> Result<MethodReply<'s, directory::rename_response>, FailInvocation> {
        let old_path = self.join_path(r.first)?;
        let new_path = self.join_path(r.second)?;
        let old_side = self.old_side(&old_path)?;
        let new_side = self.new_side(&new_path)?;
        with_mutation(
            self.mutation_namespace(),
            crate::namespace_binding::OP_RENAME,
            old_side.parent,
            old_side.target.unwrap_or(NodeKey::ZERO),
            new_side.parent,
            new_side.target.unwrap_or(NodeKey::ZERO),
            &old_side.name,
            &new_side.name,
            || self.service.rename(&old_path, &new_path),
        )?;
        self.renames.push((old_path, new_path));
        Ok(MethodReply::new(directory::rename_response {}))
    }
    fn link<'s>(
        &'s mut self,
        _: directory::link_request<'_>,
    ) -> Result<MethodReply<'s, directory::link_response>, FailInvocation> {
        Err(fail(FsError::new(Errno::EOpNotSupp)))
    }
    fn symlink<'s>(
        &'s mut self,
        _: directory::symlink_request<'_>,
    ) -> Result<MethodReply<'s, directory::symlink_response>, FailInvocation> {
        Err(fail(FsError::new(Errno::EOpNotSupp)))
    }
    fn readlink<'s>(
        &'s mut self,
        _: directory::readlink_request<'_>,
    ) -> Result<MethodReply<'s, directory::readlink_response<'s>>, FailInvocation> {
        Err(fail(FsError::new(Errno::EOpNotSupp)))
    }
    fn set_current<'s>(
        &'s mut self,
        _: directory::set_current_request,
    ) -> Result<MethodReply<'s, directory::set_current_response>, FailInvocation> {
        Err(fail(FsError::new(Errno::EOpNotSupp)))
    }
    fn set_root<'s>(
        &'s mut self,
        _: directory::set_root_request,
    ) -> Result<MethodReply<'s, directory::set_root_response>, FailInvocation> {
        Err(fail(FsError::new(Errno::EOpNotSupp)))
    }
    fn clone_binding<'s>(
        &'s mut self,
        _: directory::clone_binding_request,
    ) -> Result<MethodReply<'s, directory::clone_binding_response>, FailInvocation> {
        let (slot, t) = self.open_dir(self.state.path.clone(), self.state.confined)?;
        Ok(MethodReply::with_resources(
            directory::clone_binding_response { directory: slot },
            t,
        ))
    }
    fn stat_node<'s>(
        &'s mut self,
        r: directory::stat_node_request<'_>,
    ) -> Result<MethodReply<'s, directory::stat_node_response>, FailInvocation> {
        if r.flags & !LOOKUP_NOFOLLOW != 0 {
            return Err(fail(FsError::new(Errno::EInval)));
        }
        let p = self.join_path(r.path)?;
        let n = self.service.lookup(&p).map_err(fail)?;
        Ok(MethodReply::new(directory::stat_node_response {
            value: stat_value(n, self.service.device_id()),
        }))
    }
    fn sync<'s>(
        &'s mut self,
        _: directory::sync_request,
    ) -> Result<MethodReply<'s, directory::sync_response>, FailInvocation> {
        self.service.sync(false).map_err(fail)?;
        Ok(MethodReply::new(directory::sync_response {}))
    }
    fn rename_at<'s>(
        &'s mut self,
        _: directory::rename_at_request<'_>,
    ) -> Result<MethodReply<'s, directory::rename_at_response>, FailInvocation> {
        Err(fail(FsError::new(Errno::EOpNotSupp)))
    }
    fn link_at<'s>(
        &'s mut self,
        _: directory::link_at_request<'_>,
    ) -> Result<MethodReply<'s, directory::link_at_response>, FailInvocation> {
        Err(fail(FsError::new(Errno::EOpNotSupp)))
    }
}

struct FileContext<'a, D: BlockClient + Clone> {
    service: &'a mut FatService<D>,
    state: &'a mut FileBinding,
    next_generation: &'a mut u64,
    /// Slot-0 region handle of the request being dispatched, already checked
    /// by the generated binding/scope/rights/disposition validation.
    region: Option<sys::Handle>,
}
impl<D: BlockClient + Clone> FileContext<'_, D> {
    fn description(&self) -> Result<crate::core::OpenDescription, FailInvocation> {
        self.service
            .description(self.state.description)
            .cloned()
            .ok_or_else(|| fail(FsError::new(Errno::EBadf)))
    }

    fn readable(&self) -> Result<(), FailInvocation> {
        if self.description()?.flags & OPEN_READ != 0 {
            Ok(())
        } else {
            Err(fail(FsError::new(Errno::EBadf)))
        }
    }
    fn writable(&self) -> Result<(), FailInvocation> {
        if self.description()?.flags & OPEN_WRITE != 0 {
            Ok(())
        } else {
            Err(fail(FsError::new(Errno::EBadf)))
        }
    }
    /// Read from `o` into the caller's mapped region window.  The bytes go
    /// straight into the region, so no intermediate buffer is needed.
    fn read_at(&mut self, o: i64, out: &mut [u8]) -> Result<usize, FailInvocation> {
        if o < 0 {
            return Err(fail(FsError::new(Errno::EInval)));
        }
        let description = self.description()?;
        self.service
            .read_at(&description.path, o as u64, out)
            .map_err(fail)
    }
}
impl<D: BlockClient + Clone> FileHandler for FileContext<'_, D> {
    fn pread<'s>(
        &'s mut self,
        r: file::pread_request,
    ) -> Result<MethodReply<'s, file::pread_response>, FailInvocation> {
        self.readable()?;
        let bytes = transfer_bytes(r.size)?;
        // A zero-byte transfer is a legal empty read and must not install a
        // zero-length mapping.
        if bytes == 0 {
            return Ok(MethodReply::new(file::pread_response { count: 0 }));
        }
        let handle = region_handle(self.region)?;
        let offset = r.offset;
        let count = servicekit::memory::with_region_write(
            handle,
            0,
            bytes,
            |out| self.read_at(offset, out),
        )
        .map_err(|_| fail(FsError::new(Errno::EInval)))??;
        Ok(MethodReply::new(file::pread_response {
            count: count as u64,
        }))
    }
    fn pwrite<'s>(
        &'s mut self,
        r: file::pwrite_request,
    ) -> Result<MethodReply<'s, file::pwrite_response>, FailInvocation> {
        self.writable()?;
        let bytes = transfer_bytes(r.size)?;
        if r.offset < 0 {
            return Err(fail(FsError::new(Errno::EInval)));
        }
        if bytes == 0 {
            return Ok(MethodReply::new(file::pwrite_response { count: 0 }));
        }
        let handle = region_handle(self.region)?;
        let description = self.description()?;
        let offset = r.offset as u64;
        let count = servicekit::memory::with_region_read(
            handle,
            0,
            bytes,
            |data| self.service.write_at(&description.path, offset, data),
        )
        .map_err(|_| fail(FsError::new(Errno::EInval)))?
        .map_err(fail)?;
        Ok(MethodReply::new(file::pwrite_response {
            count: count as u64,
        }))
    }
    fn seek<'s>(
        &'s mut self,
        r: file::seek_request,
    ) -> Result<MethodReply<'s, file::seek_response>, FailInvocation> {
        // Wire codes follow mlibc/Rust NaOS protocol constants: 0=current,
        // 1=begin, 2=end (not the libc SEEK_* values themselves).
        let description = self.description()?;
        let b = match r.whence {
            0 => description.offset,
            1 => 0,
            2 => self.service.lookup(&description.path).map_err(fail)?.size as i64,
            _ => return Err(fail(FsError::new(Errno::EInval))),
        };
        let n = b
            .checked_add(r.offset)
            .ok_or_else(|| fail(FsError::new(Errno::EInval)))?;
        if n < 0 {
            return Err(fail(FsError::new(Errno::EInval)));
        }
        if !self.service.set_offset(self.state.description, n) {
            return Err(fail(FsError::new(Errno::EBadf)));
        }
        Ok(MethodReply::new(file::seek_response { offset: n }))
    }
    fn stat<'s>(
        &'s mut self,
        _: file::stat_request,
    ) -> Result<MethodReply<'s, file::stat_response>, FailInvocation> {
        let description = self.description()?;
        let n = self.service.lookup(&description.path).map_err(fail)?;
        Ok(MethodReply::new(file::stat_response {
            value: file_stat(n, self.service.device_id()),
        }))
    }
    fn sync<'s>(
        &'s mut self,
        _: file::sync_request,
    ) -> Result<MethodReply<'s, file::sync_response>, FailInvocation> {
        self.service.sync(true).map_err(fail)?;
        Ok(MethodReply::new(file::sync_response {}))
    }
    fn truncate<'s>(
        &'s mut self,
        r: file::truncate_request,
    ) -> Result<MethodReply<'s, file::truncate_response>, FailInvocation> {
        self.writable()?;
        let description = self.description()?;
        self.service
            .truncate_to(&description.path, r.length)
            .map_err(fail)?;
        Ok(MethodReply::new(file::truncate_response {}))
    }
    fn allocate<'s>(
        &'s mut self,
        _: file::allocate_request,
    ) -> Result<MethodReply<'s, file::allocate_response>, FailInvocation> {
        Err(fail(FsError::new(Errno::EOpNotSupp)))
    }
    fn get_flags<'s>(
        &'s mut self,
        _: file::get_flags_request,
    ) -> Result<MethodReply<'s, file::get_flags_response>, FailInvocation> {
        let description = self.description()?;
        Ok(MethodReply::new(file::get_flags_response {
            flags: description.flags,
        }))
    }
    fn set_flags<'s>(
        &'s mut self,
        r: file::set_flags_request,
    ) -> Result<MethodReply<'s, file::set_flags_response>, FailInvocation> {
        if !self.service.set_flags(self.state.description, r.flags) {
            return Err(fail(FsError::new(Errno::EBadf)));
        }
        Ok(MethodReply::new(file::set_flags_response {}))
    }
    fn device_control<'s>(
        &'s mut self,
        _: file::device_control_request,
    ) -> Result<MethodReply<'s, file::device_control_response>, FailInvocation> {
        Err(fail(FsError::new(Errno::EOpNotSupp)))
    }
    fn read<'s>(
        &'s mut self,
        r: file::read_request,
    ) -> Result<MethodReply<'s, file::read_response>, FailInvocation> {
        self.readable()?;
        let bytes = transfer_bytes(r.size)?;
        if bytes == 0 {
            return Ok(MethodReply::new(file::read_response { count: 0 }));
        }
        let handle = region_handle(self.region)?;
        let offset = self.description()?.offset;
        let count = servicekit::memory::with_region_write(
            handle,
            0,
            bytes,
            |out| self.read_at(offset, out),
        )
        .map_err(|_| fail(FsError::new(Errno::EInval)))??;
        if !self
            .service
            .set_offset(self.state.description, offset + count as i64)
        {
            return Err(fail(FsError::new(Errno::EBadf)));
        }
        Ok(MethodReply::new(file::read_response {
            count: count as u64,
        }))
    }
    fn write<'s>(
        &'s mut self,
        r: file::write_request,
    ) -> Result<MethodReply<'s, file::write_response>, FailInvocation> {
        self.writable()?;
        let bytes = transfer_bytes(r.size)?;
        if bytes == 0 {
            return Ok(MethodReply::new(file::write_response { count: 0 }));
        }
        let handle = region_handle(self.region)?;
        let description = self.description()?;
        let o = if description.flags & OPEN_APPEND != 0 {
            self.service.lookup(&description.path).map_err(fail)?.size as i64
        } else {
            description.offset
        };
        let count = servicekit::memory::with_region_read(
            handle,
            0,
            bytes,
            |data| self.service.write_at(&description.path, o as u64, data),
        )
        .map_err(|_| fail(FsError::new(Errno::EInval)))?
        .map_err(fail)?;
        if !self
            .service
            .set_offset(self.state.description, o + count as i64)
        {
            return Err(fail(FsError::new(Errno::EBadf)));
        }
        Ok(MethodReply::new(file::write_response {
            count: count as u64,
        }))
    }
    fn preadv<'s>(
        &'s mut self,
        r: file::preadv_request,
    ) -> Result<MethodReply<'s, file::preadv_response>, FailInvocation> {
        self.readable()?;
        let bytes = transfer_bytes(r.size)?;
        let segment_count = iov_segment_count(&r.layout, r.size)?;
        if r.offset < 0 {
            return Err(fail(FsError::new(Errno::EInval)));
        }
        if bytes == 0 {
            return Ok(MethodReply::new(file::preadv_response { count: 0 }));
        }
        let handle = region_handle(self.region)?;
        let path = self.description()?.path;
        let count = servicekit::memory::with_region_write(
            handle,
            0,
            bytes,
            |out| {
                let mut copied = 0usize;
                let mut offset = r.offset as u64;
                for length in r.layout.lengths[..segment_count].iter().copied() {
                    let length = usize::try_from(length)
                        .map_err(|_| fail(FsError::new(Errno::EInval)))?;
                    let end = copied
                        .checked_add(length)
                        .ok_or_else(|| fail(FsError::new(Errno::EInval)))?;
                    let read = self
                        .service
                        .read_at(&path, offset, &mut out[copied..end])
                        .map_err(fail)?;
                    copied += read;
                    offset = offset
                        .checked_add(read as u64)
                        .ok_or_else(|| fail(FsError::new(Errno::EInval)))?;
                    if read != length {
                        break;
                    }
                }
                Ok(copied)
            },
        )
        .map_err(|_| fail(FsError::new(Errno::EInval)))??;
        Ok(MethodReply::new(file::preadv_response {
            count: count as u64,
        }))
    }
    fn pwritev<'s>(
        &'s mut self,
        r: file::pwritev_request,
    ) -> Result<MethodReply<'s, file::pwritev_response>, FailInvocation> {
        self.writable()?;
        let bytes = transfer_bytes(r.size)?;
        let segment_count = iov_segment_count(&r.layout, r.size)?;
        if r.offset < 0 {
            return Err(fail(FsError::new(Errno::EInval)));
        }
        if bytes == 0 {
            return Ok(MethodReply::new(file::pwritev_response { count: 0 }));
        }
        let handle = region_handle(self.region)?;
        let path = self.description()?.path;
        let count = servicekit::memory::with_region_read(
            handle,
            0,
            bytes,
            |data| {
                let mut copied = 0usize;
                let mut offset = r.offset as u64;
                for length in r.layout.lengths[..segment_count].iter().copied() {
                    let length = usize::try_from(length)
                        .map_err(|_| fail(FsError::new(Errno::EInval)))?;
                    let end = copied
                        .checked_add(length)
                        .ok_or_else(|| fail(FsError::new(Errno::EInval)))?;
                    let written = self
                        .service
                        .write_at(&path, offset, &data[copied..end])
                        .map_err(fail)?;
                    copied += written;
                    offset = offset
                        .checked_add(written as u64)
                        .ok_or_else(|| fail(FsError::new(Errno::EInval)))?;
                    if written != length {
                        break;
                    }
                }
                Ok(copied)
            },
        )
        .map_err(|_| fail(FsError::new(Errno::EInval)))??;
        Ok(MethodReply::new(file::pwritev_response {
            count: count as u64,
        }))
    }
    fn readv<'s>(
        &'s mut self,
        r: file::readv_request,
    ) -> Result<MethodReply<'s, file::readv_response>, FailInvocation> {
        self.readable()?;
        let bytes = transfer_bytes(r.size)?;
        let segment_count = iov_segment_count(&r.layout, r.size)?;
        if bytes == 0 {
            return Ok(MethodReply::new(file::readv_response { count: 0 }));
        }
        let handle = region_handle(self.region)?;
        let description = self.description()?;
        let path = description.path;
        let offset = description.offset;
        if offset < 0 {
            return Err(fail(FsError::new(Errno::EInval)));
        }
        let count = servicekit::memory::with_region_write(
            handle,
            0,
            bytes,
            |out| {
                let mut copied = 0usize;
                let mut position = offset as u64;
                for length in r.layout.lengths[..segment_count].iter().copied() {
                    let length = usize::try_from(length)
                        .map_err(|_| fail(FsError::new(Errno::EInval)))?;
                    let end = copied
                        .checked_add(length)
                        .ok_or_else(|| fail(FsError::new(Errno::EInval)))?;
                    let read = self
                        .service
                        .read_at(&path, position, &mut out[copied..end])
                        .map_err(fail)?;
                    copied += read;
                    position = position
                        .checked_add(read as u64)
                        .ok_or_else(|| fail(FsError::new(Errno::EInval)))?;
                    if read != length {
                        break;
                    }
                }
                Ok(copied)
            },
        )
        .map_err(|_| fail(FsError::new(Errno::EInval)))??;
        let new_offset = offset
            .checked_add(count as i64)
            .ok_or_else(|| fail(FsError::new(Errno::EInval)))?;
        if !self.service.set_offset(self.state.description, new_offset) {
            return Err(fail(FsError::new(Errno::EBadf)));
        }
        Ok(MethodReply::new(file::readv_response {
            count: count as u64,
        }))
    }
    fn writev<'s>(
        &'s mut self,
        r: file::writev_request,
    ) -> Result<MethodReply<'s, file::writev_response>, FailInvocation> {
        self.writable()?;
        let bytes = transfer_bytes(r.size)?;
        let segment_count = iov_segment_count(&r.layout, r.size)?;
        if bytes == 0 {
            return Ok(MethodReply::new(file::writev_response { count: 0 }));
        }
        let handle = region_handle(self.region)?;
        let description = self.description()?;
        let path = description.path;
        let offset = if description.flags & OPEN_APPEND != 0 {
            self.service.lookup(&path).map_err(fail)?.size
        } else {
            u64::try_from(description.offset)
                .map_err(|_| fail(FsError::new(Errno::EInval)))?
        };
        let count = servicekit::memory::with_region_read(
            handle,
            0,
            bytes,
            |data| {
                let mut copied = 0usize;
                let mut position = offset;
                for length in r.layout.lengths[..segment_count].iter().copied() {
                    let length = usize::try_from(length)
                        .map_err(|_| fail(FsError::new(Errno::EInval)))?;
                    let end = copied
                        .checked_add(length)
                        .ok_or_else(|| fail(FsError::new(Errno::EInval)))?;
                    let written = self
                        .service
                        .write_at(&path, position, &data[copied..end])
                        .map_err(fail)?;
                    copied += written;
                    position = position
                        .checked_add(written as u64)
                        .ok_or_else(|| fail(FsError::new(Errno::EInval)))?;
                    if written != length {
                        break;
                    }
                }
                Ok(copied)
            },
        )
        .map_err(|_| fail(FsError::new(Errno::EInval)))??;
        let new_offset = offset
            .checked_add(count as u64)
            .and_then(|offset| i64::try_from(offset).ok())
            .ok_or_else(|| fail(FsError::new(Errno::EInval)))?;
        if !self.service.set_offset(self.state.description, new_offset) {
            return Err(fail(FsError::new(Errno::EBadf)));
        }
        Ok(MethodReply::new(file::writev_response {
            count: count as u64,
        }))
    }
    fn materialize<'s>(
        &'s mut self,
        _: file::materialize_request,
    ) -> Result<MethodReply<'s, file::materialize_response>, FailInvocation> {
        self.readable()?;
        let description = self.description()?;
        let metadata = self.service.lookup(&description.path).map_err(fail)?;
        if metadata.kind != NodeKind::File {
            return Err(fail(FsError::new(Errno::EIsDir)));
        }
        if metadata.size == 0 {
            return Err(fail(FsError::new(Errno::EInval)));
        }
        if metadata.size > sys::MEMORY_OBJECT_MAX_BYTES {
            return Err(fail(FsError::new(Errno::EFbig)));
        }
        let length = usize::try_from(metadata.size).map_err(|_| fail(FsError::new(Errno::EFbig)))?;
        let mut snapshot = vec![0u8; length];
        let count = self
            .service
            .read_at(&description.path, 0, &mut snapshot)
            .map_err(fail)?;
        if count != length {
            return Err(fail(FsError::new(Errno::EIo)));
        }
        let object = servicekit::memory::create_and_fill_read_only(&snapshot)
            .map_err(|_| fail(FsError::new(Errno::EIo)))?;
        let mut resources = ResourceTable::new();
        let slot = resources
            .push_move(object)
            .map_err(|_| fail(FsError::new(Errno::EIo)))?;
        let generation = *self.next_generation;
        *self.next_generation = self.next_generation.wrapping_add(1);
        Ok(MethodReply::with_resources(
            file::materialize_response {
                object: slot,
                length: metadata.size,
                generation,
            },
            resources,
        ))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn directory_paths_are_relative_to_binding() {
        assert_eq!(join("/", b"etc\0", false).unwrap(), "/etc");
        assert_eq!(join("/etc", b"motd", false).unwrap(), "/etc/motd");
        assert_eq!(join("/etc", b"/absolute", false).unwrap(), "/absolute");
        assert_eq!(join("/srv", b"/absolute", true).unwrap(), "/srv/absolute");
        assert!(join("/", &[0xff], false).is_err());
    }

    #[test]
    fn vectored_layout_is_bounded_by_transfer_size() {
        let layout = file::IOVLayout {
            segment_count: 2,
            lengths: [3, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0],
        };
        assert_eq!(iov_segment_count(&layout, 7).unwrap(), 2);
        assert!(iov_segment_count(&layout, 6).is_err());

        let too_many = file::IOVLayout {
            segment_count: 17,
            ..layout
        };
        assert!(iov_segment_count(&too_many, 7).is_err());
    }

    #[test]
    fn control_lookup_uses_final_component() {
        assert_eq!(last_component(b"/etc/motd\0"), b"motd");
        assert_eq!(last_component(b"/"), b"/");
    }

    #[test]
    fn stat_conversion_reports_fat_node_type_and_size() {
        let value = crate::worker::NodeStat {
            ino: 7,
            size: 512,
            kind: NodeKind::File,
        };
        let result = stat_value(value, 11);
        assert_eq!(result.device, 11);
        assert_eq!(result.inode, 7);
        assert_eq!(result.size, 512);
        assert_eq!(result.mode & 0o170000, 0o100000);
        assert_eq!(result.blocks, 1);
    }
}
