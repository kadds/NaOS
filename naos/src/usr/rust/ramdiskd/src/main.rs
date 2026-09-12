//! `ramdiskd`: the userland block manager for the fixed NaOS ramdisk.
//!
//! This process owns the ramdisk backing store and the public
//! BlockDeviceFactory/BlockDevice endpoints, LBA range allocation, lease
//! lifetime, and the policy that a filesystem worker sees.

mod service;

#[tokio::main]
async fn main() -> std::process::ExitCode {
    std::process::ExitCode::from(servicekit::run("ramdiskd", service::run).await as u8)
}
