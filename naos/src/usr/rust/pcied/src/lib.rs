extern crate alloc;

#[cfg(target_os = "naos")]
mod block_catalog;
mod driver_binding;
mod topology;
#[cfg(target_os = "naos")]
pub mod virtio_dma;
pub mod virtio_pci;
pub mod virtio_queue;
#[cfg(target_os = "naos")]
mod virtio_transport;

#[cfg(target_os = "naos")]
mod service;
#[cfg(target_os = "naos")]
mod virtio_blk;

#[cfg(target_os = "naos")]
pub async fn run_manager(context: servicekit::Context) -> i64 {
    service::run(context).await
}

#[cfg(target_os = "naos")]
pub async fn run_virtio_blk_driver(context: servicekit::Context) -> i64 {
    virtio_blk::run(context).await
}

#[cfg(all(target_os = "naos", feature = "pci-msi-route-smoke"))]
pub async fn run_driver(context: servicekit::Context) -> i64 {
    service::run_driver(context).await
}
