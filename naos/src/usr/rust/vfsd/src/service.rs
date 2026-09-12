//! Native VFS mount manager.
//!
//! vfsd owns only the system namespace and mount topology. The root
//! filesystem data plane is provided by the independently bootstrapped
//! rootfsd worker through blockd; no archive or local filesystem fallback is
//! part of the startup path.

use servicekit::Context;

#[cfg(target_os = "linux")]
#[path = "host.rs"]
mod host;

#[cfg(target_os = "naos")]
use servicekit::boot;

#[cfg(target_os = "naos")]
use naos_idl::vfs;
#[cfg(target_os = "naos")]
use servicekit::publish_listener;
#[cfg(target_os = "naos")]
use servicekit::server::EndpointResource;
#[cfg(target_os = "naos")]
use vfsd::backend::RamFs;
#[cfg(target_os = "naos")]
use vfsd::server::VfsServer;

#[cfg(target_os = "naos")]
const FS_BYTE_BUDGET: u64 = 64 << 20;
#[cfg(target_os = "naos")]
const MOUNT_PREFIX: &[u8] = b"/data";

#[cfg(target_os = "linux")]
pub async fn run(context: Context) -> i64 {
    host::run(context).await
}

#[cfg(target_os = "naos")]
pub async fn run(context: Context) -> i64 {
    let mut fs = RamFs::new(FS_BYTE_BUDGET);
    let root = fs.root();
    if let Err(error) = fs.mkdir_p(root, MOUNT_PREFIX) {
        log::error!("root mountpoint creation failed: errno={error:?}");
        return 1;
    }
    let mut vfs_server = VfsServer::new_external(fs);
    let admin_listener = match publish_listener(
        context.service_directory_handle(),
        servicekit::uri::FS_VFS_ADMIN,
        &vfs::protocol_descriptor(),
    ) {
        Ok(listener) => listener,
        Err(status) => {
            log::error!("publish vfs mount authority failed: {status}");
            return 1;
        }
    };
    log::info!(
        "mount authority ready service={}",
        servicekit::uri::FS_VFS_ADMIN
    );

    // The kernel has published these as one-shot MemoryObject resources, but
    // deliberately did not create either child. vfsd is the boot manager:
    // consume rootfsd only after the mount authority exists, then consume
    // init only after the worker has committed the root route.
    let rootfsd = match boot::read_module(&context, servicekit::uri::BOOT_MODULE_ROOTFSD) {
        Ok(image) => image,
        Err(error) => {
            log::error!("rootfsd boot module unavailable: {error:?}");
            return 1;
        }
    };
    let init = match boot::read_module(&context, servicekit::uri::BOOT_MODULE_INIT) {
        Ok(image) => image,
        Err(error) => {
            log::error!("init boot module unavailable: {error:?}");
            return 1;
        }
    };
    match boot::spawn_early_service(&context, &rootfsd, "/bin/exfatd") {
        Ok(pid) => log::info!("rootfsd worker bootstrap requested pid={pid}"),
        Err(error) => {
            log::error!("rootfsd worker bootstrap failed: {error:?}");
            return 1;
        }
    }

    let mut request_wire = vec![0_u8; 65_536];
    let mut reply_wire = vec![0_u8; 65_536];
    let admin_listeners = [(admin_listener.get(), true)];
    while !vfs_server.root_route_ready() {
        let result = vfs_server
            .serve_once_async_with_listeners(&admin_listeners, &mut request_wire, &mut reply_wire)
            .await;
        if let Err(status) = result {
            if status != servicekit::sys::STATUS_WAIT_TIMED_OUT
                && status != servicekit::sys::STATUS_WOULD_BLOCK
            {
                log::error!("vfs mount manager stopped before root commit: {status}");
                return 1;
            }
        }
        tokio::task::yield_now().await;
    }

    // This is the only publication point for the root Directory route in the
    // mounted-root boot. init cannot obtain a placeholder namespace before
    // the worker bind handshake has completed.
    let data_listener = match publish_listener(
        context.service_directory_handle(),
        servicekit::uri::FS_VFS,
        &naos_idl::directory::protocol_descriptor(),
    ) {
        Ok(listener) => listener,
        Err(status) => {
            log::error!("publish committed vfs root route failed: {status}");
            return 1;
        }
    };
    log::info!("root route ready service={}", servicekit::uri::FS_VFS);
    log::info!("ready service={}", servicekit::uri::FS_VFS);

    let root = vfs_server.backend().root();
    let root_handle = match vfs_server.new_directory_binding(root, root) {
        Ok(handle) => handle,
        Err(status) => {
            log::error!("root endpoint creation failed: {status}");
            return 1;
        }
    };
    let cwd_handle = match vfs_server.new_directory_binding(root, root) {
        Ok(handle) => handle,
        Err(status) => {
            log::error!("cwd endpoint creation failed: {status}");
            return 1;
        }
    };
    let mut root_endpoint = match EndpointResource::from_owned(
        root_handle,
        &naos_idl::directory::protocol_descriptor(),
    ) {
        Ok(endpoint) => endpoint,
        Err(error) => {
            log::error!("root endpoint descriptor failed: {error:?}");
            return 1;
        }
    };
    let mut cwd_endpoint = match EndpointResource::from_owned(
        cwd_handle,
        &naos_idl::directory::protocol_descriptor(),
    ) {
        Ok(endpoint) => endpoint,
        Err(error) => {
            log::error!("cwd endpoint descriptor failed: {error:?}");
            return 1;
        }
    };
    match boot::spawn_init(&context, &init, &mut root_endpoint, &mut cwd_endpoint) {
        Ok(pid) => log::info!("init spawned pid={pid}"),
        Err(error) => {
            log::error!("init spawn failed: {error:?}");
            return 1;
        }
    }

    let listeners = [(data_listener.get(), false), (admin_listener.get(), true)];
    loop {
        let result = vfs_server
            .serve_once_async_with_listeners(&listeners, &mut request_wire, &mut reply_wire)
            .await;
        if let Err(status) = result {
            if status != servicekit::sys::STATUS_WAIT_TIMED_OUT
                && status != servicekit::sys::STATUS_WOULD_BLOCK
            {
                log::error!("vfs service loop stopped: {status}");
                return 1;
            }
        }
        tokio::task::yield_now().await;
    }
}
