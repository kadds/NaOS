//! Linux host implementation of the public VFS listener.
//!
//! Host runs do not have NaOS protocol endpoint handles or boot modules.  The
//! same bounded RAM namespace is nevertheless useful for process-boundary
//! smoke tests, so this adapter exposes it through servicekit's UDS server.
//! It intentionally contains no mount authority: the native boot path remains
//! in `service.rs` and owns the VFS admin listener.

use std::collections::BTreeMap;
use std::vec::Vec;

use naos_idl::{directory, file, ResourceSlot};
use naos_idl::transport::RpcRequestOwned;
use servicekit::server::{Request, Response, ServeHandler, Server};
use servicekit::{Context, sys};

use vfsd::backend::{FileKind, NodeId, RamFs};
use vfsd::errno::Errno;

const MAX_TRANSFER: u64 = 65_536;
const CREATE_DIRECTORY: u64 = 1;
const OPEN_CREATE: u64 = 1;
const OPEN_DIRECTORY: u64 = 16;
const OPEN_TRUNC: u64 = 256;
const OPEN_EXCL: u64 = 128;
const OPEN_APPEND: u64 = 8;
const OPEN_MODE_READ: u64 = 1;
const OPEN_MODE_WRITE: u64 = 2;
const LOOKUP_NOFOLLOW: u64 = 1;
const CHROOT: u64 = 1 << 63;

fn validate_directory_request(request: &RpcRequestOwned) -> Result<(), i64> {
    macro_rules! check {
        ($decode:ident, $validate:ident) => {{
            let value = directory::$decode(&request.payload).map_err(|_| -22_i64)?;
            directory::$validate(&value, &request.resources).map_err(|_| -22_i64)
        }};
    }
    match request.method_id {
        directory::METHOD_OPEN => check!(decode_open_request, validate_open_request_transport_resources),
        directory::METHOD_LIST => check!(decode_list_request, validate_list_request_transport_resources),
        directory::METHOD_STAT => check!(decode_stat_request, validate_stat_request_transport_resources),
        directory::METHOD_CREATE => check!(decode_create_request, validate_create_request_transport_resources),
        directory::METHOD_REMOVE => check!(decode_remove_request, validate_remove_request_transport_resources),
        directory::METHOD_PATH => check!(decode_path_request, validate_path_request_transport_resources),
        directory::METHOD_ACCESS => check!(decode_access_request, validate_access_request_transport_resources),
        directory::METHOD_RENAME => check!(decode_rename_request, validate_rename_request_transport_resources),
        directory::METHOD_LINK => check!(decode_link_request, validate_link_request_transport_resources),
        directory::METHOD_SYMLINK => check!(decode_symlink_request, validate_symlink_request_transport_resources),
        directory::METHOD_READLINK => check!(decode_readlink_request, validate_readlink_request_transport_resources),
        directory::METHOD_SET_CURRENT => check!(decode_set_current_request, validate_set_current_request_transport_resources),
        directory::METHOD_SET_ROOT => check!(decode_set_root_request, validate_set_root_request_transport_resources),
        directory::METHOD_CLONE_BINDING => check!(decode_clone_binding_request, validate_clone_binding_request_transport_resources),
        directory::METHOD_STAT_NODE => check!(decode_stat_node_request, validate_stat_node_request_transport_resources),
        directory::METHOD_SYNC => check!(decode_sync_request, validate_sync_request_transport_resources),
        directory::METHOD_RENAME_AT => check!(decode_rename_at_request, validate_rename_at_request_transport_resources),
        directory::METHOD_LINK_AT => check!(decode_link_at_request, validate_link_at_request_transport_resources),
        _ => Err(-38),
    }
}

