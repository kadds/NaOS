//! Full host-side application round trip through MountControl -> Directory ->
//! File. This uses the IDL loopback kernel, so no real block device is used.

#![cfg(feature = "test-loopback")]

use exfatd::block::RamBlockDevice;
use exfatd::mount_control::{METHOD_BIND_ROOT, MountControlServer};
use exfatd::worker::FatWorker;
use naos_idl::directory;
use naos_idl::file;
use naos_idl::loopback;
use naos_idl::{
    Invocation, ProtocolClientEndpoint, ReceivedResources, ResourceSlot, ResourceTable,
};
use naos_sys as sys;

const SECTORS: usize = 69_632;

fn worker() -> FatWorker<RamBlockDevice> {
    FatWorker::format(RamBlockDevice::new(vec![0; SECTORS * 512], 512)).unwrap()
}

fn take_raw(
    invocation: &mut Invocation,
) -> (Vec<u8>, ReceivedResources, naos_idl::loopback::RawResult) {
    let mut bytes = vec![0_u8; 65_536];
    let mut handles = [sys::HANDLE_INVALID; naos_idl::MAX_RESOURCES];
    let result =
        loopback::raw_take_result_into(invocation.get(), &mut bytes, &mut handles).unwrap();
    invocation.mark_completed();
    let actual_bytes = result.actual_bytes as usize;
    let actual_resources = result.actual_resources as usize;
    let received = unsafe { ReceivedResources::from_raw(&handles[..actual_resources]) }.unwrap();
    bytes.truncate(actual_bytes);
    (bytes, received, result)
}

/// Region capability for one bulk File request.
///
/// The loopback kernel has no MemoryObject mapping syscall, so the bytes a
/// service reaches through the received capability handle are registered with
/// servicekit's host-test registry.  The capability carries the metadata the
/// generated receive validation checks: a memory object at its protocol
/// scope, transferable, with MAP and the direction right (WRITE when the
/// service produces the bytes, READ when it consumes them).
fn memory_region(bytes: &[u8], service_writes: bool) -> naos_idl::OwnedHandle {
    let direction = if service_writes {
        sys::MEMORY_RIGHT_WRITE
    } else {
        sys::MEMORY_RIGHT_READ
    };
    let handle = loopback::add_capability_with_rights(
        sys::BINDING_MEMORY_OBJECT,
        naos_idl::memory_object::PROTOCOL_SCOPE,
        sys::RIGHT_DUPLICATE | sys::RIGHT_TRANSFER,
        sys::MEMORY_RIGHT_MAP | direction,
    );
    servicekit::memory::register_loopback_region(handle.get(), bytes.to_vec());
    handle
}

/// Read the host bytes a loopback region is backed by.
fn region_bytes(region: &naos_idl::OwnedHandle) -> Vec<u8> {
    servicekit::memory::with_loopback_region(region.get(), |bytes| bytes.to_vec())
        .expect("registered loopback region")
}

