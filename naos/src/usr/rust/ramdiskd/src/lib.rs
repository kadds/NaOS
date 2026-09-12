//! ramdiskd: the in-memory NaOS BlockDevice backend (VFS ADR §4, §7 item 1).
//!
//! The service uses the pinned NaOS `std` and Tokio runtime through the
//! platform-neutral `servicekit` facade; the binary owns its ramdisk and consumes only the ordinary
//! early-service bootstrap.
//! The format-specific worker runs in the independent `exfatd` process.
//!
//! Host-runnable pieces live here (`cargo test -p ramdiskd`):
//! - [`lease`]: pure LBD lease-range arithmetic and protocol flag constants.
//! - [`ordering`]: the admission-sequence ledger that makes `flush` observe
//!   every `write` admitted before it.

extern crate alloc;

pub mod core;
pub mod lease;
pub mod ordering;