fn validate_file_request(request: &RpcRequestOwned) -> Result<(), i64> {
    macro_rules! check {
        ($decode:ident, $validate:ident) => {{
            let value = file::$decode(&request.payload).map_err(|_| -22_i64)?;
            file::$validate(&value, &request.resources).map_err(|_| -22_i64)
        }};
    }
    match request.method_id {
        file::METHOD_PREAD => check!(decode_pread_request, validate_pread_request_transport_resources),
        file::METHOD_PWRITE => check!(decode_pwrite_request, validate_pwrite_request_transport_resources),
        file::METHOD_SEEK => check!(decode_seek_request, validate_seek_request_transport_resources),
        file::METHOD_STAT => check!(decode_stat_request, validate_stat_request_transport_resources),
        file::METHOD_SYNC => check!(decode_sync_request, validate_sync_request_transport_resources),
        file::METHOD_TRUNCATE => check!(decode_truncate_request, validate_truncate_request_transport_resources),
        file::METHOD_ALLOCATE => check!(decode_allocate_request, validate_allocate_request_transport_resources),
        file::METHOD_GET_FLAGS => check!(decode_get_flags_request, validate_get_flags_request_transport_resources),
        file::METHOD_SET_FLAGS => check!(decode_set_flags_request, validate_set_flags_request_transport_resources),
        file::METHOD_DEVICE_CONTROL => check!(decode_device_control_request, validate_device_control_request_transport_resources),
        file::METHOD_READ => check!(decode_read_request, validate_read_request_transport_resources),
        file::METHOD_WRITE => check!(decode_write_request, validate_write_request_transport_resources),
        file::METHOD_PREADV => check!(decode_preadv_request, validate_preadv_request_transport_resources),
        file::METHOD_PWRITEV => check!(decode_pwritev_request, validate_pwritev_request_transport_resources),
        file::METHOD_READV => check!(decode_readv_request, validate_readv_request_transport_resources),
        file::METHOD_WRITEV => check!(decode_writev_request, validate_writev_request_transport_resources),
        file::METHOD_MATERIALIZE => check!(decode_materialize_request, validate_materialize_request_transport_resources),
        _ => Err(-38),
    }
}

fn encoded(encode: impl Fn(&mut [u8]) -> Result<usize, naos_idl::CodecError>) -> Vec<u8> {
    let mut wire = vec![0_u8; 64];
    loop {
        match encode(&mut wire) {
            Ok(size) => {
                wire.truncate(size);
                return wire;
            }
            Err(naos_idl::CodecError::Overflow) if wire.len() < 65_536 => {
                wire.resize((wire.len() * 2).min(65_536), 0);
            }
            Err(error) => panic!("generated host VFS response fits: {error:?}"),
        }
    }
}

fn failure(request: &Request, errno: Errno) -> Response {
    Response::error(request, -i64::from(errno.to_i32()))
}

fn protocol_failure(request: &Request, errno: i64) -> Response {
    Response::error(request, errno)
}

fn strip_nul(path: &[u8]) -> &[u8] {
    let mut end = path.len();
    while end > 0 && path[end - 1] == 0 {
        end -= 1;
    }
    &path[..end]
}

enum HostHandle {
    Directory { fd: u64, root: NodeId, current: NodeId },
    File { fd: u64, mode: u64 },
}

struct HostVfs {
    fs: RamFs,
    handles: BTreeMap<u64, HostHandle>,
}

impl HostVfs {
    fn new() -> Result<Self, Errno> {
        let mut fs = RamFs::new(64 << 20);
        fs.mkdir_p(fs.root(), b"/data")?;
        Ok(Self {
            fs,
            handles: BTreeMap::new(),
        })
    }

    fn reclaim(&mut self, server: &mut Server) {
        for resource in server.take_closed_resources() {
            if let Some(handle) = self.handles.remove(&resource.resource_id) {
                let fd = match handle {
                    HostHandle::Directory { fd, .. } | HostHandle::File { fd, .. } => fd,
                };
                let _ = self.fs.close(fd);
            }
        }
    }

    fn scope(&self, request: &Request) -> Result<(NodeId, NodeId), Errno> {
        let Some(resource) = request.target.or_else(|| request.resources.first().copied()) else {
            let root = self.fs.root();
            return Ok((root, root));
        };
        if !request.validate_resource(resource) {
            return Err(Errno::EAccess);
        }
        match self.handles.get(&resource.resource_id) {
            Some(HostHandle::Directory { root, current, .. }) => Ok((*root, *current)),
            _ => Err(Errno::EBadf),
        }
    }

