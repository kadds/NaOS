//! Host-only process-boundary acceptance test for ramdiskd -> exfatd -> vfsd.
//!
//! The test deliberately talks to the generated Directory/File codecs over
//! the shared Linux transport.  It does not link any daemon implementation
//! into the test process, so a green result proves the three independent
//! process boundary and the RAM block backend path.

use std::env;
use std::path::PathBuf;
use std::process::{Child, Command, Stdio};
use std::time::Duration;
use std::vec::Vec;

use naos_idl::CodecError;
use naos_idl::ResourceSlot;
use naos_idl::block_device;
use naos_idl::block_device_factory;
use naos_idl::directory;
use naos_idl::directory::DirectoryClient;
use naos_idl::file;
use naos_idl::transport::{
    BulkBuffer, BulkDirection, ResourceDescriptor, RpcRequest, RpcResponse, RpcResponseOwned,
    ServiceLocator,
};
use servicekit::memory::{
    MemoryObject, RIGHT_READ as BULK_RIGHT_READ, RIGHT_WRITE as BULK_RIGHT_WRITE,
};
use servicekit::transport::{Endpoint, ServiceDirectory, UdsTransport};
use servicekit::uri::{
    BLOCK_PREFIX, BLOCK_RAMDISK as BLOCK_SERVICE_URI, FS_EXFAT as EXFATD_SERVICE_URI, FS_PREFIX,
    FS_VFS as VFSD_SERVICE_URI,
};

struct Children(Vec<Child>);

impl Drop for Children {
    fn drop(&mut self) {
        for child in &mut self.0 {
            let _ = child.kill();
            let _ = child.wait();
        }
    }
}

fn encode(encode: impl FnOnce(&mut [u8]) -> Result<usize, CodecError>) -> Vec<u8> {
    let mut wire = vec![0; 65_536];
    let size = encode(&mut wire).expect("generated message fits");
    wire.truncate(size);
    wire
}

async fn invoke(
    transport: &UdsTransport,
    endpoint: &Endpoint,
    uuid: [u8; 16],
    revision: u64,
    method_id: u64,
    payload: &[u8],
    resources: &[ResourceDescriptor],
) -> RpcResponseOwned {
    invoke_with_bulk(
        transport,
        endpoint,
        uuid,
        revision,
        method_id,
        payload,
        resources,
        &[],
    )
    .await
}

async fn invoke_with_bulk(
    transport: &UdsTransport,
    endpoint: &Endpoint,
    uuid: [u8; 16],
    revision: u64,
    method_id: u64,
    payload: &[u8],
    resources: &[ResourceDescriptor],
    bulk: &[BulkBuffer],
) -> RpcResponseOwned {
    let response = transport
        .invoke_async(
            endpoint,
            RpcRequest {
                protocol_uuid: uuid,
                revision,
                method_id,
                request_id: 0,
                target: None,
                payload,
                resources,
                bulk,
            },
        )
        .await
        .expect("transport invocation");
    assert_eq!(response.outcome.execution, 0);
    response
}

async fn wait_for_socket(transport: &UdsTransport, endpoint: &Endpoint, request: RpcRequest<'_>) {
    // A published endpoint only becomes usable after the daemon has bound it;
    // the listener path may exist earlier, so probe by making a real request.
    for _ in 0..2000 {
        if transport.invoke_async(endpoint, request).await.is_ok() {
            return;
        }
        // This is readiness polling against an external process, not a
        // synchronization sleep: the loop exits immediately once the UDS
        // accepts a request and has a bounded retry count.
        tokio::time::sleep(Duration::from_millis(5)).await;
    }
    panic!(
        "service did not become ready: {}",
        endpoint.path().display()
    );
}

fn child(binary: &PathBuf, args: &[(&str, String)]) -> Child {
    let mut command = Command::new(binary);
    for (key, value) in args {
        command.arg(key).arg(value);
    }
    command
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .spawn()
        .expect("spawn daemon")
}

