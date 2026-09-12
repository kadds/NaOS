//! Transport-neutral FAT service.
//!
//! The filesystem worker owns FAT semantics and the `BlockClient` trait owns
//! the byte-range boundary. Servicekit owns whether the endpoint is a Linux
//! UDS socket or a NaOS capability channel, so this file is deliberately
//! unaware of the target operating system.

use std::sync::Arc;
use std::vec::Vec;

use exfatd::block::{BlockClient, BlockError, BlockInfo, FEATURE_FLUSH, FEATURE_READ_ONLY};
use exfatd::core::{FatService, file_stat, path, stat_value};
use exfatd::errno::{Errno, FsError};
use exfatd::worker::FatWorker;
use naos_idl::block_device as block_idl;
use naos_idl::block_device_factory as factory_idl;
use naos_idl::directory;
use naos_idl::file;
#[cfg(target_os = "naos")]
use naos_idl::mount_ticket;
#[cfg(target_os = "naos")]
use naos_idl::vfs;
use naos_idl::{CodecError, ResourceSlot};
use naos_idl::transport::RpcRequestOwned;
use servicekit::Context;
use servicekit::client::{Client, ClientError, Response};
use servicekit::memory::MemoryObject;
use servicekit::server::{Request, Response as ServerResponse, ServeHandler, Server, ServerError};
use servicekit::sys;

#[cfg(target_os = "naos")]
use exfatd::mount_control::MountControlServer;
#[cfg(target_os = "naos")]
use servicekit::{accept_listener, publish_listener};

const WIRE_BYTES: usize = 65_536;
/// Seed for the response wire.  Every message this service encodes is a small
/// control frame (a count, a stat, an endpoint index), so the encode buffer
/// starts at one cache line and grows only if a message needs more.  A fixed
/// 64 KiB scratch per call would defeat the bulk-region data path.
const WIRE_SEED_BYTES: usize = 64;
const MAX_FILE_TRANSFER: u64 = 65_536;
const HOST_RIGHTS: u64 = u64::MAX;

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

fn encoded(encode: impl Fn(&mut [u8]) -> Result<usize, CodecError>) -> Vec<u8> {
    let mut wire = vec![0_u8; WIRE_SEED_BYTES];
    loop {
        match encode(&mut wire) {
            Ok(length) => {
                wire.truncate(length);
                return wire;
            }
            // The generated encoder reports a too-small buffer as Overflow;
            // grow within the protocol limit and encode again.
            Err(CodecError::Overflow) if wire.len() < WIRE_BYTES => {
                wire.resize((wire.len() * 2).min(WIRE_BYTES), 0);
            }
            Err(error) => panic!("generated message fits protocol limit: {error:?}"),
        }
    }
}

fn map_client_error(_: ClientError) -> BlockError {
    BlockError::Io
}

fn check_client_response(response: &Response) -> Result<(), BlockError> {
    if response.is_success() {
        return Ok(());
    }
    match response.protocol_error {
        -13 => Err(BlockError::ReadOnly),
        -22 => Err(BlockError::OutOfRange),
        _ => Err(BlockError::Io),
    }
}

/// The FAT worker's synchronous block interface over servicekit's common
/// client. `Client::invoke_blocking` is implemented by servicekit for both
/// transports; it is only called while servicing the worker's synchronous
/// metadata/data operation.
#[derive(Clone)]
struct RemoteBlockIo {
    device: Client,
    info: BlockInfo,
    /// One region reused by every transfer.  Creating it per request would
    /// make each block I/O allocate a MemoryObject and install (then tear
    /// down) its mapping; a block service runs transfers back to back through
    /// the same window, so the region and its mapping are established once.
    /// Shared, not cloned: a clone would duplicate the capability and its
    /// mapping instead of reusing one transfer window.
    region: Arc<MemoryObject>,
}

