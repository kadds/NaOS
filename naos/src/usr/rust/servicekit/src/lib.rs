//! `servicekit`: shared `std` + Tokio utilities for NaOS services. Platform
//! selection is derived from the compilation target: Linux gets the UDS
//! adapter and NaOS gets the capability-handle runtime facade.
//!
//! Everything here is the boring plumbing every boot service needs before it
//! can do its real job:
//!
//! - [`tidy_log`]: init-once logging over the bootstrap stdout Stream client,
//!   with a seam trait ([`tidy_log::StreamOps`]) so host tests fake the kernel.
//! - [`park`]: yield-based terminal park loop.
//!
//! `tidy_log::init` installs the process-global `log` facade and, for NaOS
//! services, mirrors records through both bootstrap stdout and `_s_log`.
//!
//! Host-runnable pieces (`cargo test -p servicekit`): log formatting/chunking
//! and hex debug rendering.

extern crate alloc;

pub mod admission;
pub mod boot;
pub mod client;
#[cfg(target_os = "linux")]
mod linux;
#[cfg(target_os = "linux")]
// The low-level channel wrapper is retained only by its C-ABI tests.  Service
// implementations use the bounded Server/Client facade, so no arbitrary raw
// resource-id endpoint type is part of the production servicekit API.
#[cfg(test)]
mod linux_ipc;
pub mod memory;
#[cfg(target_os = "naos")]
mod naos;
pub mod park;
pub mod server;
pub mod tidy_log;
pub mod uri;

/// Platform-neutral readiness returned by servicekit's event adapter.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ReadyEvent {
    pub index: usize,
    pub readable: bool,
    pub writable: bool,
    pub read_closed: bool,
    pub write_closed: bool,
    pub error: bool,
}

/// Platform-neutral service transport façade.  The Linux implementation is
/// private; consumers use these endpoint, directory, memory, and serving
/// interfaces without importing a platform module.
#[cfg(target_os = "linux")]
pub mod transport {
    pub use super::linux::{
        BlockingTransport, Endpoint, ServiceDirectory, ServiceDirectoryError, ServiceSession,
        TransportError, UdsTransport, init_log,
    };
}

#[cfg(target_os = "linux")]
pub use linux::{
    Context, ReadinessSet, monotonic_ticks, run, wait_for_completion, wait_ready, wait_ready_async,
};

/// Re-export so the [`panic`] macro works from downstream binaries without
/// adding their own `naos-sys` dependency spelling.
pub use naos_sys as sys;

#[cfg(target_os = "naos")]
pub use naos_runtime;

/// Target-specific runtime details stay private behind the common servicekit
/// API. A daemon should depend on `servicekit::run` and `servicekit::Channel`,
/// never on a platform namespace or raw runtime bootstrap.
#[cfg(target_os = "naos")]
pub use naos::{
    Channel, Context, accept_listener, connect_until, list_service_uris, monotonic_ticks,
    publish_listener, register_service, resolve_resource, resolve_service, resolve_until, run,
    ReadinessSet, take_root_and_current, wait_for_completion, wait_for_service_prefix, wait_ready,
    wait_ready_async,
};