async fn block_acceptance(
    ramdiskd: &PathBuf,
    root: &PathBuf,
    transport: &UdsTransport,
    children: &mut Children,
) {
    let service_root = root.join("block-acceptance-services");
    let log_path = root.join("logs/ramdiskd-block-acceptance.log");
    let services = ServiceDirectory::new(&service_root);
    let endpoint = services
        .endpoint_for(BLOCK_SERVICE_URI)
        .expect("valid block acceptance URI");
    let acceptance_child = children.0.len();
    children.0.push(child(
        ramdiskd,
        &[
            ("--service-dir", service_root.to_string_lossy().into_owned()),
            ("--log", log_path.to_string_lossy().into_owned()),
        ],
    ));

    let get_info = encode(|wire| {
        block_device_factory::encode_get_info_request(
            &block_device_factory::get_info_request {},
            wire,
        )
    });
    wait_for_socket(
        transport,
        &endpoint,
        RpcRequest {
            protocol_uuid: block_device_factory::PROTOCOL_UUID,
            revision: block_device_factory::PROTOCOL_REVISION,
            method_id: block_device_factory::METHOD_GET_INFO,
            request_id: 0,
            target: None,
            payload: &get_info,
            resources: &[],
            bulk: &[],
        },
    )
    .await;
    let info = invoke(
        transport,
        &endpoint,
        block_device_factory::PROTOCOL_UUID,
        block_device_factory::PROTOCOL_REVISION,
        block_device_factory::METHOD_GET_INFO,
        &get_info,
        &[],
    )
    .await;
    let medium = block_device_factory::decode_get_info_response(&info.payload)
        .expect("block medium info")
        .value;
    assert!(medium.total_block_count >= 512);

    let acquire = |start_lba, flags| {
        encode(|wire| {
            block_device_factory::encode_acquire_request(
                &block_device_factory::acquire_request {
                    start_lba,
                    block_count: 256,
                    flags,
                },
                wire,
            )
        })
    };
    let writable_payload = acquire(0, 0);
    let writable = invoke(
        transport,
        &endpoint,
        block_device_factory::PROTOCOL_UUID,
        block_device_factory::PROTOCOL_REVISION,
        block_device_factory::METHOD_ACQUIRE,
        &writable_payload,
        &[],
    )
    .await;
    let writable_value = block_device_factory::decode_acquire_response(&writable.payload)
        .expect("writable lease response");
    let writable_resource = *writable
        .resources
        .get(writable_value.device.index() as usize)
        .expect("writable lease resource");
    assert_ne!(writable_resource.rights & (1 << 15), 0);
    let writable_endpoint = endpoint.with_target(writable_resource);

    let readonly_payload = acquire(256, 1);
    let readonly = invoke(
        transport,
        &endpoint,
        block_device_factory::PROTOCOL_UUID,
        block_device_factory::PROTOCOL_REVISION,
        block_device_factory::METHOD_ACQUIRE,
        &readonly_payload,
        &[],
    )
    .await;
    let readonly_value = block_device_factory::decode_acquire_response(&readonly.payload)
        .expect("read-only lease response");
    let readonly_resource = *readonly
        .resources
        .get(readonly_value.device.index() as usize)
        .expect("read-only lease resource");
    assert_eq!(readonly_resource.rights & (1 << 15), 0);
    let readonly_endpoint = endpoint.with_target(readonly_resource);

    let overlap = invoke(
        transport,
        &endpoint,
        block_device_factory::PROTOCOL_UUID,
        block_device_factory::PROTOCOL_REVISION,
        block_device_factory::METHOD_ACQUIRE,
        &readonly_payload,
        &[],
    )
    .await;
    assert_eq!(overlap.outcome.protocol_error, -16);

    let write = block_device::write_request {
        lba: 0,
        block_count: 1,
        buffer: ResourceSlot::new(0).unwrap(),
        flags: 0,
    };
    let write_payload = encode(|wire| block_device::encode_write_request(&write, wire));
    let readonly_write = invoke(
        transport,
        &readonly_endpoint,
        block_device::PROTOCOL_UUID,
        block_device::PROTOCOL_REVISION,
        block_device::METHOD_WRITE,
        &write_payload,
        &[ResourceDescriptor {
            resource_id: 1,
            binding: naos_sys::BINDING_MEMORY_OBJECT,
            scope: naos_idl::memory_object::PROTOCOL_SCOPE,
            rights: naos_sys::MEMORY_RIGHT_WRITE | naos_sys::RIGHT_TRANSFER,
            ..ResourceDescriptor::default()
        }],
    )
    .await;
    assert_eq!(readonly_write.outcome.protocol_error, -13);

    let flush =
        encode(|wire| block_device::encode_flush_request(&block_device::flush_request {}, wire));
    let flushed = invoke(
        transport,
        &writable_endpoint,
        block_device::PROTOCOL_UUID,
        block_device::PROTOCOL_REVISION,
        block_device::METHOD_FLUSH,
        &flush,
        &[],
    )
    .await;
    assert_eq!(flushed.outcome.protocol_error, 0);

    for (region_bytes, transfer_bytes) in [
        (4 * 1024, 4 * 1024),
        (64 * 1024, 64 * 1024),
        (128 * 1024, 64 * 1024),
    ] {
        let offset = if region_bytes > transfer_bytes {
            32 * 1024
        } else {
            0
        };
        let memory = MemoryObject::new(region_bytes).expect("memory object");
        let input: Vec<u8> = (0..transfer_bytes)
            .map(|index| (index as u8).wrapping_add(region_bytes as u8))
            .collect();
        memory
            .write_at(offset as u64, &input)
            .expect("memory input");
        memory
            .register(&writable_endpoint)
            .expect("register write region");
        let descriptor = memory
            .descriptor(
                offset as u64,
                transfer_bytes as u64,
                BulkDirection::In,
                BULK_RIGHT_READ,
            )
            .expect("write descriptor");
        let buffer_resource = memory.resource_descriptor(descriptor, false);
        let write = block_device::write_request {
            lba: 0,
            block_count: (transfer_bytes / 512) as u64,
            buffer: ResourceSlot::new(0).unwrap(),
            flags: 0,
        };
        let payload = encode(|wire| block_device::encode_write_request(&write, wire));
        let response = invoke_with_bulk(
            transport,
            &writable_endpoint,
            block_device::PROTOCOL_UUID,
            block_device::PROTOCOL_REVISION,
            block_device::METHOD_WRITE,
            &payload,
            std::slice::from_ref(&buffer_resource),
            std::slice::from_ref(&descriptor),
        )
        .await;
        assert_eq!(response.outcome.protocol_error, 0);

        memory
            .register(&writable_endpoint)
            .expect("register read region");
        let descriptor = memory
            .descriptor(
                offset as u64,
                transfer_bytes as u64,
                BulkDirection::Out,
                BULK_RIGHT_WRITE,
            )
            .expect("read descriptor");
        let buffer_resource = memory.resource_descriptor(descriptor, true);
        let read = block_device::read_request {
            lba: 0,
            block_count: (transfer_bytes / 512) as u64,
            buffer: ResourceSlot::new(0).unwrap(),
            flags: 0,
        };
        let payload = encode(|wire| block_device::encode_read_request(&read, wire));
        let response = invoke_with_bulk(
            transport,
            &writable_endpoint,
            block_device::PROTOCOL_UUID,
            block_device::PROTOCOL_REVISION,
            block_device::METHOD_READ,
            &payload,
            std::slice::from_ref(&buffer_resource),
            std::slice::from_ref(&descriptor),
        )
        .await;
        assert_eq!(response.outcome.protocol_error, 0);
        let mut output = vec![0; transfer_bytes];
        memory
            .read_at(offset as u64, &mut output)
            .expect("memory output");
        assert_eq!(output, input);
    }

    // The acceptance daemon uses a separate service directory but the same
    // RAM-backed image.  Stop it before the real service chain starts so its
    // leases cannot make the production formatter observe a busy medium.
    let mut acceptance = children.0.swap_remove(acceptance_child);
    let _ = acceptance.kill();
    let _ = acceptance.wait();
    let _ = std::fs::remove_dir_all(service_root);
}