impl RemoteBlockIo {
    async fn acquire(directory: &servicekit::server::ServiceDirectory) -> Result<Self, BlockError> {
        let factory_descriptor = factory_idl::protocol_descriptor();
        let factory = Client::connect(
            directory,
            servicekit::uri::BLOCK_RAMDISK,
            &factory_descriptor,
        )
        .await
        .map_err(map_client_error)?;

        let get_info = factory_idl::get_info_request {};
        let request = encoded(|wire| factory_idl::encode_get_info_request(&get_info, wire));
        let response = factory
            .invoke_blocking(factory_idl::METHOD_GET_INFO, &request, &[], &[])
            .map_err(map_client_error)?;
        check_client_response(&response)?;
        let medium = factory_idl::decode_get_info_response(&response.payload)
            .map_err(|_| BlockError::Io)?
            .value;

        let acquire = factory_idl::acquire_request {
            start_lba: 0,
            block_count: medium.total_block_count,
            flags: 0,
        };
        let request = encoded(|wire| factory_idl::encode_acquire_request(&acquire, wire));
        let mut response = factory
            .invoke_blocking(factory_idl::METHOD_ACQUIRE, &request, &[], &[])
            .map_err(map_client_error)?;
        check_client_response(&response)?;
        let acquired =
            factory_idl::decode_acquire_response(&response.payload).map_err(|_| BlockError::Io)?;
        let lease = response
            .take_resource(acquired.device)
            .ok_or(BlockError::Io)?;
        let device_descriptor = block_idl::protocol_descriptor();
        let device = Client::from_resource(lease, &device_descriptor).map_err(map_client_error)?;
        let info = BlockInfo {
            sector_size: u32::try_from(medium.logical_block_bytes).map_err(|_| BlockError::Io)?,
            physical_sector_size: u32::try_from(medium.physical_block_bytes)
                .map_err(|_| BlockError::Io)?,
            block_count: medium.total_block_count,
            max_transfer_blocks: medium.max_transfer_blocks,
            max_transfer_bytes: medium.max_transfer_bytes,
            max_in_flight: medium.max_in_flight,
            features: medium.features,
            media_generation: medium.media_generation,
            medium_id: medium.medium_id,
            read_only: medium.features & FEATURE_READ_ONLY != 0,
        };
        if info.sector_size == 0 || info.block_count == 0 {
            return Err(BlockError::OutOfRange);
        }
        if info.physical_sector_size < info.sector_size
            || info.max_transfer_blocks == 0
            || info.max_transfer_bytes < u64::from(info.sector_size)
            || info.max_in_flight == 0
            || info.media_generation == 0
        {
            return Err(BlockError::OutOfRange);
        }
        // The transfer window is bounded by the device's advertised limits, so
        // one region sized to them serves every I/O.
        let window = usize::try_from(info.max_transfer_bytes).map_err(|_| BlockError::OutOfRange)?;
        if window == 0 {
            return Err(BlockError::OutOfRange);
        }
        let region = MemoryObject::new(window).map_err(|_| BlockError::Io)?;
        region
            .map_persistent(window, true)
            .map_err(|_| BlockError::Io)?;
        Ok(Self {
            device,
            info,
            region: Arc::new(region),
        })
    }

    fn validate_range(&self, offset: u64, length: usize) -> Result<(), BlockError> {
        let sector = u64::from(self.info.sector_size);
        if sector == 0 || offset % sector != 0 || length as u64 % sector != 0 {
            return Err(BlockError::OutOfRange);
        }
        let end = offset
            .checked_add(length as u64)
            .ok_or(BlockError::OutOfRange)?;
        if end > self.info.block_count.saturating_mul(sector) {
            return Err(BlockError::OutOfRange);
        }
        let block_count = length as u64 / sector;
        if block_count > self.info.max_transfer_blocks
            || length as u64 > self.info.max_transfer_bytes
        {
            return Err(BlockError::OutOfRange);
        }
        Ok(())
    }

    fn invoke_buffer(
        &self,
        method_id: u64,
        payload: &[u8],
        length: usize,
        direction: servicekit::memory::MemoryDirection,
        rights: u64,
    ) -> Result<Response, BlockError> {
        self.device
            .invoke_memory_blocking(
                method_id,
                payload,
                &self.region,
                0,
                length as u64,
                direction,
                rights,
            )
            .map_err(map_client_error)
    }
}

impl BlockClient for RemoteBlockIo {
    fn get_info(&self) -> Result<BlockInfo, BlockError> {
        Ok(self.info)
    }

    fn read(&self, offset: u64, buffer: &mut [u8]) -> Result<(), BlockError> {
        self.validate_range(offset, buffer.len())?;
        let request = block_idl::read_request {
            lba: offset / u64::from(self.info.sector_size),
            block_count: buffer.len() as u64 / u64::from(self.info.sector_size),
            buffer: ResourceSlot::new(0).ok_or(BlockError::Io)?,
            flags: 0,
        };
        let payload = encoded(|wire| block_idl::encode_read_request(&request, wire));
        let response = self.invoke_buffer(
            block_idl::METHOD_READ,
            &payload,
            buffer.len(),
            servicekit::memory::MemoryDirection::Out,
            sys::MEMORY_RIGHT_WRITE | sys::MEMORY_RIGHT_MAP | sys::RIGHT_TRANSFER,
        )?;
        check_client_response(&response)?;
        if !response.payload.is_empty() || !response.resources.is_empty() {
            return Err(BlockError::Io);
        }
        // The service wrote into the shared region, so this copies out of the
        // mapping the client already holds.
        self.region
            .read_region(0, buffer)
            .map_err(|_| BlockError::Io)
    }

