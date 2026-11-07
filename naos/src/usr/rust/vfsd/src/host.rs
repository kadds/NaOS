//! Linux host implementation of the public VFS listener.
//!
//! Host runs do not have NaOS protocol endpoint handles or boot modules.  The
//! same bounded RAM namespace is nevertheless useful for process-boundary
//! smoke tests, so this adapter exposes it through servicekit's UDS server.
//! It intentionally contains no mount authority: the native boot path remains
//! in `service.rs` and owns the VFS admin listener.

use std::collections::BTreeMap;

use naos_idl::transport::{RpcMethodReply, RpcOutcome};
use naos_idl::{ResourceSlot, directory, file};
use servicekit::server::{EndpointResource, Request, ServeHandler, Server};
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

fn strip_nul(path: &[u8]) -> &[u8] {
    let mut end = path.len();
    while end > 0 && path[end - 1] == 0 {
        end -= 1;
    }
    &path[..end]
}

enum HostHandle {
    Directory {
        fd: u64,
        root: NodeId,
        current: NodeId,
    },
    File {
        fd: u64,
        mode: u64,
    },
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
        let Some(resource) = request
            .target
            .or_else(|| request.resources.first().copied())
        else {
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
                    directory::validate_request_transport_resources
                } else {
                    file::validate_request_transport_resources
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

    fn descriptor(&self, request: &Request) -> Result<(u64, u64), Errno> {
        let id = Self::endpoint_id(request)?;
        match self.handles.get(&id) {
            Some(HostHandle::File { fd, mode }) => Ok((*fd, *mode)),
            _ => Err(Errno::EBadf),
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

struct HostRequestContext<'a> {
    request: &'a Request,
    server: &'a mut Server,
}

fn rpc_failure(error: i64) -> RpcOutcome {
    RpcOutcome {
        execution: 0,
        reason: 0,
        protocol_error: error,
    }
}

fn errno_failure(errno: Errno) -> RpcOutcome {
    rpc_failure(-i64::from(errno.to_i32()))
}

macro_rules! unsupported_directory_method {
    ($name:ident, $request:ty, $response:ty) => {
        async fn $name<'s>(
            &'s mut self,
            context: &mut HostRequestContext<'_>,
            _request: $request,
        ) -> Result<RpcMethodReply<$response, Self::Resource>, RpcOutcome> {
            if !context.request.bulk.is_empty() {
                return Err(rpc_failure(-22));
            }
            Err(rpc_failure(-38))
        }
    };
}

impl<'ctx> directory::DirectoryServiceKitHandler<HostRequestContext<'ctx>> for Handler {
    type Resource = EndpointResource;

    async fn open<'s>(
        &'s mut self,
        context: &mut HostRequestContext<'ctx>,
        value: directory::open_request<'_>,
    ) -> Result<RpcMethodReply<directory::open_response, Self::Resource>, RpcOutcome> {
        let request = context.request;
        if !request.bulk.is_empty() {
            return Err(rpc_failure(-22));
        }
        let (root, current) = self.state.scope(request).map_err(errno_failure)?;
        let path = strip_nul(value.path);
        let node = match self.state.fs.lookup_scoped(root, current, path, true) {
            Ok(node) => node,
            Err(Errno::ENoent) if value.flags & OPEN_CREATE != 0 => {
                if value.flags & OPEN_DIRECTORY != 0 {
                    return Err(errno_failure(Errno::ENoent));
                }
                self.state
                    .fs
                    .create_scoped(root, current, path, value.mode & OPEN_EXCL != 0)
                    .map_err(errno_failure)?
                    .node_id
            }
            Err(error) => return Err(errno_failure(error)),
        };
        let meta = self.state.fs.metadata_of(node).map_err(errno_failure)?;
        if meta.kind == FileKind::Directory && value.flags & OPEN_TRUNC != 0 {
            return Err(errno_failure(Errno::EIsDir));
        }
        if meta.kind == FileKind::Regular && value.flags & OPEN_TRUNC != 0 {
            self.state
                .fs
                .truncate_path(current, path, 0)
                .map_err(errno_failure)?;
        }
        let fd = self.state.fs.open_node(node).map_err(errno_failure)?;
        let endpoint = if meta.kind == FileKind::Directory {
            self.state.endpoint(
                context.server,
                &directory::protocol_descriptor(),
                meta.kind,
                fd,
                value.mode,
                if value.flags & CHROOT != 0 {
                    node
                } else {
                    root
                },
                node,
            )
        } else {
            self.state.endpoint(
                context.server,
                &file::protocol_descriptor(),
                meta.kind,
                fd,
                value.mode,
                root,
                current,
            )
        }
        .map_err(|error| {
            let _ = self.state.fs.close(fd);
            errno_failure(error)
        })?;
        let mut reply = RpcMethodReply::new(directory::open_response {
            object: ResourceSlot::new(0).expect("resource slot zero"),
        });
        reply.resources.push(endpoint);
        Ok(reply)
    }

    async fn list<'s>(
        &'s mut self,
        context: &mut HostRequestContext<'ctx>,
        _request: directory::list_request,
    ) -> Result<RpcMethodReply<directory::list_response, Self::Resource>, RpcOutcome> {
        if !context.request.bulk.is_empty() {
            return Err(rpc_failure(-22));
        }
        Err(rpc_failure(-22))
    }