async fn run_smoke(ramdiskd: PathBuf, exfatd: PathBuf, vfsd: PathBuf, root: PathBuf) {
    let service_root = root.join("services");
    let log_dir = root.join("logs");
    std::fs::create_dir_all(&log_dir).expect("host smoke log dir");
    let services = ServiceDirectory::new(&service_root);
    let transport = UdsTransport::new();
    let block_endpoint = services.endpoint_for(BLOCK_SERVICE_URI).unwrap();
    let exfat_endpoint = services.endpoint_for(EXFATD_SERVICE_URI).unwrap();
    let vfs_endpoint = services.endpoint_for(VFSD_SERVICE_URI).unwrap();

    let mut children = Children(Vec::new());
    block_acceptance(&ramdiskd, &root, &transport, &mut children).await;
    children.0.push(child(
        &ramdiskd,
        &[
            ("--service-dir", service_root.to_string_lossy().into_owned()),
            (
                "--log",
                log_dir.join("ramdiskd.log").to_string_lossy().into_owned(),
            ),
        ],
    ));
    let get_info = encode(|wire| directory::encode_stat_request(&directory::stat_request {}, wire));
    // A factory probe is enough to establish the ramdiskd UDS is live.
    let factory_probe = naos_idl::block_device_factory::get_info_request {};
    let factory_payload = encode(|wire| {
        naos_idl::block_device_factory::encode_get_info_request(&factory_probe, wire)
    });
    wait_for_socket(
        &transport,
        &block_endpoint,
        RpcRequest {
            protocol_uuid: naos_idl::block_device_factory::PROTOCOL_UUID,
            revision: naos_idl::block_device_factory::PROTOCOL_REVISION,
            method_id: naos_idl::block_device_factory::METHOD_GET_INFO,
            request_id: 0,
            target: None,
            payload: &factory_payload,
            resources: &[],
            bulk: &[],
        },
    )
    .await;
    children.0.push(child(
        &exfatd,
        &[
            ("--service-dir", service_root.to_string_lossy().into_owned()),
            (
                "--log",
                log_dir.join("exfatd.log").to_string_lossy().into_owned(),
            ),
            ("--format", String::new()),
        ],
    ));
    wait_for_socket(
        &transport,
        &exfat_endpoint,
        RpcRequest {
            protocol_uuid: directory::PROTOCOL_UUID,
            revision: directory::PROTOCOL_REVISION,
            method_id: directory::METHOD_STAT,
            request_id: 0,
            target: None,
            payload: &get_info,
            resources: &[],
            bulk: &[],
        },
    )
    .await;
    children.0.push(child(
        &vfsd,
        &[
            ("--service-dir", service_root.to_string_lossy().into_owned()),
            (
                "--log",
                log_dir.join("vfsd.log").to_string_lossy().into_owned(),
            ),
        ],
    ));
    wait_for_socket(
        &transport,
        &vfs_endpoint,
        RpcRequest {
            protocol_uuid: directory::PROTOCOL_UUID,
            revision: directory::PROTOCOL_REVISION,
            method_id: directory::METHOD_STAT,
            request_id: 0,
            target: None,
            payload: &get_info,
            resources: &[],
            bulk: &[],
        },
    )
    .await;

    // The Linux locator uses the same stable URI names as NaOS and only
    // changes the endpoint root for this isolated test run.
    assert_eq!(
        block_endpoint.path(),
        service_root.join("block/ramdiskd/0").as_path()
    );
    let mut block_services = vec![Endpoint::new("unused"); 4];
    assert_eq!(
        services
            .list(BLOCK_PREFIX, &mut block_services)
            .await
            .unwrap(),
        1
    );
    assert_eq!(block_services[0].path(), block_endpoint.path());
    let mut filesystem_services = vec![Endpoint::new("unused"); 4];
    assert_eq!(
        services
            .list(FS_PREFIX, &mut filesystem_services)
            .await
            .unwrap(),
        2
    );

    // Exercise revision and resource validation at the process boundary.
    let bad_revision = invoke(
        &transport,
        &vfs_endpoint,
        directory::PROTOCOL_UUID,
        directory::PROTOCOL_REVISION + 1,
        directory::METHOD_STAT,
        &get_info,
        &[],
    )
    .await;
    assert_eq!(bad_revision.outcome.protocol_error, -71);
    let bogus_resource = ResourceDescriptor {
        resource_id: 123,
        binding: 0,
        scope: directory::PROTOCOL_SCOPE,
        rights: u64::MAX,
        ..ResourceDescriptor::default()
    };
    let bad_resources = invoke(
        &transport,
        &vfs_endpoint,
        directory::PROTOCOL_UUID,
        directory::PROTOCOL_REVISION,
        directory::METHOD_STAT,
        &get_info,
        std::slice::from_ref(&bogus_resource),
    )
    .await;
    assert_eq!(bad_resources.outcome.protocol_error, -22);

    // A peer closing without a request must not take down the service.
    drop(std::os::unix::net::UnixStream::connect(vfs_endpoint.path()).unwrap());

    // Exercise the generator-produced transport-aware client helper rather
    // than only the raw envelope API.  The helper supplies UUID, revision and
    // method metadata from the schema itself.
    let stat_request = directory::stat_request {};
    let mut generated_wire = [0_u8; 64];
    let generated_client = directory::Client::new(transport.clone(), vfs_endpoint.clone());
    let generated_response = DirectoryClient::stat(
        &generated_client,
        &stat_request,
        &mut generated_wire,
        &[],
        &[],
        0,
    )
    .await
    .expect("generated transport invocation");
    assert_eq!(generated_response.outcome().protocol_error, 0);

    let create = directory::create_request {
        mode: 0,
        flags: 0,
        path: b"/data/host-smoke.txt",
    };
    let create_payload = encode(|wire| directory::encode_create_request(&create, wire));
    let response = invoke(
        &transport,
        &vfs_endpoint,
        directory::PROTOCOL_UUID,
        directory::PROTOCOL_REVISION,
        directory::METHOD_CREATE,
        &create_payload,
        &[],
    )
    .await;
    assert_eq!(response.outcome.protocol_error, 0);
    let response = invoke(
        &transport,
        &vfs_endpoint,
        directory::PROTOCOL_UUID,
        directory::PROTOCOL_REVISION,
        directory::METHOD_CREATE,
        &create_payload,
        &[],
    )
    .await;
    assert_eq!(response.outcome.protocol_error, -17); // O_EXCL / EEXIST.

    let open = directory::open_request {
        mode: 3,
        flags: 0,
        path: b"/data/host-smoke.txt",
    };
    let open_payload = encode(|wire| directory::encode_open_request(&open, wire));
    let response = invoke(
        &transport,
        &vfs_endpoint,
        directory::PROTOCOL_UUID,
        directory::PROTOCOL_REVISION,
        directory::METHOD_OPEN,
        &open_payload,
        &[],
    )
    .await;
    let open_value = directory::decode_open_response(&response.payload).expect("open response");
    let file_resource = response.resources[open_value.object.index() as usize];
    let file_endpoint = vfs_endpoint.with_target(file_resource);

    // File payloads travel as a MemoryObject region now (revision 5), not as an
    // inline field: the request names the caller's buffer as a capability plus
    // an offset and length, and the response carries only the byte count.
    const PAYLOAD: &[u8] = b"hello linux";
    let file_region = servicekit::memory::MemoryObject::new(PAYLOAD.len()).expect("file region");
    file_region.register(&vfs_endpoint).expect("file region registration");
    file_region
        .write_at(0, PAYLOAD)
        .expect("file region contents");
    let write = file::write_request {
        size: PAYLOAD.len() as u64,
        flags: 0,
        buffer: ResourceSlot::new(0).unwrap(),
    };
    let write_payload = encode(|wire| file::encode_write_request(&write, wire));
    let write_descriptor = file_region
        .descriptor(
            0,
            PAYLOAD.len() as u64,
            BulkDirection::In,
            BULK_RIGHT_READ,
        )
        .expect("file write descriptor");
    let write_region = file_region.resource_descriptor(write_descriptor, false);
    let response = invoke_with_bulk(
        &transport,
        &file_endpoint,
        file::PROTOCOL_UUID,
        file::PROTOCOL_REVISION,
        file::METHOD_WRITE,
        &write_payload,
        &[write_region],
        core::slice::from_ref(&write_descriptor),
    )
    .await;
    assert_eq!(
        file::decode_write_response(&response.payload)
            .unwrap()
            .count,
        PAYLOAD.len() as u64
    );

    let sync_payload = encode(|wire| file::encode_sync_request(&file::sync_request {}, wire));
    let response = invoke(
        &transport,
        &file_endpoint,
        file::PROTOCOL_UUID,
        file::PROTOCOL_REVISION,
        file::METHOD_SYNC,
        &sync_payload,
        &[],
    )
    .await;
    assert_eq!(response.outcome.protocol_error, 0);

    // The read writes into that same region, so the bytes are read back out of
    // the mapping rather than out of the response payload.
    let read = file::pread_request {
        offset: 0,
        size: PAYLOAD.len() as u64,
        flags: 0,
        buffer: ResourceSlot::new(0).unwrap(),
    };
    let read_payload = encode(|wire| file::encode_pread_request(&read, wire));
    let read_descriptor = file_region
        .descriptor(
            0,
            PAYLOAD.len() as u64,
            BulkDirection::Out,
            BULK_RIGHT_WRITE,
        )
        .expect("file read descriptor");
    let read_region = file_region.resource_descriptor(read_descriptor, true);
    let _response = invoke_with_bulk(
        &transport,
        &file_endpoint,
        file::PROTOCOL_UUID,
        file::PROTOCOL_REVISION,
        file::METHOD_PREAD,
        &read_payload,
        &[read_region],
        core::slice::from_ref(&read_descriptor),
    )
    .await;
    let mut read_back = vec![0u8; PAYLOAD.len()];
    file_region.read_at(0, &mut read_back).expect("read region");
    assert_eq!(read_back, PAYLOAD);

    let stat = file::stat_request {};
    let stat_payload = encode(|wire| file::encode_stat_request(&stat, wire));
    let response = invoke(
        &transport,
        &file_endpoint,
        file::PROTOCOL_UUID,
        file::PROTOCOL_REVISION,
        file::METHOD_STAT,
        &stat_payload,
        &[],
    )
    .await;
    assert_eq!(
        file::decode_stat_response(&response.payload)
            .unwrap()
            .value
            .size,
        11
    );

    let rename = directory::rename_request {
        first_size: b"/data/host-smoke.txt".len() as u64,
        second_size: b"/data/host-smoke-renamed".len() as u64,
        first: b"/data/host-smoke.txt",
        second: b"/data/host-smoke-renamed",
    };
    let rename_payload = encode(|wire| directory::encode_rename_request(&rename, wire));
    invoke(
        &transport,
        &vfs_endpoint,
        directory::PROTOCOL_UUID,
        directory::PROTOCOL_REVISION,
        directory::METHOD_RENAME,
        &rename_payload,
        &[],
    )
    .await;

    let truncate = file::truncate_request { length: 5 };
    let truncate_payload = encode(|wire| file::encode_truncate_request(&truncate, wire));
    invoke(
        &transport,
        &file_endpoint,
        file::PROTOCOL_UUID,
        file::PROTOCOL_REVISION,
        file::METHOD_TRUNCATE,
        &truncate_payload,
        &[],
    )
    .await;
    let response = invoke(
        &transport,
        &file_endpoint,
        file::PROTOCOL_UUID,
        file::PROTOCOL_REVISION,
        file::METHOD_STAT,
        &stat_payload,
        &[],
    )
    .await;
    assert_eq!(
        file::decode_stat_response(&response.payload)
            .unwrap()
            .value
            .size,
        5
    );

    let remove = directory::remove_request {
        mode: 0,
        flags: 0,
        path: b"/data/host-smoke-renamed",
    };
    let remove_payload = encode(|wire| directory::encode_remove_request(&remove, wire));
    let response = invoke(
        &transport,
        &vfs_endpoint,
        directory::PROTOCOL_UUID,
        directory::PROTOCOL_REVISION,
        directory::METHOD_REMOVE,
        &remove_payload,
        &[],
    )
    .await;
    assert_eq!(response.outcome.protocol_error, -16); // active file is busy.

    let disposable = directory::create_request {
        mode: 0,
        flags: 0,
        path: b"/data/host-smoke-disposable.txt",
    };
    let disposable_payload = encode(|wire| directory::encode_create_request(&disposable, wire));
    let response = invoke(
        &transport,
        &vfs_endpoint,
        directory::PROTOCOL_UUID,
        directory::PROTOCOL_REVISION,
        directory::METHOD_CREATE,
        &disposable_payload,
        &[],
    )
    .await;
    assert_eq!(response.outcome.protocol_error, 0);
    let disposable_remove = directory::remove_request {
        mode: 0,
        flags: 0,
        path: b"/data/host-smoke-disposable.txt",
    };
    let disposable_remove_payload =
        encode(|wire| directory::encode_remove_request(&disposable_remove, wire));
    let response = invoke(
        &transport,
        &vfs_endpoint,
        directory::PROTOCOL_UUID,
        directory::PROTOCOL_REVISION,
        directory::METHOD_REMOVE,
        &disposable_remove_payload,
        &[],
    )
    .await;
    assert_eq!(response.outcome.protocol_error, 0);
    drop(children);
    for daemon in ["ramdiskd", "exfatd", "vfsd"] {
        let contents = std::fs::read_to_string(log_dir.join(format!("{daemon}.log")))
            .expect("daemon log file");
        assert!(
            contents.contains(&format!("{daemon}: INFO: ready")),
            "missing {daemon} ready log: {contents}"
        );
    }
    println!("host-filesystem-smoke: PASS ramdiskd=ram exfatd=fat vfsd=/data");
}