    fn write(&self, offset: u64, buffer: &[u8]) -> Result<(), BlockError> {
        self.validate_range(offset, buffer.len())?;
        if self.info.read_only {
            return Err(BlockError::ReadOnly);
        }
        self.region
            .write_region(0, buffer)
            .map_err(|_| BlockError::Io)?;
        let request = block_idl::write_request {
            lba: offset / u64::from(self.info.sector_size),
            block_count: buffer.len() as u64 / u64::from(self.info.sector_size),
            buffer: ResourceSlot::new(0).ok_or(BlockError::Io)?,
            flags: 0,
        };
        let payload = encoded(|wire| block_idl::encode_write_request(&request, wire));
        let response = self.invoke_buffer(
            block_idl::METHOD_WRITE,
            &payload,
            buffer.len(),
            servicekit::memory::MemoryDirection::In,
            sys::MEMORY_RIGHT_READ | sys::MEMORY_RIGHT_MAP | sys::RIGHT_TRANSFER,
        )?;
        check_client_response(&response)
    }

    fn flush(&self, _: bool) -> Result<(), BlockError> {
        if self.info.features & FEATURE_FLUSH == 0 {
            return Err(BlockError::Io);
        }
        let request = block_idl::flush_request {};
        let payload = encoded(|wire| block_idl::encode_flush_request(&request, wire));
        let response = self
            .device
            .invoke_blocking(block_idl::METHOD_FLUSH, &payload, &[], &[])
            .map_err(map_client_error)?;
        check_client_response(&response)
    }
}

struct State {
    service: FatService<RemoteBlockIo>,
}

fn success(request: &Request, payload: Vec<u8>) -> ServerResponse {
    ServerResponse::success(request, payload)
}

fn failure(request: &Request, errno: i32) -> ServerResponse {
    ServerResponse::error(request, -i64::from(errno))
}

fn fs_failure(request: &Request, error: FsError) -> ServerResponse {
    failure(request, error.errno.to_i32())
}

fn endpoint_response(
    request: &Request,
    payload: Vec<u8>,
    endpoint: servicekit::server::EndpointResource,
) -> ServerResponse {
    let mut response = success(request, payload);
    response.push_resource(endpoint);
    response
}

fn checked_size(size: u64) -> Result<usize, i32> {
    if size > MAX_FILE_TRANSFER {
        return Err(22);
    }
    usize::try_from(size).map_err(|_| 22)
}

/// Errno for a region-mapping failure.  A caller that did not grant the
/// direction this operation needs is reported as EACCES; anything else is an
/// unmappable or out-of-range window, which is EIO.
fn region_failure(error: ServerError) -> i32 {
    match error {
        ServerError::Status(sys::STATUS_ACCESS_DENIED) => 13,
        _ => 5,
    }
}

/// Unwrap the region-mapping outcome and the filesystem outcome the callback
/// returned.  Both nested error carriers are flattened into one errno so the
/// dispatch arms stay uniform.
fn region_outcome<T>(result: Result<Result<T, i64>, ServerError>) -> Result<T, i32> {
    match result {
        Ok(Ok(value)) => Ok(value),
        Ok(Err(domain)) => Err((-domain) as i32),
        Err(error) => Err(region_failure(error)),
    }
}

/// Domain error for a filesystem failure reported through a region callback.
fn domain_error(error: FsError) -> i64 {
    -i64::from(error.errno.to_i32())
}

