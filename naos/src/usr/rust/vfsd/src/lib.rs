//! vfsd: the NaOS early-system file service (USERSPACE_FILESYSTEM_ADR Phase 2).
//!
//! The service uses the pinned NaOS `std` through the platform-neutral
//! `servicekit` facade. It must never call a hosted `std::fs`/`std::path`
//! surface, because those routes become self-invocations
//! through `naos://service/fs/vfs/0` once the File/Directory protocol server
//! lands in this same binary.
//! The native NaOS binary feature additionally links the pinned NaOS `std` and
//! Tokio runtime without changing the backend protocol implementation.
//!
//! Host-runnable pieces live here (`cargo test -p vfsd`):
//! - [`backend`]: RAM namespace fixture with POSIX error semantics.

extern crate alloc;

pub mod backend;
pub mod errno;
pub mod internal;
pub mod mobj;
pub mod mount;
pub mod mount_admin;
pub mod server;