    async fn create<'s>(
        &'s mut self,
        context: &mut HostRequestContext<'ctx>,
        value: directory::create_request<'_>,
    ) -> Result<RpcMethodReply<directory::create_response, Self::Resource>, RpcOutcome> {
        if !context.request.bulk.is_empty() {
            return Err(rpc_failure(-22));
        }
        let (root, current) = self.state.scope(context.request).map_err(errno_failure)?;
        let path = strip_nul(value.path);
        if value.flags & CREATE_DIRECTORY != 0 {
            self.state
                .fs
                .mkdir_scoped(root, current, path)
                .map_err(errno_failure)?;
        } else {
            self.state
                .fs
                .create_scoped(root, current, path, true)
                .map_err(errno_failure)?;
        }
        Ok(RpcMethodReply::new(directory::create_response {}))
    }

    async fn remove<'s>(
        &'s mut self,
        context: &mut HostRequestContext<'ctx>,
        value: directory::remove_request<'_>,
    ) -> Result<RpcMethodReply<directory::remove_response, Self::Resource>, RpcOutcome> {
        if !context.request.bulk.is_empty() {
            return Err(rpc_failure(-22));
        }
        let (root, current) = self.state.scope(context.request).map_err(errno_failure)?;
        let path = strip_nul(value.path);
        if value.flags & CREATE_DIRECTORY == 0 {
            let target = self
                .state
                .fs
                .lookup_scoped(root, current, path, false)
                .map_err(errno_failure)?;
            let is_open = self.state.handles.values().any(|handle| {
                let HostHandle::File { fd, .. } = handle else {
                    return false;
                };
                self.state
                    .fs
                    .fstat(*fd)
                    .is_ok_and(|meta| meta.node_id == target)
            });
            if is_open {
                return Err(errno_failure(Errno::EBusy));
            }
        }
        if value.flags & CREATE_DIRECTORY != 0 {
            self.state
                .fs
                .rmdir_scoped(root, current, path)
                .map_err(errno_failure)?;
        } else {
            self.state
                .fs
                .unlink_scoped(root, current, path)
                .map_err(errno_failure)?;
        }
        Ok(RpcMethodReply::new(directory::remove_response {}))
    }

    async fn stat<'s>(
        &'s mut self,
        context: &mut HostRequestContext<'ctx>,
        _request: directory::stat_request,
    ) -> Result<RpcMethodReply<directory::stat_response, Self::Resource>, RpcOutcome> {
        if !context.request.bulk.is_empty() {
            return Err(rpc_failure(-22));
        }
        let (_, current) = self.state.scope(context.request).map_err(errno_failure)?;
        let meta = self.state.fs.metadata_of(current).map_err(errno_failure)?;
        Ok(RpcMethodReply::new(directory::stat_response {
            value: HostVfs::stat(meta),
        }))
    }

    async fn stat_node<'s>(
        &'s mut self,
        context: &mut HostRequestContext<'ctx>,
        value: directory::stat_node_request<'_>,
    ) -> Result<RpcMethodReply<directory::stat_node_response, Self::Resource>, RpcOutcome> {
        if !context.request.bulk.is_empty() {
            return Err(rpc_failure(-22));
        }
        let (root, current) = self.state.scope(context.request).map_err(errno_failure)?;
        if value.flags & !LOOKUP_NOFOLLOW != 0 {
            return Err(errno_failure(Errno::EInval));
        }
        let node = self
            .state
            .fs
            .lookup_scoped(
                root,
                current,
                strip_nul(value.path),
                value.flags & LOOKUP_NOFOLLOW == 0,
            )
            .map_err(errno_failure)?;
        let meta = self.state.fs.metadata_of(node).map_err(errno_failure)?;
        Ok(RpcMethodReply::new(directory::stat_node_response {
            value: HostVfs::stat(meta),
        }))
    }

    async fn access<'s>(
        &'s mut self,
        context: &mut HostRequestContext<'ctx>,
        _value: directory::access_request<'_>,
    ) -> Result<RpcMethodReply<directory::access_response, Self::Resource>, RpcOutcome> {
        if !context.request.bulk.is_empty() {
            return Err(rpc_failure(-22));
        }
        self.state.scope(context.request).map_err(errno_failure)?;
        Ok(RpcMethodReply::new(directory::access_response {}))
    }

    async fn rename<'s>(
        &'s mut self,
        context: &mut HostRequestContext<'ctx>,
        value: directory::rename_request<'_>,
    ) -> Result<RpcMethodReply<directory::rename_response, Self::Resource>, RpcOutcome> {
        if !context.request.bulk.is_empty() {
            return Err(rpc_failure(-22));
        }
        let (root, current) = self.state.scope(context.request).map_err(errno_failure)?;
        self.state
            .fs
            .rename_scoped(
                root,
                current,
                strip_nul(value.first),
                root,
                current,
                strip_nul(value.second),
            )
            .map_err(errno_failure)?;
        Ok(RpcMethodReply::new(directory::rename_response {}))
    }

    async fn sync<'s>(
        &'s mut self,
        context: &mut HostRequestContext<'ctx>,
        _request: directory::sync_request,
    ) -> Result<RpcMethodReply<directory::sync_response, Self::Resource>, RpcOutcome> {
        if !context.request.bulk.is_empty() {
            return Err(rpc_failure(-22));
        }
        self.state.scope(context.request).map_err(errno_failure)?;
        Ok(RpcMethodReply::new(directory::sync_response {}))
    }

    async fn clone_binding<'s>(
        &'s mut self,
        context: &mut HostRequestContext<'ctx>,
        _request: directory::clone_binding_request,
    ) -> Result<RpcMethodReply<directory::clone_binding_response, Self::Resource>, RpcOutcome> {
        if !context.request.bulk.is_empty() {
            return Err(rpc_failure(-22));
        }
        let (root, current) = self.state.scope(context.request).map_err(errno_failure)?;
        let fd = self.state.fs.open_node(current).map_err(errno_failure)?;
        let endpoint = self
            .state
            .endpoint(
                context.server,
                &directory::protocol_descriptor(),
                FileKind::Directory,
                fd,
                0,
                root,
                current,
            )
            .map_err(|error| {
                let _ = self.state.fs.close(fd);
                errno_failure(error)
            })?;
        let mut reply = RpcMethodReply::new(directory::clone_binding_response {
            directory: ResourceSlot::new(0).expect("resource slot zero"),
        });
        reply.resources.push(endpoint);
        Ok(reply)
    }

    unsupported_directory_method!(path, directory::path_request, directory::path_response<'s>);
    unsupported_directory_method!(link, directory::link_request<'_>, directory::link_response);
    unsupported_directory_method!(
        symlink,
        directory::symlink_request<'_>,
        directory::symlink_response
    );
    unsupported_directory_method!(
        readlink,
        directory::readlink_request<'_>,
        directory::readlink_response<'s>
    );
    unsupported_directory_method!(
        set_current,
        directory::set_current_request,
        directory::set_current_response
    );
    unsupported_directory_method!(
        set_root,
        directory::set_root_request,
        directory::set_root_response
    );
    unsupported_directory_method!(
        rename_at,
        directory::rename_at_request<'_>,
        directory::rename_at_response
    );
    unsupported_directory_method!(
        link_at,
        directory::link_at_request<'_>,
        directory::link_at_response
    );
}

macro_rules! unsupported_file_method {
    ($name:ident, $request:ty, $response:ty) => {
        async fn $name<'s>(
            &'s mut self,
            context: &mut HostRequestContext<'_>,
            _request: $request,
        ) -> Result<RpcMethodReply<$response, Self::Resource>, RpcOutcome> {
            HostVfs::endpoint_id(context.request).map_err(errno_failure)?;
            Err(rpc_failure(-38))
        }
    };
}