fn directory_request(request: &Request, state: &mut State, server: &mut Server) -> ServerResponse {
    // Directory listing is a bulk method at this revision and the worker
    // endpoint (MountControl) owns it; the standalone service never
    // implemented it, so report it as unsupported rather than letting its
    // mandatory region argument look like a malformed request.
    if request.method_id == directory::METHOD_LIST {
        return failure(request, 38);
    }
    if !request.resources.is_empty() || !request.bulk.is_empty() {
        return failure(request, 22);
    }
    let service = &mut state.service;
    match request.method_id {
        directory::METHOD_OPEN => {
            let value = match directory::decode_open_request(&request.payload) {
                Ok(value) => value,
                Err(_) => return failure(request, 22),
            };
            let name = match path(value.path) {
                Ok(name) => name,
                Err(errno) => return failure(request, errno),
            };
            if let Err(error) = service.lookup(&name) {
                return fs_failure(request, error);
            }
            let endpoint = match server.create_endpoint(
                &file::protocol_descriptor(),
                0,
                file::PROTOCOL_SCOPE,
                HOST_RIGHTS,
                validate_file_request,
            ) {
                Ok(endpoint) => endpoint,
                Err(_) => return failure(request, 12),
            };
            service.open_description_with_id(endpoint.descriptor().resource_id, name, value.mode);
            let response = directory::open_response {
                object: ResourceSlot::new(0).expect("resource slot zero is valid"),
            };
            endpoint_response(
                request,
                encoded(|wire| directory::encode_open_response(&response, wire)),
                endpoint,
            )
        }
        directory::METHOD_CREATE => {
            let value = match directory::decode_create_request(&request.payload) {
                Ok(value) => value,
                Err(_) => return failure(request, 22),
            };
            let name = match path(value.path) {
                Ok(name) => name,
                Err(errno) => return failure(request, errno),
            };
            if value.flags & 1 != 0 {
                service.mkdir(&name).map_or_else(
                    |error| fs_failure(request, error),
                    |_| success(request, Vec::new()),
                )
            } else {
                service.create_file(&name).map_or_else(
                    |error| fs_failure(request, error),
                    |_| success(request, Vec::new()),
                )
            }
        }
        directory::METHOD_REMOVE => {
            let value = match directory::decode_remove_request(&request.payload) {
                Ok(value) => value,
                Err(_) => return failure(request, 22),
            };
            let name = match path(value.path) {
                Ok(name) => name,
                Err(errno) => return failure(request, errno),
            };
            if value.mode & 1 != 0 {
                service.rmdir(&name).map_or_else(
                    |error| fs_failure(request, error),
                    |_| success(request, Vec::new()),
                )
            } else {
                if service.has_open_path(&name) {
                    return failure(request, 16);
                }
                service.unlink(&name).map_or_else(
                    |error| fs_failure(request, error),
                    |_| success(request, Vec::new()),
                )
            }
        }
        directory::METHOD_STAT => service.lookup("/").map_or_else(
            |error| fs_failure(request, error),
            |stat| {
                let response = directory::stat_response {
                    value: stat_value(stat, service.device_id()),
                };
                success(
                    request,
                    encoded(|wire| directory::encode_stat_response(&response, wire)),
                )
            },
        ),
        directory::METHOD_STAT_NODE => {
            let value = match directory::decode_stat_node_request(&request.payload) {
                Ok(value) => value,
                Err(_) => return failure(request, 22),
            };
            let name = match path(value.path) {
                Ok(name) => name,
                Err(errno) => return failure(request, errno),
            };
            service.lookup(&name).map_or_else(
                |error| fs_failure(request, error),
                |stat| {
                    let response = directory::stat_node_response {
                        value: stat_value(stat, service.device_id()),
                    };
                    success(
                        request,
                        encoded(|wire| directory::encode_stat_node_response(&response, wire)),
                    )
                },
            )
        }
        directory::METHOD_ACCESS => {
            let value = match directory::decode_access_request(&request.payload) {
                Ok(value) => value,
                Err(_) => return failure(request, 22),
            };
            let name = match path(value.path) {
                Ok(name) => name,
                Err(errno) => return failure(request, errno),
            };
            service.access(&name, value.mode as u32).map_or_else(
                |error| fs_failure(request, error),
                |_| success(request, Vec::new()),
            )
        }
        directory::METHOD_RENAME => {
            let value = match directory::decode_rename_request(&request.payload) {
                Ok(value) => value,
                Err(_) => return failure(request, 22),
            };
            let first = match path(value.first) {
                Ok(path) => path,
                Err(errno) => return failure(request, errno),
            };
            let second = match path(value.second) {
                Ok(path) => path,
                Err(errno) => return failure(request, errno),
            };
            match service.rename(&first, &second) {
                Ok(()) => {
                    service.rewrite_open_paths(&first, &second);
                    success(request, Vec::new())
                }
                Err(error) => fs_failure(request, error),
            }
        }
        directory::METHOD_LINK | directory::METHOD_SYMLINK | directory::METHOD_READLINK => {
            fs_failure(request, FsError::new(Errno::EOpNotSupp))
        }
        directory::METHOD_SYNC => service.sync(true).map_or_else(
            |error| fs_failure(request, error),
            |_| success(request, Vec::new()),
        ),
        directory::METHOD_CLONE_BINDING => {
            let endpoint = match server.create_endpoint(
                &directory::protocol_descriptor(),
                0,
                directory::PROTOCOL_SCOPE,
                HOST_RIGHTS,
                validate_directory_request,
            ) {
                Ok(endpoint) => endpoint,
                Err(_) => return failure(request, 12),
            };
            service.open_description_with_id(endpoint.descriptor().resource_id, "/".to_owned(), 0);
            let response = directory::clone_binding_response {
                directory: ResourceSlot::new(0).expect("resource slot zero is valid"),
            };
            endpoint_response(
                request,
                encoded(|wire| directory::encode_clone_binding_response(&response, wire)),
                endpoint,
            )
        }
        _ => failure(request, 38),
    }
}