fn main() {
    let runtime = tokio::runtime::Builder::new_multi_thread()
        .worker_threads(8)
        .enable_all()
        .build()
        .expect("host smoke runtime");
    runtime.block_on(async_main());
}

async fn async_main() {
    let mut args = env::args().skip(1);
    // Optional mode word, then the daemon binaries.  The positional daemon
    // arguments are the original contract (`ramdiskd exfatd vfsd`), so only a
    // recognised mode word is consumed: everything else is a binary path.
    let first = args.next().expect("ramdiskd binary");
    let measure_mode = first == "measure";
    let ramdiskd = if measure_mode {
        PathBuf::from(args.next().expect("ramdiskd binary"))
    } else {
        PathBuf::from(first)
    };
    let exfatd = PathBuf::from(args.next().unwrap_or_default());
    let vfsd = PathBuf::from(args.next().unwrap_or_default());
    let root = std::env::temp_dir().join(format!("naos-host-smoke-{}", std::process::id()));
    std::fs::create_dir_all(&root).expect("host smoke temp dir");
    if measure_mode {
        measure::run(&ramdiskd, &root).await;
    } else {
        run_smoke(ramdiskd, exfatd, vfsd, root.clone()).await;
    }
    let _ = std::fs::remove_dir_all(root);
}