    fn endpoint(
        &mut self,
        server: &mut Server,
        descriptor: &sys::ProtocolDescriptor,
        kind: FileKind,
        fd: u64,
        mode: u64,
        root: NodeId,
        current: NodeId,
    ) -> Result<servicekit::server::EndpointResource, Errno> {
        let (binding, scope) = if kind == FileKind::Directory {
            (sys::BINDING_CLIENT_END, directory::PROTOCOL_SCOPE)
        } else {
            (sys::BINDING_CLIENT_END, file::PROTOCOL_SCOPE)
        };
        let endpoint = server
            .create_endpoint(
                descriptor,
                binding,
                scope,
                u64::MAX,
                if kind == FileKind::Directory {
                    validate_directory_request
                } else {
                    validate_file_request
                },
            )
            .map_err(|_| Errno::ENomem)?;
        let id = endpoint.descriptor().resource_id;
        let handle = if kind == FileKind::Directory {
            HostHandle::Directory { fd, root, current }
        } else {
            HostHandle::File { fd, mode }
        };
        self.handles.insert(id, handle);
        Ok(endpoint)
    }

    fn stat(meta: vfsd::backend::Metadata) -> directory::Stat {
        let mode = match meta.kind {
            FileKind::Regular => 0o100000 | 0o644,
            FileKind::Directory => 0o040000 | 0o755,
            FileKind::Symlink => 0o120000 | 0o755,
        };
        directory::Stat {
            device: 1,
            inode: meta.node_id,
            links: meta.nlink,
            mode,
            uid: 0,
            gid: 0,
            padding: 0,
            device_id: 0,
            size: meta.size as i64,
            block_size: 4096,
            blocks: meta.size.div_ceil(512) as i64,
            access_seconds: 0,
            access_nanoseconds: 0,
            modify_seconds: 0,
            modify_nanoseconds: 0,
            change_seconds: 0,
            change_nanoseconds: 0,
            unused0: 0,
            unused1: 0,
            unused2: 0,
        }
    }

    fn stat_file(meta: vfsd::backend::Metadata) -> file::Stat {
        let value = Self::stat(meta);
        file::Stat {
            device: value.device,
            inode: value.inode,
            links: value.links,
            mode: value.mode,
            uid: value.uid,
            gid: value.gid,
            padding: value.padding,
            device_id: value.device_id,
            size: value.size,
            block_size: value.block_size,
            blocks: value.blocks,
            access_seconds: value.access_seconds,
            access_nanoseconds: value.access_nanoseconds,
            modify_seconds: value.modify_seconds,
            modify_nanoseconds: value.modify_nanoseconds,
            change_seconds: value.change_seconds,
            change_nanoseconds: value.change_nanoseconds,
            unused0: value.unused0,
            unused1: value.unused1,
            unused2: value.unused2,
        }
    }

    fn endpoint_id(request: &Request) -> Result<u64, Errno> {
        let resource = request
            .target
            .or_else(|| request.resources.first().copied())
            .ok_or(Errno::EBadf)?;
        if !request.validate_resource(resource) {
            return Err(Errno::EAccess);
        }
        Ok(resource.resource_id)
    }