fn file_resource(request: &Request) -> Result<u64, i32> {
    let resource = if let Some(target) = request.target {
        // A migrated bulk method names its region in slot 0; more than one
        // resource on an endpoint request is malformed.
        if request.resources.len() > 1 {
            return Err(22);
        }
        target
    } else {
        if request.resources.len() != 1 {
            return Err(22);
        }
        request.resources[0]
    };
    if resource.resource_id == 0
        || (resource.scope != file::PROTOCOL_SCOPE && resource.scope != directory::PROTOCOL_SCOPE)
        || !request.validate_resource(resource)
    {
        return Err(13);
    }
    Ok(resource.resource_id)
}

fn file_request(request: &Request, state: &mut State) -> ServerResponse {
    // Every payload method at this revision names a bulk region; the inline
    // File methods (stat, seek, truncate, sync, flags, materialize) must not
    // carry one, so a request that does is malformed.
    let bulk_method = matches!(
        request.method_id,
        file::METHOD_PREAD
            | file::METHOD_READ
            | file::METHOD_PWRITE
            | file::METHOD_WRITE
            | file::METHOD_PREADV
            | file::METHOD_PWRITEV
            | file::METHOD_READV
            | file::METHOD_WRITEV
            | file::METHOD_DEVICE_CONTROL
    );
    if !bulk_method && !request.bulk.is_empty() {
        return failure(request, 22);
    }
    let resource = match file_resource(request) {
        Ok(resource) => resource,
        Err(errno) => return failure(request, errno),
    };
    let (name, offset) = match state.service.description(resource).cloned() {
        Some(value) => (value.path, value.offset),
        None => return failure(request, 9),
    };
    let service = &mut state.service;
    match request.method_id {
        file::METHOD_PREAD => {
            let value = match file::decode_pread_request(&request.payload) {
                Ok(value) => value,
                Err(_) => return failure(request, 22),
            };
            let size = match checked_size(value.size) {
                Ok(size) => size,
                Err(errno) => return failure(request, errno),
            };
            if size == 0 {
                let response = file::pread_response { count: 0 };
                return success(
                    request,
                    encoded(|wire| file::encode_pread_response(&response, wire)),
                );
            }
            // The region admission decision covers which rights the caller
            // granted and which direction this operation needs; a caller that
            // did not grant them is denied rather than reported as malformed.
            if !request.validate_memory(value.buffer, false) {
                return failure(request, 13);
            }
            let offset = value.offset.max(0) as u64;
            let count = match region_outcome(request.with_write_memory(
                value.buffer,
                0,
                size,
                |data| service.read_at(&name, offset, data).map_err(domain_error),
            )) {
                Ok(count) => count,
                Err(errno) => return failure(request, errno),
            };
            let response = file::pread_response {
                count: count as u64,
            };
            success(
                request,
                encoded(|wire| file::encode_pread_response(&response, wire)),
            )
        }
        file::METHOD_READ => {
            let value = match file::decode_read_request(&request.payload) {
                Ok(value) => value,
                Err(_) => return failure(request, 22),
            };
            let size = match checked_size(value.size) {
                Ok(size) => size,
                Err(errno) => return failure(request, errno),
            };
            if size == 0 {
                let response = file::read_response { count: 0 };
                return success(
                    request,
                    encoded(|wire| file::encode_read_response(&response, wire)),
                );
            }
            if !request.validate_memory(value.buffer, false) {
                return failure(request, 13);
            }
            let start = offset.max(0) as u64;
            let count = match region_outcome(request.with_write_memory(
                value.buffer,
                0,
                size,
                |data| service.read_at(&name, start, data).map_err(domain_error),
            )) {
                Ok(count) => count,
                Err(errno) => return failure(request, errno),
            };
            let _ = service.set_offset(resource, offset.saturating_add(count as i64));
            let response = file::read_response {
                count: count as u64,
            };
            success(
                request,
                encoded(|wire| file::encode_read_response(&response, wire)),
            )
        }
        file::METHOD_PWRITE | file::METHOD_WRITE => {
            let (value_offset, size, buffer) =
                if request.method_id == file::METHOD_PWRITE {
                    let value = match file::decode_pwrite_request(&request.payload) {
                        Ok(value) => value,
                        Err(_) => return failure(request, 22),
                    };
                    (
                        value.offset.max(0),
                        value.size,
                        value.buffer,
                    )
                } else {
                    let value = match file::decode_write_request(&request.payload) {
                        Ok(value) => value,
                        Err(_) => return failure(request, 22),
                    };
                    (
                        offset,
                        value.size,
                        value.buffer,
                    )
                };
            let size = match checked_size(size) {
                Ok(size) => size,
                Err(errno) => return failure(request, errno),
            };
            let response = if size == 0 {
                file::write_response { count: 0 }
            } else {
                if !request.validate_memory(buffer, true) {
                    return failure(request, 13);
                }
                let count = match region_outcome(request.with_read_memory(
                    buffer,
                    0,
                    size,
                    |data| {
                        service
                            .write_at(&name, value_offset as u64, data)
                            .map_err(domain_error)
                    },
                )) {
                    Ok(count) => count,
                    Err(errno) => return failure(request, errno),
                };
                let _ = service.set_offset(resource, value_offset.saturating_add(count as i64));
                file::write_response {
                    count: count as u64,
                }
            };
            if request.method_id == file::METHOD_PWRITE {
                let response = file::pwrite_response {
                    count: response.count,
                };
                success(
                    request,
                    encoded(|wire| file::encode_pwrite_response(&response, wire)),
                )
            } else {
                success(
                    request,
                    encoded(|wire| file::encode_write_response(&response, wire)),
                )
            }
        }
        file::METHOD_STAT => service.lookup(&name).map_or_else(
            |error| fs_failure(request, error),
            |stat| {
                let response = file::stat_response {
                    value: file_stat(stat, service.device_id()),
                };
                success(
                    request,
                    encoded(|wire| file::encode_stat_response(&response, wire)),
                )
            },
        ),
        file::METHOD_TRUNCATE => {
            let value = match file::decode_truncate_request(&request.payload) {
                Ok(value) => value,
                Err(_) => return failure(request, 22),
            };
            service.truncate_to(&name, value.length).map_or_else(
                |error| fs_failure(request, error),
                |_| success(request, Vec::new()),
            )
        }
        file::METHOD_SYNC => service.sync(true).map_or_else(
            |error| fs_failure(request, error),
            |_| success(request, Vec::new()),
        ),
        _ => failure(request, 38),
    }
}