impl<'ctx> file::FileServiceKitHandler<HostRequestContext<'ctx>> for Handler {
    type Resource = EndpointResource;

    async fn pread<'s>(
        &'s mut self,
        context: &mut HostRequestContext<'ctx>,
        value: file::pread_request,
    ) -> Result<RpcMethodReply<file::pread_response, Self::Resource>, RpcOutcome> {
        let request = context.request;
        let (fd, mode) = self.state.descriptor(request).map_err(errno_failure)?;
        if mode & OPEN_MODE_READ == 0 {
            return Err(errno_failure(Errno::EBadf));
        }
        if value.offset < 0 {
            return Err(rpc_failure(-22));
        }
        let count = HostVfs::read_region(request, value.buffer, 0, value.size, |data| {
            self.state.fs.pread(fd, value.offset as u64, data)
        })
        .map_err(rpc_failure)?;
        Ok(RpcMethodReply::new(file::pread_response {
            count: count as u64,
        }))
    }

    async fn pwrite<'s>(
        &'s mut self,
        context: &mut HostRequestContext<'ctx>,
        value: file::pwrite_request,
    ) -> Result<RpcMethodReply<file::pwrite_response, Self::Resource>, RpcOutcome> {
        let request = context.request;
        let (fd, mode) = self.state.descriptor(request).map_err(errno_failure)?;
        if mode & OPEN_MODE_WRITE == 0 {
            return Err(errno_failure(Errno::EBadf));
        }
        if value.offset < 0 {
            return Err(rpc_failure(-22));
        }
        let count = HostVfs::write_region(request, value.buffer, 0, value.size, |data| {
            self.state.fs.pwrite(fd, value.offset as u64, data)
        })
        .map_err(rpc_failure)?;
        Ok(RpcMethodReply::new(file::pwrite_response {
            count: count as u64,
        }))
    }

    unsupported_file_method!(seek, file::seek_request, file::seek_response);

    async fn stat<'s>(
        &'s mut self,
        context: &mut HostRequestContext<'ctx>,
        _request: file::stat_request,
    ) -> Result<RpcMethodReply<file::stat_response, Self::Resource>, RpcOutcome> {
        let (fd, _) = self
            .state
            .descriptor(context.request)
            .map_err(errno_failure)?;
        let meta = self.state.fs.fstat(fd).map_err(errno_failure)?;
        Ok(RpcMethodReply::new(file::stat_response {
            value: HostVfs::stat_file(meta),
        }))
    }

    async fn sync<'s>(
        &'s mut self,
        context: &mut HostRequestContext<'ctx>,
        _request: file::sync_request,
    ) -> Result<RpcMethodReply<file::sync_response, Self::Resource>, RpcOutcome> {
        self.state
            .descriptor(context.request)
            .map_err(errno_failure)?;
        Ok(RpcMethodReply::new(file::sync_response {}))
    }

    async fn truncate<'s>(
        &'s mut self,
        context: &mut HostRequestContext<'ctx>,
        value: file::truncate_request,
    ) -> Result<RpcMethodReply<file::truncate_response, Self::Resource>, RpcOutcome> {
        let (fd, mode) = self
            .state
            .descriptor(context.request)
            .map_err(errno_failure)?;
        if mode & OPEN_MODE_WRITE == 0 {
            return Err(errno_failure(Errno::EBadf));
        }
        self.state
            .fs
            .ftruncate(fd, value.length)
            .map_err(errno_failure)?;
        Ok(RpcMethodReply::new(file::truncate_response {}))
    }

    unsupported_file_method!(allocate, file::allocate_request, file::allocate_response);

    async fn get_flags<'s>(
        &'s mut self,
        context: &mut HostRequestContext<'ctx>,
        _request: file::get_flags_request,
    ) -> Result<RpcMethodReply<file::get_flags_response, Self::Resource>, RpcOutcome> {
        let (_, mode) = self
            .state
            .descriptor(context.request)
            .map_err(errno_failure)?;
        Ok(RpcMethodReply::new(file::get_flags_response {
            flags: mode,
        }))
    }

    async fn set_flags<'s>(
        &'s mut self,
        context: &mut HostRequestContext<'ctx>,
        value: file::set_flags_request,
    ) -> Result<RpcMethodReply<file::set_flags_response, Self::Resource>, RpcOutcome> {
        let id = HostVfs::endpoint_id(context.request).map_err(errno_failure)?;
        let Some(HostHandle::File { mode, .. }) = self.state.handles.get_mut(&id) else {
            return Err(errno_failure(Errno::EBadf));
        };
        *mode = value.flags;
        Ok(RpcMethodReply::new(file::set_flags_response {}))
    }

    unsupported_file_method!(
        device_control,
        file::device_control_request,
        file::device_control_response
    );

    async fn read<'s>(
        &'s mut self,
        context: &mut HostRequestContext<'ctx>,
        value: file::read_request,
    ) -> Result<RpcMethodReply<file::read_response, Self::Resource>, RpcOutcome> {
        let request = context.request;
        let (fd, mode) = self.state.descriptor(request).map_err(errno_failure)?;
        if mode & OPEN_MODE_READ == 0 {
            return Err(errno_failure(Errno::EBadf));
        }
        let count = HostVfs::read_region(request, value.buffer, 0, value.size, |data| {
            self.state.fs.read(fd, data)
        })
        .map_err(rpc_failure)?;
        Ok(RpcMethodReply::new(file::read_response {
            count: count as u64,
        }))
    }

    async fn write<'s>(
        &'s mut self,
        context: &mut HostRequestContext<'ctx>,
        value: file::write_request,
    ) -> Result<RpcMethodReply<file::write_response, Self::Resource>, RpcOutcome> {
        let request = context.request;
        let (fd, mode) = self.state.descriptor(request).map_err(errno_failure)?;
        if mode & OPEN_MODE_WRITE == 0 {
            return Err(errno_failure(Errno::EBadf));
        }
        if mode & OPEN_APPEND != 0 {
            self.state
                .fs
                .seek(fd, vfsd::backend::SeekFrom::End(0))
                .map_err(errno_failure)?;
        }
        let count = HostVfs::write_region(request, value.buffer, 0, value.size, |data| {
            self.state.fs.write(fd, data)
        })
        .map_err(rpc_failure)?;
        Ok(RpcMethodReply::new(file::write_response {
            count: count as u64,
        }))
    }

    unsupported_file_method!(preadv, file::preadv_request, file::preadv_response);
    unsupported_file_method!(pwritev, file::pwritev_request, file::pwritev_response);
    unsupported_file_method!(readv, file::readv_request, file::readv_response);
    unsupported_file_method!(writev, file::writev_request, file::writev_response);
    unsupported_file_method!(
        materialize,
        file::materialize_request,
        file::materialize_response
    );
}

struct Handler {
    state: HostVfs,
}

impl ServeHandler for Handler {
    async fn handle(&mut self, server: &mut Server, request: Request) {
        self.state.reclaim(server);
        let protocol_uuid = request.protocol_uuid;
        let dispatched = {
            let rpc_request = request.rpc_request();
            let mut context = HostRequestContext {
                request: &request,
                server,
            };
            if protocol_uuid == directory::PROTOCOL_UUID {
                directory::dispatch_servicekit(&rpc_request, &mut context, self).await
            } else if protocol_uuid == file::PROTOCOL_UUID {
                file::dispatch_servicekit(&rpc_request, &mut context, self).await
            } else {
                naos_idl::transport::RpcDispatchResult::failure(rpc_request.method_id, -71)
            }
        };
        if let Err(error) = request.respond_dispatch(dispatched).await {
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
        directory::validate_request_transport_resources,
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