    fn directory_request(
        &mut self,
        server: &mut Server,
        request: &Request,
    ) -> Response {
        if request.method_id == directory::METHOD_LIST
            || !request.bulk.is_empty()
            || (request.target.is_none() && !request.resources.is_empty())
        {
            return protocol_failure(request, -22);
        }
        let (root, current) = match self.scope(request) {
            Ok(scope) => scope,
            Err(errno) => return failure(request, errno),
        };
        match request.method_id {
            directory::METHOD_OPEN => {
                let value = match directory::decode_open_request(&request.payload) {
                    Ok(value) => value,
                    Err(_) => return protocol_failure(request, -22),
                };
                let path = strip_nul(value.path);
                let node = match self.fs.lookup_scoped(root, current, path, true) {
                    Ok(node) => node,
                    Err(Errno::ENoent) if value.flags & OPEN_CREATE != 0 => {
                        if value.flags & OPEN_DIRECTORY != 0 {
                            return failure(request, Errno::ENoent);
                        }
                        match self.fs.create_scoped(
                            root,
                            current,
                            path,
                            value.mode & OPEN_EXCL != 0,
                        ) {
                            Ok(meta) => meta.node_id,
                            Err(errno) => return failure(request, errno),
                        }
                    }
                    Err(errno) => return failure(request, errno),
                };
                let meta = match self.fs.metadata_of(node) {
                    Ok(meta) => meta,
                    Err(errno) => return failure(request, errno),
                };
                if meta.kind == FileKind::Directory && value.flags & OPEN_TRUNC != 0 {
                    return failure(request, Errno::EIsDir);
                }
                if meta.kind == FileKind::Regular && value.flags & OPEN_TRUNC != 0 {
                    if let Err(errno) = self.fs.truncate_path(current, path, 0) {
                        return failure(request, errno);
                    }
                }
                let fd = match self.fs.open_node(node) {
                    Ok(fd) => fd,
                    Err(errno) => return failure(request, errno),
                };
                let endpoint = if meta.kind == FileKind::Directory {
                    self.endpoint(
                        server,
                        &directory::protocol_descriptor(),
                        meta.kind,
                        fd,
                        value.mode,
                        if value.flags & CHROOT != 0 { node } else { root },
                        node,
                    )
                } else {
                    self.endpoint(
                        server,
                        &file::protocol_descriptor(),
                        meta.kind,
                        fd,
                        value.mode,
                        root,
                        current,
                    )
                };
                let endpoint = match endpoint {
                    Ok(endpoint) => endpoint,
                    Err(errno) => {
                        let _ = self.fs.close(fd);
                        return failure(request, errno);
                    }
                };
                let response = directory::open_response {
                    object: ResourceSlot::new(0).expect("slot zero"),
                };
                let mut response = Response::success(
                    request,
                    encoded(|wire| directory::encode_open_response(&response, wire)),
                );
                response.push_resource(endpoint);
                response
            }
            directory::METHOD_CREATE => {
                let value = match directory::decode_create_request(&request.payload) {
                    Ok(value) => value,
                    Err(_) => return protocol_failure(request, -22),
                };
                let path = strip_nul(value.path);
                let result = if value.flags & CREATE_DIRECTORY != 0 {
                    self.fs.mkdir_scoped(root, current, path).map(|_| ())
                } else {
                    self.fs
                        .create_scoped(root, current, path, true)
                        .map(|_| ())
                };
                result.map_or_else(|errno| failure(request, errno), |_| Response::success(request, Vec::new()))
            }
            directory::METHOD_REMOVE => {
                let value = match directory::decode_remove_request(&request.payload) {
                    Ok(value) => value,
                    Err(_) => return protocol_failure(request, -22),
                };
                let path = strip_nul(value.path);
                if value.flags & CREATE_DIRECTORY == 0 {
                    let target = match self.fs.lookup_scoped(root, current, path, false) {
                        Ok(target) => target,
                        Err(errno) => return failure(request, errno),
                    };
                    let open = self.handles.values().any(|handle| {
                        let HostHandle::File { fd, .. } = handle else {
                            return false;
                        };
                        self.fs.fstat(*fd).is_ok_and(|meta| meta.node_id == target)
                    });
                    if open {
                        return failure(request, Errno::EBusy);
                    }
                }
                let result = if value.flags & CREATE_DIRECTORY != 0 {
                    self.fs.rmdir_scoped(root, current, path)
                } else {
                    self.fs.unlink_scoped(root, current, path)
                };
                result.map_or_else(|errno| failure(request, errno), |_| Response::success(request, Vec::new()))
            }
            directory::METHOD_STAT => self.fs.metadata_of(current).map_or_else(
                |errno| failure(request, errno),
                |meta| Response::success(request, encoded(|wire| {
                    directory::encode_stat_response(&directory::stat_response { value: Self::stat(meta) }, wire)
                })),
            ),
            directory::METHOD_STAT_NODE => {
                let value = match directory::decode_stat_node_request(&request.payload) {
                    Ok(value) => value,
                    Err(_) => return protocol_failure(request, -22),
                };
                if value.flags & !LOOKUP_NOFOLLOW != 0 {
                    return failure(request, Errno::EInval);
                }
                self.fs
                    .lookup_scoped(root, current, strip_nul(value.path), value.flags & LOOKUP_NOFOLLOW == 0)
                    .map_or_else(
                        |errno| failure(request, errno),
                        |node| self.fs.metadata_of(node).map_or_else(
                            |errno| failure(request, errno),
                            |meta| Response::success(request, encoded(|wire| {
                                directory::encode_stat_node_response(
                                    &directory::stat_node_response { value: Self::stat(meta) },
                                    wire,
                                )
                            })),
                        ),
                    )
            }
            directory::METHOD_ACCESS => Response::success(request, Vec::new()),
            directory::METHOD_RENAME => {
                let value = match directory::decode_rename_request(&request.payload) {
                    Ok(value) => value,
                    Err(_) => return protocol_failure(request, -22),
                };
                self.fs
                    .rename_scoped(root, current, strip_nul(value.first), root, current, strip_nul(value.second))
                    .map_or_else(|errno| failure(request, errno), |_| Response::success(request, Vec::new()))
            }
            directory::METHOD_SYNC => Response::success(request, Vec::new()),
            directory::METHOD_CLONE_BINDING => {
                let fd = match self.fs.open_node(current) {
                    Ok(fd) => fd,
                    Err(errno) => return failure(request, errno),
                };
                let endpoint = match self.endpoint(
                    server,
                    &directory::protocol_descriptor(),
                    FileKind::Directory,
                    fd,
                    0,
                    root,
                    current,
                ) {
                    Ok(endpoint) => endpoint,
                    Err(errno) => {
                        let _ = self.fs.close(fd);
                        return failure(request, errno);
                    }
                };
                let value = directory::clone_binding_response {
                    directory: ResourceSlot::new(0).expect("slot zero"),
                };
                let mut response = Response::success(
                    request,
                    encoded(|wire| directory::encode_clone_binding_response(&value, wire)),
                );
                response.push_resource(endpoint);
                response
            }
            _ => protocol_failure(request, -38),
        }
    }