fn dispatch(request: &Request, state: &mut State, server: &mut Server) -> ServerResponse {
    let expected_revision = if request.protocol_uuid == directory::PROTOCOL_UUID {
        directory::PROTOCOL_REVISION
    } else if request.protocol_uuid == file::PROTOCOL_UUID {
        file::PROTOCOL_REVISION
    } else {
        return failure(request, 71);
    };
    if request.revision != expected_revision {
        return failure(request, 71);
    }
    if request.protocol_uuid == directory::PROTOCOL_UUID {
        directory_request(request, state, server)
    } else if request.protocol_uuid == file::PROTOCOL_UUID {
        file_request(request, state)
    } else {
        failure(request, 71)
    }
}

async fn service(context: Context) -> i64 {
    let directory = context.service_directory();
    let block = match RemoteBlockIo::acquire(&directory).await {
        Ok(block) => block,
        Err(error) => {
            log::error!("block service acquire failed: {error:?}");
            return 1;
        }
    };
    let format_requested = context.has_flag("--format");
    let worker = match if format_requested {
        FatWorker::format(block)
    } else {
        FatWorker::mount(block)
    } {
        Ok(worker) => worker,
        Err(error) => {
            if format_requested {
                log::error!("FAT volume format failed: {error:?}");
            } else {
                log::error!(
                    "FAT volume mount failed (format is an explicit offline operation): {error:?}"
                );
            }
            return 1;
        }
    };
    if format_requested {
        log::info!("FAT volume formatted and mounted device={}", worker.dev());
    } else {
        log::info!(
            "existing FAT volume mounted device={} read_only={}",
            worker.dev(),
            worker.is_read_only()
        );
    }

    let mut server = match Server::publish(
        &directory,
        servicekit::uri::FS_EXFAT,
        &directory::protocol_descriptor(),
        validate_directory_request,
    )
    .await
    {
        Ok(server) => server,
        Err(error) => {
            log::error!("publish exfat service failed: {error:?}");
            return 1;
        }
    };
    let mut state = State {
        service: FatService::new(worker),
    };
    log::info!("ready service={}", servicekit::uri::FS_EXFAT);

    let mut handler = FatHandler { state: &mut state };
    // The loop, admission and peer-fault policy come from servicekit; the
    // handler adds only the FAT service's own dispatch.
    let status = server.serve(&mut handler).await;
    status as i64
}