/// W7 measurement harness.
///
/// It measures the *host* transport path (UDS + real daemon processes), which
/// is the platform-independent part of the data plane.  Numbers that only exist
/// in the guest -- kernel epoll scan cost and in-kernel payload copies -- are
/// measured by the NaOS opt-in smoke cases, not here.
mod measure {
    use super::*;
    use std::time::Instant;

    /// One latency sample set for a request/response round trip.
    struct Latency {
        samples: Vec<u64>,
    }

    impl Latency {
        fn new() -> Self {
            Self {
                samples: Vec::new(),
            }
        }

        fn record(&mut self, micros: u64) {
            self.samples.push(micros);
        }

        fn percentile(&self, percent: u32) -> u64 {
            if self.samples.is_empty() {
                return 0;
            }
            let mut sorted = self.samples.clone();
            sorted.sort_unstable();
            let rank = (sorted.len() as u64 * percent as u64).div_ceil(100);
            let index = rank.saturating_sub(1).min(sorted.len() as u64 - 1) as usize;
            sorted[index]
        }

        fn summary(&self) -> String {
            format!(
                "p50={}us p95={}us p99={}us min={}us max={}us n={}",
                self.percentile(50),
                self.percentile(95),
                self.percentile(99),
                self.samples.iter().copied().min().unwrap_or(0),
                self.samples.iter().copied().max().unwrap_or(0),
                self.samples.len()
            )
        }
    }