    fn file_request(&mut self, request: &Request) -> Response {
        let id = match Self::endpoint_id(request) {
            Ok(id) => id,
            Err(errno) => return failure(request, errno),
        };
        let (fd, mut mode) = match self.handles.get(&id) {
            Some(HostHandle::File { fd, mode }) => (*fd, *mode),
            _ => return failure(request, Errno::EBadf),
        };
        match request.method_id {
            file::METHOD_PREAD => {
                if mode & OPEN_MODE_READ == 0 {
                    return failure(request, Errno::EBadf);
                }
                let value = match file::decode_pread_request(&request.payload) {
                    Ok(value) => value,
                    Err(_) => return protocol_failure(request, -22),
                };
                if value.offset < 0 {
                    return protocol_failure(request, -22);
                }
                Self::read_region(request, value.buffer, 0, value.size, |data| {
                    self.fs.pread(fd, value.offset.max(0) as u64, data)
                })
                .map_or_else(|error| protocol_failure(request, error), |count| Response::success(request, encoded(|wire| {
                    file::encode_pread_response(&file::pread_response { count: count as u64 }, wire)
                })))
            }
            file::METHOD_READ => {
                if mode & OPEN_MODE_READ == 0 {
                    return failure(request, Errno::EBadf);
                }
                let value = match file::decode_read_request(&request.payload) {
                    Ok(value) => value,
                    Err(_) => return protocol_failure(request, -22),
                };
                Self::read_region(request, value.buffer, 0, value.size, |data| self.fs.read(fd, data))
                    .map_or_else(|error| protocol_failure(request, error), |count| Response::success(request, encoded(|wire| {
                        file::encode_read_response(&file::read_response { count: count as u64 }, wire)
                    })))
            }
            file::METHOD_PWRITE => {
                if mode & OPEN_MODE_WRITE == 0 {
                    return failure(request, Errno::EBadf);
                }
                let value = match file::decode_pwrite_request(&request.payload) {
                    Ok(value) => value,
                    Err(_) => return protocol_failure(request, -22),
                };
                if value.offset < 0 {
                    return protocol_failure(request, -22);
                }
                Self::write_region(request, value.buffer, 0, value.size, |data| {
                    self.fs.pwrite(fd, value.offset.max(0) as u64, data)
                })
                .map_or_else(|error| protocol_failure(request, error), |count| Response::success(request, encoded(|wire| {
                    file::encode_pwrite_response(&file::pwrite_response { count: count as u64 }, wire)
                })))
            }
            file::METHOD_WRITE => {
                if mode & OPEN_MODE_WRITE == 0 {
                    return failure(request, Errno::EBadf);
                }
                let value = match file::decode_write_request(&request.payload) {
                    Ok(value) => value,
                    Err(_) => return protocol_failure(request, -22),
                };
                if mode & OPEN_APPEND != 0 {
                    if let Err(errno) = self.fs.seek(fd, vfsd::backend::SeekFrom::End(0)) {
                        return failure(request, errno);
                    }
                }
                Self::write_region(request, value.buffer, 0, value.size, |data| self.fs.write(fd, data))
                    .map_or_else(|error| protocol_failure(request, error), |count| Response::success(request, encoded(|wire| {
                        file::encode_write_response(&file::write_response { count: count as u64 }, wire)
                    })))
            }
            file::METHOD_STAT => self.fs.fstat(fd).map_or_else(
                |errno| failure(request, errno),
                |meta| Response::success(request, encoded(|wire| {
                    file::encode_stat_response(&file::stat_response { value: Self::stat_file(meta) }, wire)
                })),
            ),
            file::METHOD_SYNC => Response::success(request, Vec::new()),
            file::METHOD_TRUNCATE => {
                if mode & OPEN_MODE_WRITE == 0 {
                    return failure(request, Errno::EBadf);
                }
                let value = match file::decode_truncate_request(&request.payload) {
                    Ok(value) => value,
                    Err(_) => return protocol_failure(request, -22),
                };
                self.fs.ftruncate(fd, value.length).map_or_else(
                    |errno| failure(request, errno),
                    |_| Response::success(request, Vec::new()),
                )
            }
            file::METHOD_GET_FLAGS => Response::success(request, encoded(|wire| {
                file::encode_get_flags_response(&file::get_flags_response { flags: mode }, wire)
            })),
            file::METHOD_SET_FLAGS => {
                let value = match file::decode_set_flags_request(&request.payload) {
                    Ok(value) => value,
                    Err(_) => return protocol_failure(request, -22),
                };
                mode = value.flags;
                if let Some(HostHandle::File { mode: stored, .. }) = self.handles.get_mut(&id) {
                    *stored = mode;
                }
                Response::success(request, Vec::new())
            }
            _ => protocol_failure(request, -38),
        }
    }