/// exfatd's per-request work.
struct FatHandler<'a> {
    state: &'a mut State,
}

impl ServeHandler for FatHandler<'_> {
    async fn handle(&mut self, server: &mut Server, request: Request) {
        // fatfs and the synchronous BlockClient contract may perform real I/O.
        // Keep that work off the Tokio worker that drives servicekit; otherwise
        // one slow medium operation can starve service discovery and the
        // reactor itself.
        let response = tokio::task::block_in_place(|| dispatch(&request, self.state, server));
        if let Err(error) = server.respond(request, response).await {
            log::debug!("exfat service response failed: {error:?}");
        }
        tokio::task::yield_now().await;
    }
}

#[cfg(target_os = "naos")]
async fn native_service(context: Context) -> i64 {
    let directory = context.service_directory();
    let block = match RemoteBlockIo::acquire(&directory).await {
        Ok(block) => block,
        Err(error) => {
            log::error!("block service acquire failed: {error:?}");
            return 1;
        }
    };
    let worker = match FatWorker::mount(block.clone()) {
        Ok(worker) => worker,
        Err(error) => {
            log::error!(
                "FAT volume mount failed (format is an explicit offline operation): {error:?}"
            );
            return 1;
        }
    };

    // The worker itself is the mount manager client. It acquires the LBD
    // first, reserves /data through vfsd, and only then asks the ticket to
    // publish the worker root. No root route is advertised before this
    // transaction reaches its commit point.
    let admin = match Client::connect(
        &directory,
        servicekit::uri::FS_VFS_ADMIN,
        &vfs::protocol_descriptor(),
    )
    .await
    {
        Ok(client) => client,
        Err(error) => {
            log::error!("vfs mount authority unavailable: {error:?}");
            return 1;
        }
    };
    let request = vfs::prepare_mount_request {
        target_size: 5,
        target: b"/data",
        flags: 0,
    };
    let mut request_wire = vec![0_u8; 256];
    let request_bytes = match vfs::encode_prepare_mount_request(&request, &mut request_wire) {
        Ok(bytes) => bytes,
        Err(error) => {
            log::error!("prepare_mount encoding failed: {error:?}");
            return 1;
        }
    };
    let mut prepared = match admin
        .invoke(
            vfs::METHOD_PREPARE_MOUNT,
            &request_wire[..request_bytes],
            &[],
            &[],
        )
        .await
    {
        Ok(response) => response,
        Err(error) => {
            log::error!("prepare_mount request failed: {error:?}");
            return 1;
        }
    };
    if !prepared.is_success() {
        log::error!(
            "prepare_mount rejected protocol_error={}",
            prepared.protocol_error
        );
        return 1;
    }
    let prepared_value = match vfs::decode_prepare_mount_response(&prepared.payload) {
        Ok(value) => value,
        Err(error) => {
            log::error!("prepare_mount response malformed: {error:?}");
            return 1;
        }
    };
    let control = match prepared.take_resource(prepared_value.control) {
        Some(resource) => resource,
        None => {
            log::error!("prepare_mount response omitted MountControl");
            return 1;
        }
    };
    let ticket = match prepared.take_resource(prepared_value.ticket) {
        Some(resource) => resource,
        None => {
            log::error!("prepare_mount response omitted MountTicket");
            return 1;
        }
    };
    let control_descriptor = control.descriptor();
    if control_descriptor.binding != sys::BINDING_SERVER_END
        || control_descriptor.scope != 21
        || control_descriptor.rights & sys::RIGHT_TRANSFER == 0
    {
        log::error!("prepare_mount returned invalid MountControl descriptor");
        return 1;
    }
    let ticket_descriptor = ticket.descriptor();
    if ticket_descriptor.binding != sys::BINDING_CLIENT_END
        || ticket_descriptor.scope != mount_ticket::PROTOCOL_SCOPE
        || ticket_descriptor.rights & sys::RIGHT_TRANSFER == 0
    {
        log::error!("prepare_mount returned invalid MountTicket descriptor");
        return 1;
    }
    let control = match control.into_owned_handle() {
        Ok(handle) => handle,
        Err(error) => {
            log::error!("MountControl ownership transfer failed: {error:?}");
            return 1;
        }
    };
    let ticket = match Client::from_resource(ticket, &mount_ticket::protocol_descriptor()) {
        Ok(client) => client,
        Err(error) => {
            log::error!("MountTicket adoption failed: {error:?}");
            return 1;
        }
    };
    let mut mount_server = unsafe { MountControlServer::from_raw(control.into_raw()) };
    mount_server.install_worker(worker);

    let commit_request = mount_ticket::commit_request {
        root_node: 1,
        root_generation: 1,
    };
    let mut commit_wire = [0_u8; 64];
    let commit_bytes = match mount_ticket::encode_commit_request(&commit_request, &mut commit_wire)
    {
        Ok(bytes) => bytes,
        Err(error) => {
            log::error!("MountTicket commit encoding failed: {error:?}");
            return 1;
        }
    };
    let mut commit = Box::pin(ticket.invoke(
        mount_ticket::METHOD_COMMIT,
        &commit_wire[..commit_bytes],
        &[],
        &[],
    ));
    let mut readiness = servicekit::ReadinessSet::new();
    let mount_info = loop {
        let handles = mount_server.wait_handles();
        if let Err(status) = readiness.refresh(&handles) {
            log::error!("MountControl readiness registration failed: {status}");
            return 1;
        }
        let outcome = tokio::select! {
            response = &mut commit => Some(response),
            ready = readiness.wait(None) => {
                match ready {
                    Ok(event) => {
                        if let Err(status) = mount_server.serve_once() {
                            log::error!("MountControl service failed: {status}");
                            return 1;
                        }
                        readiness.clear(event.index);
                    }
                    Err(status) => {
                        log::error!("MountControl readiness failed: {status}");
                        return 1;
                    }
                }
                None
            }
        };
        let Some(response) = outcome else {
            continue;
        };
        let mut response = match response {
            Ok(response) => response,
            Err(error) => {
                log::error!("MountTicket commit request failed: {error:?}");
                return 1;
            }
        };
        if !response.is_success() {
            log::error!(
                "MountTicket commit rejected protocol_error={}",
                response.protocol_error
            );
            return 1;
        }
        break match mount_ticket::decode_commit_response(&response.payload) {
            Ok(value) => value.value,
            Err(error) => {
                log::error!("MountTicket commit response malformed: {error:?}");
                return 1;
            }
        };
    };
    log::info!(
        "mount committed mount={} generation={} device={}",
        mount_info.mount_id,
        mount_info.backend_generation,
        mount_info.device_id
    );

    let listener = match publish_listener(
        context.service_directory_handle(),
        servicekit::uri::FS_EXFAT,
        &directory::protocol_descriptor(),
    ) {
        Ok(listener) => listener,
        Err(status) => {
            log::error!("publish exfat service failed: {status}");
            return 1;
        }
    };
    log::info!("ready service={}", servicekit::uri::FS_EXFAT);
    loop {
        let mut handles = mount_server.wait_handles();
        handles.push(listener.get());
        if let Err(status) = readiness.refresh(&handles) {
            log::error!("exfat service readiness registration failed: {status}");
            return 1;
        }
        let event = match readiness.wait(None).await {
            Ok(event) => event,
            Err(status) => {
                log::error!("exfat service readiness failed: {status}");
                return 1;
            }
        };
        if event.index + 1 == handles.len() {
            loop {
                match accept_listener(&listener) {
                    Ok(endpoint) => mount_server.add_directory_endpoint(endpoint),
                    Err(naos_idl::CallError::Status(status))
                        if status == sys::STATUS_WOULD_BLOCK =>
                    {
                        break;
                    }
                    Err(error) => {
                        log::error!("exfat listener accept failed: {error:?}");
                        return 1;
                    }
                }
            }
        }
        if let Err(status) = mount_server.serve_once() {
            log::error!("exfat worker service failed: {status}");
            return 1;
        }
        readiness.clear(event.index);
    }
}

#[cfg(target_os = "linux")]
pub async fn run(context: Context) -> i64 {
    service(context).await
}

#[cfg(target_os = "naos")]
pub async fn run(context: Context) -> i64 {
    native_service(context).await
}