    /// Context switches this process performed, from `/proc/self/status`.
    ///
    /// The host has no `strace`/`perf`, so a syscall count cannot be taken
    /// directly; context switches are the closest raw counter the kernel
    /// exposes, and the report says plainly which one was measured.
    fn context_switches() -> Option<(u64, u64)> {
        let status = std::fs::read_to_string("/proc/self/status").ok()?;
        let mut voluntary = None;
        let mut involuntary = None;
        for line in status.lines() {
            if let Some(value) = line.strip_prefix("voluntary_ctxt_switches:") {
                voluntary = value.trim().parse::<u64>().ok();
            } else if let Some(value) = line.strip_prefix("nonvoluntary_ctxt_switches:") {
                involuntary = value.trim().parse::<u64>().ok();
            }
        }
        Some((voluntary?, involuntary?))
    }

    const TRANSFER_BYTES: usize = 64 * 1024;

    pub async fn run(ramdiskd: &PathBuf, root: &PathBuf) {
        let service_root = root.join("measure-services");
        let log_path = root.join("logs/ramdiskd-measure.log");
        std::fs::create_dir_all(log_path.parent().expect("log path has a parent"))
            .expect("measure log dir");
        let transport = UdsTransport::new();
        let services = ServiceDirectory::new(&service_root);
        let endpoint = services
            .endpoint_for(BLOCK_SERVICE_URI)
            .expect("valid measure URI");
        let mut children = Children(Vec::new());
        children.0.push(child(
            ramdiskd,
            &[
                ("--service-dir", service_root.to_string_lossy().into_owned()),
                ("--log", log_path.to_string_lossy().into_owned()),
            ],
        ));

        let get_info = encode(|wire| {
            block_device_factory::encode_get_info_request(
                &block_device_factory::get_info_request {},
                wire,
            )
        });
        wait_for_socket(
            &transport,
            &endpoint,
            RpcRequest {
                protocol_uuid: block_device_factory::PROTOCOL_UUID,
                revision: block_device_factory::PROTOCOL_REVISION,
                method_id: block_device_factory::METHOD_GET_INFO,
                request_id: 0,
                target: None,
                payload: &get_info,
                resources: &[],
                bulk: &[],
            },
        )
        .await;

        println!("measure: transport=linux-uds transfer_bytes={TRANSFER_BYTES}");
        let medium = block_device_factory::decode_get_info_response(
            &invoke(
                &transport,
                &endpoint,
                block_device_factory::PROTOCOL_UUID,
                block_device_factory::PROTOCOL_REVISION,
                block_device_factory::METHOD_GET_INFO,
                &get_info,
                &[],
            )
            .await
            .payload,
        )
        .expect("medium info")
        .value;
        println!(
            "measure: medium blocks={} logical={} max_transfer_blocks={} max_transfer_bytes={} max_in_flight={}",
            medium.total_block_count,
            medium.logical_block_bytes,
            medium.max_transfer_blocks,
            medium.max_transfer_bytes,
            medium.max_in_flight
        );
        assert!(
            medium.max_in_flight >= 1,
            "the daemon must advertise a real admission bound"
        );

        // ---- QD=1 control call/reply latency -------------------------------
        let mut latency = Latency::new();
        for _ in 0..500 {
            let start = Instant::now();
            let response = invoke(
                &transport,
                &endpoint,
                block_device_factory::PROTOCOL_UUID,
                block_device_factory::PROTOCOL_REVISION,
                block_device_factory::METHOD_GET_INFO,
                &get_info,
                &[],
            )
            .await;
            assert_eq!(response.outcome.protocol_error, 0);
            latency.record(start.elapsed().as_micros() as u64);
        }
        println!("measure: qd1_control_latency {}", latency.summary());

        // ---- pipelined 64 KiB block reads at several depths ----------------
        let acquire = encode(|wire| {
            block_device_factory::encode_acquire_request(
                &block_device_factory::acquire_request {
                    start_lba: 0,
                    block_count: 256,
                    flags: 0,
                },
                wire,
            )
        });
        let lease = invoke(
            &transport,
            &endpoint,
            block_device_factory::PROTOCOL_UUID,
            block_device_factory::PROTOCOL_REVISION,
            block_device_factory::METHOD_ACQUIRE,
            &acquire,
            &[],
        )
        .await;
        let lease_value = block_device_factory::decode_acquire_response(&lease.payload)
            .expect("lease response");
        let lease_resource = *lease
            .resources
            .get(lease_value.device.index() as usize)
            .expect("lease resource");
        let device = endpoint.with_target(lease_resource);

        let read = block_device::read_request {
            lba: 0,
            block_count: TRANSFER_BYTES as u64 / 512,
            buffer: ResourceSlot::new(0).unwrap(),
            flags: 0,
        };
        let read_payload = encode(|wire| block_device::encode_read_request(&read, wire));
        let write = block_device::write_request {
            lba: 0,
            block_count: TRANSFER_BYTES as u64 / 512,
            buffer: ResourceSlot::new(0).unwrap(),
            flags: 0,
        };
        let write_payload = encode(|wire| block_device::encode_write_request(&write, wire));

        // The device advertises the depth it will admit, so a well-behaved
        // client never exceeds it: probing deeper would measure the refusal
        // path (EAGAIN), not the data plane.  The advertised depth itself is
        // always probed -- otherwise a shallow limit would leave the sweep
        // measuring only the serial case and hide the pipeline it enables.
        let max_depth = usize::try_from(medium.max_in_flight).unwrap_or(1).max(1);
        println!("measure: advertised_max_in_flight={max_depth}");
        let depths = {
            let mut depths = std::vec![1usize];
            for candidate in [max_depth / 2, max_depth, 8, 32, 128] {
                if candidate > 1 && candidate <= max_depth && !depths.contains(&candidate) {
                    depths.push(candidate);
                }
            }
            depths
        };
        for depth in depths.iter().copied() {
            let memory = MemoryObject::new(TRANSFER_BYTES).expect("transfer region");
            let descriptor = memory
                .descriptor(0, TRANSFER_BYTES as u64, BulkDirection::Out, BULK_RIGHT_WRITE)
                .expect("bulk descriptor");
            // A service read writes the caller's buffer, so the caller grants
            // WRITE; `resource_descriptor` builds exactly that right set.
            let resource = memory.resource_descriptor(descriptor, true);
            let iterations = 256usize.max(depth * 8);
            let (before_v, before_i) = context_switches().unwrap_or((0, 0));
            let start = Instant::now();
            let mut issued = 0usize;
            while issued < iterations {
                let batch = depth.min(iterations - issued);
                // The Linux transport transfers the region once per in-flight
                // call and the peer releases it with the response, so a batch
                // of D needs D registrations; that is the transport's real
                // contract, not a harness shortcut.
                for _ in 0..batch {
                    memory.register(&device).expect("region registration");
                }
                // The batch must be *in flight* together: building futures and
                // awaiting them one at a time would measure a serial run and
                // make every depth look identical.  Spawn them so the runtime
                // overlaps the round trips, which is what depth means.
                let mut pending = tokio::task::JoinSet::new();
                for _ in 0..batch {
                    let transport = transport.clone();
                    let device = device.clone();
                    let payload = read_payload.clone();
                    let resource = resource;
                    let descriptor = descriptor;
                    pending.spawn(async move {
                        invoke_with_bulk(
                            &transport,
                            &device,
                            block_device::PROTOCOL_UUID,
                            block_device::PROTOCOL_REVISION,
                            block_device::METHOD_READ,
                            &payload,
                            std::slice::from_ref(&resource),
                            std::slice::from_ref(&descriptor),
                        )
                        .await
                    });
                }
                while let Some(joined) = pending.join_next().await {
                    let response = joined.expect("batch task did not panic");
                    assert_eq!(response.outcome.protocol_error, 0);
                }
                issued += batch;
            }
            let elapsed = start.elapsed();
            let (after_v, after_i) = context_switches().unwrap_or((0, 0));
            let per_request = elapsed.as_micros() as f64 / iterations as f64;
            let throughput = (iterations as f64 * TRANSFER_BYTES as f64)
                / elapsed.as_secs_f64()
                / (1024.0 * 1024.0);
            let switches = (after_v - before_v) + (after_i - before_i);
            println!(
                "measure: qd={depth} requests={iterations} total={}us per_request={per_request:.1}us throughput={throughput:.1}MiB/s ctxt_switches={switches} ctxt_per_request={:.2}",
                elapsed.as_micros(),
                switches as f64 / iterations as f64
            );
            drop(memory);
        }

        // ---- segmented 64 KiB read and write ------------------------------
        for (label, method, payload, direction, rights) in [
            (
                "read",
                block_device::METHOD_READ,
                &read_payload,
                BulkDirection::Out,
                BULK_RIGHT_WRITE,
            ),
            (
                "write",
                block_device::METHOD_WRITE,
                &write_payload,
                BulkDirection::In,
                BULK_RIGHT_READ,
            ),
        ] {
            let iterations = 128usize;
            let mut create = 0u64;
            let mut fill = 0u64;
            let mut register = 0u64;
            let mut submit = 0u64;
            let mut drain = 0u64;
            for _ in 0..iterations {
                let start = Instant::now();
                let memory = MemoryObject::new(TRANSFER_BYTES).expect("segment region");
                create += start.elapsed().as_micros() as u64;

                let start = Instant::now();
                if direction == BulkDirection::In {
                    let sample = std::vec![0x5a; TRANSFER_BYTES];
                    memory.write_at(0, &sample).expect("fill region");
                }
                fill += start.elapsed().as_micros() as u64;

                let start = Instant::now();
                memory.register(&device).expect("region registration");
                let descriptor = memory
                    .descriptor(0, TRANSFER_BYTES as u64, direction, rights)
                    .expect("bulk descriptor");
                register += start.elapsed().as_micros() as u64;
                let resource = memory.resource_descriptor(descriptor, direction == BulkDirection::Out);

                let response = invoke_with_bulk(
                    &transport,
                    &device,
                    block_device::PROTOCOL_UUID,
                    block_device::PROTOCOL_REVISION,
                    method,
                    payload,
                    std::slice::from_ref(&resource),
                    std::slice::from_ref(&descriptor),
                )
                .await;
                submit += start.elapsed().as_micros() as u64;
                assert_eq!(response.outcome.protocol_error, 0);

                let start = Instant::now();
                if direction == BulkDirection::Out {
                    let mut out = std::vec![0u8; TRANSFER_BYTES];
                    memory.read_at(0, &mut out).expect("drain region");
                }
                drain += start.elapsed().as_micros() as u64;
            }
            println!(
                "measure: segments_{label}_64k per_call: create={}us fill={}us register={}us submit={}us drain={}us",
                create / iterations as u64,
                fill / iterations as u64,
                register / iterations as u64,
                submit / iterations as u64,
                drain / iterations as u64
            );
        }

        // ---- control-path QD sweep -----------------------------------------
        // The control call has no bulk region and no registration, so it
        // isolates the accept/read/dispatch/write path from the region
        // transfer.  If this sweep is flat, the serializer is the server's
        // request intake; if it scales, the serializer is the region path.
        for depth in [1usize, 8, 32] {
            let iterations = 256usize.max(depth * 8);
            let start = Instant::now();
            let mut issued = 0usize;
            while issued < iterations {
                let batch = depth.min(iterations - issued);
                let mut pending = tokio::task::JoinSet::new();
                for _ in 0..batch {
                    let transport = transport.clone();
                    let endpoint = endpoint.clone();
                    let payload = get_info.clone();
                    pending.spawn(async move {
                        invoke(
                            &transport,
                            &endpoint,
                            block_device_factory::PROTOCOL_UUID,
                            block_device_factory::PROTOCOL_REVISION,
                            block_device_factory::METHOD_GET_INFO,
                            &payload,
                            &[],
                        )
                        .await
                    });
                }
                while let Some(joined) = pending.join_next().await {
                    let response = joined.expect("batch task did not panic");
                    assert_eq!(response.outcome.protocol_error, 0);
                }
                issued += batch;
            }
            let per_request = start.elapsed().as_micros() as f64 / iterations as f64;
            println!(
                "measure: control_qd={depth} per_request={per_request:.1}us requests_per_s={:.0}",
                iterations as f64 / start.elapsed().as_secs_f64()
            );
        }

        // ---- write QD sweep -------------------------------------------------
        // Writes are dispatched on their own tasks and apply in admission order
        // through the ordering ledger, so this is the sweep that shows whether
        // overlapping writes buy anything over a serial dispatcher.  The region
        // is filled once and reused: the measurement is about the write path,
        // not about re-preparing the payload.
        for depth in depths.clone() {
            let memory = MemoryObject::new(TRANSFER_BYTES).expect("write region");
            memory
                .write_at(0, &std::vec![0x5a; TRANSFER_BYTES])
                .expect("write region contents");
            let descriptor = memory
                .descriptor(0, TRANSFER_BYTES as u64, BulkDirection::In, BULK_RIGHT_READ)
                .expect("write descriptor");
            let resource = memory.resource_descriptor(descriptor, false);
            let iterations = 256usize.max(depth * 8);
            let start = Instant::now();
            let mut issued = 0usize;
            while issued < iterations {
                let batch = depth.min(iterations - issued);
                for _ in 0..batch {
                    memory.register(&device).expect("write registration");
                }
                let mut pending = tokio::task::JoinSet::new();
                for _ in 0..batch {
                    let transport = transport.clone();
                    let device = device.clone();
                    let payload = write_payload.clone();
                    let resource = resource;
                    let descriptor = descriptor;
                    pending.spawn(async move {
                        invoke_with_bulk(
                            &transport,
                            &device,
                            block_device::PROTOCOL_UUID,
                            block_device::PROTOCOL_REVISION,
                            block_device::METHOD_WRITE,
                            &payload,
                            std::slice::from_ref(&resource),
                            std::slice::from_ref(&descriptor),
                        )
                        .await
                    });
                }
                while let Some(joined) = pending.join_next().await {
                    let response = joined.expect("batch task did not panic");
                    assert_eq!(response.outcome.protocol_error, 0);
                }
                issued += batch;
            }
            let elapsed = start.elapsed();
            let per_request = elapsed.as_micros() as f64 / iterations as f64;
            let throughput =
                (iterations as f64 * TRANSFER_BYTES as f64) / elapsed.as_secs_f64() / (1024.0 * 1024.0);
            println!(
                "measure: write_qd={depth} requests={iterations} per_request={per_request:.1}us throughput={throughput:.1}MiB/s"
            );
            drop(memory);
        }

        // ---- size sweep: is cost dominated by payload size or by the call? --
        for transfer in [4 * 1024usize, 64 * 1024] {
            let region = MemoryObject::new(transfer).expect("sweep region");
            let descriptor = region
                .descriptor(0, transfer as u64, BulkDirection::Out, BULK_RIGHT_WRITE)
                .expect("sweep descriptor");
            let resource = region.resource_descriptor(descriptor, true);
            let read = block_device::read_request {
                lba: 0,
                block_count: transfer as u64 / 512,
                buffer: ResourceSlot::new(0).unwrap(),
                flags: 0,
            };
            let payload = encode(|wire| block_device::encode_read_request(&read, wire));
            let iterations = 128usize;
            let start = Instant::now();
            for _ in 0..iterations {
                region.register(&device).expect("sweep registration");
                let response = invoke_with_bulk(
                    &transport,
                    &device,
                    block_device::PROTOCOL_UUID,
                    block_device::PROTOCOL_REVISION,
                    block_device::METHOD_READ,
                    &payload,
                    std::slice::from_ref(&resource),
                    std::slice::from_ref(&descriptor),
                )
                .await;
                assert_eq!(response.outcome.protocol_error, 0);
            }
            let per_request = start.elapsed().as_micros() as f64 / iterations as f64;
            println!(
                "measure: size={transfer}B per_request={per_request:.1}us payload_MiB_per_s={:.1}",
                (iterations as f64 * transfer as f64) / start.elapsed().as_secs_f64() / (1024.0 * 1024.0)
            );
        }

        println!(
            "measure: note syscall counts are unavailable on this host (no strace/perf); context switches are reported per request instead"
        );
        println!(
            "measure: note these numbers cover the Linux UDS transport; in-kernel payload copies and page-table work are only observable in the guest"
        );
        println!("measure: PASS");
    }
}