#[test]
fn directory_and_file_requests_reach_fat_worker() {
    loopback::install();
    // The worker control protocol is private and has no public generated
    // binding. The loopback transport does not inspect descriptor UUIDs, so a
    // Directory pair is sufficient to drive the same server dispatch here.
    let (control, control_server) = directory::create_endpoints(None).unwrap();
    let mut server =
        unsafe { MountControlServer::<RamBlockDevice>::from_raw(control_server.into_raw()) };
    server.install_worker(worker());

    let mut bind = loopback::raw_invoke_submit(control.get(), METHOD_BIND_ROOT, &[]).unwrap();
    server.serve_once().unwrap();
    let (bind_wire, mut bind_resources, bind_frame) = take_raw(&mut bind);
    assert_eq!(bind_frame.execution, naos_idl::EXECUTION_NONE);
    assert_eq!(bind_wire.len(), 4);
    let slot = ResourceSlot::new(u32::from_le_bytes(bind_wire.try_into().unwrap())).unwrap();
    let directory_raw = bind_resources.take(slot).unwrap().into_raw();
    let directory = unsafe { ProtocolClientEndpoint::from_raw(directory_raw) };

    let mut wire = vec![0_u8; 8192];
    let create = directory::create_request {
        mode: 0,
        flags: 0,
        path: b"old.txt",
    };
    let mut invocation =
        directory::submit_create(&directory, &create, ResourceTable::new(), &mut wire, 0).unwrap();
    server.serve_once().unwrap();
    let mut response = [0_u8; 64];
    directory::take_create(&mut invocation, &mut response).unwrap();

    let open = directory::open_request {
        mode: 1 | 2,
        flags: 0,
        path: b"old.txt",
    };
    let mut invocation =
        directory::submit_open(&directory, &open, ResourceTable::new(), &mut wire, 0).unwrap();
    server.serve_once().unwrap();
    let mut response = [0_u8; 64];
    let (open_response, mut resources) =
        directory::take_open(&mut invocation, &mut response).unwrap();
    let file_raw = resources.take(open_response.object).unwrap().into_raw();
    let file_endpoint = unsafe { ProtocolClientEndpoint::from_raw(file_raw) };

    // File payloads travel as a MemoryObject region: the request names the
    // caller's buffer as a capability plus an offset and length, and the
    // service reads the bytes straight out of that region.
    const PAYLOAD: &[u8] = b"abcdef";
    let write_region = memory_region(PAYLOAD, false);
    let mut write_resources = ResourceTable::new();
    let write = file::write_request {
        size: PAYLOAD.len() as u64,
        flags: 0,
        buffer: write_resources.push_duplicate(&write_region).unwrap(),
    };
    let mut invocation =
        file::submit_write(&file_endpoint, &write, write_resources, &mut wire, 0).unwrap();
    server.serve_once().unwrap();
    let mut response = [0_u8; 64];
    assert_eq!(
        file::take_write(&mut invocation, &mut response)
            .unwrap()
            .count,
        6
    );

    // A read writes into the caller's region; the bytes are then read back out
    // of the region rather than out of the response payload.  The region
    // starts zeroed so the read-back proves the service wrote it.
    let blank = vec![0_u8; PAYLOAD.len()];
    let read_region = memory_region(&blank, true);
    let mut read_resources = ResourceTable::new();
    let read = file::pread_request {
        offset: 0,
        size: PAYLOAD.len() as u64,
        flags: 0,
        buffer: read_resources.push_duplicate(&read_region).unwrap(),
    };
    let mut invocation =
        file::submit_pread(&file_endpoint, &read, read_resources, &mut wire, 0).unwrap();
    server.serve_once().unwrap();
    assert_eq!(
        file::take_pread(&mut invocation, &mut response)
            .unwrap()
            .count,
        PAYLOAD.len() as u64
    );
    assert_eq!(region_bytes(&read_region), PAYLOAD.to_vec());

    // The vectored file methods use one bounded region whose bytes are split
    // according to the fixed IOVLayout.  Exercise both positional and
    // cursor-advancing variants so the worker cannot silently fall back to
    // EOPNOTSUPP on the rootfs path.
    let mut lengths = [0_u64; 16];
    lengths[0] = 3;
    lengths[1] = 3;
    let layout = file::IOVLayout {
        segment_count: 2,
        lengths,
    };
    let rewind = file::seek_request {
        offset: 0,
        whence: 1,
    };
    let mut invocation =
        file::submit_seek(&file_endpoint, &rewind, ResourceTable::new(), &mut wire, 0).unwrap();
    server.serve_once().unwrap();
    assert_eq!(file::take_seek(&mut invocation, &mut response).unwrap().offset, 0);

    let writev_region = memory_region(PAYLOAD, false);
    let mut writev_resources = ResourceTable::new();
    let writev = file::writev_request {
        layout,
        size: PAYLOAD.len() as u64,
        flags: 0,
        buffer: writev_resources.push_duplicate(&writev_region).unwrap(),
    };
    let mut invocation =
        file::submit_writev(&file_endpoint, &writev, writev_resources, &mut wire, 0).unwrap();
    server.serve_once().unwrap();
    assert_eq!(
        file::take_writev(&mut invocation, &mut response)
            .unwrap()
            .count,
        PAYLOAD.len() as u64
    );

    let mut invocation =
        file::submit_seek(&file_endpoint, &rewind, ResourceTable::new(), &mut wire, 0).unwrap();
    server.serve_once().unwrap();
    assert_eq!(file::take_seek(&mut invocation, &mut response).unwrap().offset, 0);

    let readv_region = memory_region(&[0_u8; PAYLOAD.len()], true);
    let mut readv_resources = ResourceTable::new();
    let readv = file::readv_request {
        layout,
        size: PAYLOAD.len() as u64,
        flags: 0,
        buffer: readv_resources.push_duplicate(&readv_region).unwrap(),
    };
    let mut invocation =
        file::submit_readv(&file_endpoint, &readv, readv_resources, &mut wire, 0).unwrap();
    server.serve_once().unwrap();
    assert_eq!(
        file::take_readv(&mut invocation, &mut response)
            .unwrap()
            .count,
        PAYLOAD.len() as u64
    );
    assert_eq!(region_bytes(&readv_region), PAYLOAD.to_vec());

    let pwritev_region = memory_region(b"ghijkl", false);
    let mut pwritev_resources = ResourceTable::new();
    let pwritev = file::pwritev_request {
        offset: 0,
        layout,
        size: 6,
        flags: 0,
        buffer: pwritev_resources.push_duplicate(&pwritev_region).unwrap(),
    };
    let mut invocation = file::submit_pwritev(
        &file_endpoint,
        &pwritev,
        pwritev_resources,
        &mut wire,
        0,
    )
    .unwrap();
    server.serve_once().unwrap();
    assert_eq!(
        file::take_pwritev(&mut invocation, &mut response)
            .unwrap()
            .count,
        6
    );

    let preadv_region = memory_region(&[0_u8; 6], true);
    let mut preadv_resources = ResourceTable::new();
    let preadv = file::preadv_request {
        offset: 0,
        layout,
        size: 6,
        flags: 0,
        buffer: preadv_resources.push_duplicate(&preadv_region).unwrap(),
    };
    let mut invocation =
        file::submit_preadv(&file_endpoint, &preadv, preadv_resources, &mut wire, 0).unwrap();
    server.serve_once().unwrap();
    assert_eq!(
        file::take_preadv(&mut invocation, &mut response)
            .unwrap()
            .count,
        6
    );
    assert_eq!(region_bytes(&preadv_region), b"ghijkl".to_vec());

    // Directory.list names the region the same way: the records are encoded
    // straight into the caller's region and only counts travel in the reply.
    let records_region = memory_region(&[0_u8; 256], true);
    let mut list_resources = ResourceTable::new();
    let list = directory::list_request {
        offset: 0,
        requested_bytes: 256,
        buffer: list_resources.push_duplicate(&records_region).unwrap(),
    };
    let mut invocation =
        directory::submit_list(&directory, &list, list_resources, &mut wire, 0).unwrap();
    server.serve_once().unwrap();
    let list_response = directory::take_list(&mut invocation, &mut response).unwrap();
    assert!(list_response.count >= 1, "the open file must be listed");
    assert!(list_response.bytes <= 256, "records must fit the region window");
    let records = region_bytes(&records_region);
    let written = list_response.bytes as usize;
    assert!(
        records[..written]
            .windows(b"old.txt".len())
            .any(|window| window == b"old.txt"),
        "record bytes must land in the caller's region"
    );

    let mut invocation = file::submit_stat(
        &file_endpoint,
        &file::stat_request {},
        ResourceTable::new(),
        &mut wire,
        0,
    )
    .unwrap();
    server.serve_once().unwrap();
    assert_eq!(
        file::take_stat(&mut invocation, &mut [0_u8; 256])
            .unwrap()
            .value
            .size,
        6
    );

    let truncate = file::truncate_request { length: 2 };
    let mut invocation = file::submit_truncate(
        &file_endpoint,
        &truncate,
        ResourceTable::new(),
        &mut wire,
        0,
    )
    .unwrap();
    server.serve_once().unwrap();
    file::take_truncate(&mut invocation, &mut response).unwrap();

    let mut invocation = file::submit_stat(
        &file_endpoint,
        &file::stat_request {},
        ResourceTable::new(),
        &mut wire,
        0,
    )
    .unwrap();
    server.serve_once().unwrap();
    assert_eq!(
        file::take_stat(&mut invocation, &mut [0_u8; 256])
            .unwrap()
            .value
            .size,
        2
    );

    let mut invocation = directory::submit_rename(
        &directory,
        &directory::rename_request {
            first_size: 7,
            second_size: 7,
            first: b"old.txt",
            second: b"new.txt",
        },
        ResourceTable::new(),
        &mut wire,
        0,
    )
    .unwrap();
    server.serve_once().unwrap();
    directory::take_rename(&mut invocation, &mut response).unwrap();

    let excl = directory::open_request {
        mode: 1 | 128,
        flags: 1,
        path: b"new.txt",
    };
    let mut invocation =
        directory::submit_open(&directory, &excl, ResourceTable::new(), &mut wire, 0).unwrap();
    server.serve_once().unwrap();
    let error = match directory::take_open(&mut invocation, &mut response) {
        Err(error) => error,
        Ok(_) => panic!("O_EXCL unexpectedly opened an existing file"),
    };
    assert!(matches!(
        error,
        naos_idl::CallError::Outcome {
            protocol_error: -17,
            ..
        }
    ));

    // The already-open description remains usable after the directory rename;
    // MountControl rewrites its path index before servicing the next request.
    let mut invocation = file::submit_stat(
        &file_endpoint,
        &file::stat_request {},
        ResourceTable::new(),
        &mut wire,
        0,
    )
    .unwrap();
    server.serve_once().unwrap();
    assert_eq!(
        file::take_stat(&mut invocation, &mut [0_u8; 256])
            .unwrap()
            .value
            .size,
        2
    );

    let mut invocation = directory::submit_stat_node(
        &directory,
        &directory::stat_node_request {
            flags: 0,
            path_size: 8,
            path: b"new.txt\0",
        },
        ResourceTable::new(),
        &mut wire,
        0,
    )
    .unwrap();
    server.serve_once().unwrap();
    assert_eq!(
        directory::take_stat_node(&mut invocation, &mut [0_u8; 256])
            .unwrap()
            .value
            .size,
        2
    );

    // Re-open after rename: the worker intentionally models open
    // descriptions by path rather than retaining a self-referential FAT
    // file object.
    let open = directory::open_request {
        mode: 1,
        flags: 0,
        path: b"new.txt",
    };
    let mut invocation =
        directory::submit_open(&directory, &open, ResourceTable::new(), &mut wire, 0).unwrap();
    server.serve_once().unwrap();
    let (open_response, mut resources) =
        directory::take_open(&mut invocation, &mut response).unwrap();
    let renamed_file = unsafe {
        ProtocolClientEndpoint::from_raw(resources.take(open_response.object).unwrap().into_raw())
    };
    let mut invocation = file::submit_stat(
        &renamed_file,
        &file::stat_request {},
        ResourceTable::new(),
        &mut wire,
        0,
    )
    .unwrap();
    server.serve_once().unwrap();
    assert_eq!(
        file::take_stat(&mut invocation, &mut [0_u8; 256])
            .unwrap()
            .value
            .size,
        2
    );

    // The path-reopen implementation refuses unlink while a live File
    // endpoint still refers to the path, preserving a deterministic open
    // description rather than silently turning its next I/O into ENOENT.
    let remove = directory::remove_request {
        mode: 0,
        flags: 0,
        path: b"new.txt",
    };
    let mut invocation =
        directory::submit_remove(&directory, &remove, ResourceTable::new(), &mut wire, 0).unwrap();
    server.serve_once().unwrap();
    let error = match directory::take_remove(&mut invocation, &mut response) {
        Err(error) => error,
        Ok(_) => panic!("unlink unexpectedly removed an actively open file"),
    };
    assert!(matches!(
        error,
        naos_idl::CallError::Outcome {
            protocol_error: -16,
            ..
        }
    ));

    // Cross-directory rename_at consumes a MOVE clone of the destination
    // Directory.  The worker resolves that endpoint through its path RPC and
    // still commits the FAT rename in one worker instance.
    for path in [b"left".as_slice(), b"right".as_slice()] {
        let create = directory::create_request {
            mode: 0,
            flags: 1,
            path,
        };
        let mut invocation =
            directory::submit_create(&directory, &create, ResourceTable::new(), &mut wire, 0)
                .unwrap();
        server.serve_once().unwrap();
        directory::take_create(&mut invocation, &mut response).unwrap();
    }
    let open_directory = |name: &'static [u8],
                          server: &mut MountControlServer<RamBlockDevice>,
                          directory: &ProtocolClientEndpoint,
                          wire: &mut Vec<u8>,
                          response: &mut [u8; 64]| {
        let request = directory::open_request {
            mode: 1,
            flags: 16,
            path: name,
        };
        let mut invocation =
            directory::submit_open(directory, &request, ResourceTable::new(), wire, 0).unwrap();
        server.serve_once().unwrap();
        let (value, mut resources) = directory::take_open(&mut invocation, response).unwrap();
        unsafe {
            ProtocolClientEndpoint::from_raw(resources.take(value.object).unwrap().into_raw())
        }
    };
    let left = open_directory(b"left", &mut server, &directory, &mut wire, &mut response);
    let right = open_directory(b"right", &mut server, &directory, &mut wire, &mut response);
    let create = directory::create_request {
        mode: 0,
        flags: 0,
        path: b"move.txt",
    };
    let mut invocation =
        directory::submit_create(&left, &create, ResourceTable::new(), &mut wire, 0).unwrap();
    server.serve_once().unwrap();
    directory::take_create(&mut invocation, &mut response).unwrap();
    let clone = {
        let mut invocation = directory::submit_clone_binding(
            &right,
            &directory::clone_binding_request {},
            ResourceTable::new(),
            &mut wire,
            0,
        )
        .unwrap();
        server.serve_once().unwrap();
        let (value, mut resources) =
            directory::take_clone_binding(&mut invocation, &mut response).unwrap();
        resources.take(value.directory).unwrap()
    };
    let mut resources = ResourceTable::new();
    let new_parent = resources.push_move(clone).unwrap();
    let request = directory::rename_at_request {
        flags: 0,
        new_parent,
        first_size: 8,
        second_size: 8,
        first: b"move.txt",
        second: b"move.txt",
    };
    let mut invocation =
        directory::submit_rename_at(&left, &request, resources, &mut wire, 0).unwrap();
    server.serve_once().unwrap();
    directory::take_rename_at(&mut invocation, &mut response).unwrap();
    let moved = directory::stat_node_request {
        flags: 0,
        path_size: 8,
        path: b"move.txt",
    };
    let mut invocation =
        directory::submit_stat_node(&right, &moved, ResourceTable::new(), &mut wire, 0).unwrap();
    server.serve_once().unwrap();
    assert_eq!(
        directory::take_stat_node(&mut invocation, &mut [0_u8; 256])
            .unwrap()
            .value
            .size,
        0
    );
}