    fn read_region(
        request: &Request,
        slot: ResourceSlot,
        offset: u64,
        size: u64,
        read: impl FnOnce(&mut [u8]) -> Result<usize, Errno>,
    ) -> Result<usize, i64> {
        if size > MAX_TRANSFER {
            return Err(-22);
        }
        let size = usize::try_from(size).map_err(|_| -22_i64)?;
        if size == 0 {
            return Ok(0);
        }
        request
            .with_write_memory(slot, offset, size, |data| {
                read(data).map_err(|error| -i64::from(error.to_i32()))
            })
            .map_err(|_| -22_i64)?
            .map_err(|error| error)
    }

    fn write_region(
        request: &Request,
        slot: ResourceSlot,
        offset: u64,
        size: u64,
        write: impl FnOnce(&[u8]) -> Result<usize, Errno>,
    ) -> Result<usize, i64> {
        if size > MAX_TRANSFER {
            return Err(-22);
        }
        let size = usize::try_from(size).map_err(|_| -22_i64)?;
        if size == 0 {
            return Ok(0);
        }
        request
            .with_read_memory(slot, offset, size, |data| {
                write(data).map_err(|error| -i64::from(error.to_i32()))
            })
            .map_err(|_| -22_i64)?
            .map_err(|error| error)
    }
}

struct Handler {
    state: HostVfs,
}

impl ServeHandler for Handler {
    async fn handle(&mut self, server: &mut Server, request: Request) {
        self.state.reclaim(server);
        let response = if request.protocol_uuid == directory::PROTOCOL_UUID {
            self.state.directory_request(server, &request)
        } else if request.protocol_uuid == file::PROTOCOL_UUID {
            self.state.file_request(&request)
        } else {
            protocol_failure(&request, -71)
        };
        if let Err(error) = request.respond(response).await {
            log::debug!("host vfs response failed: {error:?}");
        }
    }
}

pub async fn run(context: Context) -> i64 {
    let directory = context.service_directory();
    let mut server = match Server::publish(
        &directory,
        servicekit::uri::FS_VFS,
        &directory::protocol_descriptor(),
        validate_directory_request,
    )
    .await
    {
        Ok(server) => server,
        Err(error) => {
            log::error!("publish host vfs service failed: {error:?}");
            return 1;
        }
    };
    let state = match HostVfs::new() {
        Ok(state) => state,
        Err(errno) => {
            log::error!("initialize host vfs failed errno={errno:?}");
            return 1;
        }
    };
    log::info!("ready service={}", servicekit::uri::FS_VFS);
    server.serve(&mut Handler { state }).await as i64
}
