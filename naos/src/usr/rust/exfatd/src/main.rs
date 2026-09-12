//! `exfatd`: an independent FAT-family filesystem worker process.
//!
//! The process resolves a block-device factory through ServiceDirectory,
//! acquires its own LBD, and runs the FAT worker lifecycle that was previously
//! embedded in ramdiskd. Keeping the worker in a separate process makes the
//! block-service boundary observable at the actual userland boundary.

mod service;

#[tokio::main]
async fn main() -> std::process::ExitCode {
    std::process::ExitCode::from(servicekit::run("exfatd", service::run).await as u8)
}
