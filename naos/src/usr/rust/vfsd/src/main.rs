//! `vfsd`: the NaOS early-system file service entry point
//! (USERSPACE_FILESYSTEM_ADR §5.1-§5.5).
//!
//! Startup sequence:
//! 1. Validate the early-service bootstrap (ServiceDirectory + stdio; no
//!    root/cwd or service-specific resource capability on this path).
//! 2. Publish the mount authority on the kernel ServiceDirectory.
//! 3. Resolve the kernel-published `rootfsd`, `init`, and `rootimage`
//!    MemoryObjects through the ServiceDirectory.
//! 4. Start the rootfsd worker with the common early-service
//!    bootstrap. The worker discovers block and mount authorities through
//!    ServiceDirectory instead of receiving service-specific capabilities.
//! 5. Publish the committed root route, then spawn `/bin/init` over Process.spawn
//!    and hand it root/cwd Directory
//!    endpoints served by this process, an attenuated ServiceDirectory alias
//!    and duplicated stdio (v4 bootstrap message).
//! 6. Serve File/Directory protocol endpoints forever through the Tokio
//!    readiness selector.
//!
//! Every failure writes a distinct boot error to the diagnostic stream and
//! exits nonzero — never a silent fallback.

mod service;

#[tokio::main]
async fn main() -> std::process::ExitCode {
    std::process::ExitCode::from(servicekit::run("vfsd", service::run).await as u8)
}
