//! exfatd: the NaOS FAT-family filesystem worker (VFS ADR §13, appendix A).
//!
//! The first persistent format worker serves the FAT-family data-plane column
//! of the ADR appendix A matrix on top of a channel-backed `BlockDevice`
//! client. The standalone `exfatd` binary resolves the block factory
//! through `servicekit` and acquires its own LBD. The service uses the pinned
//! NaOS `std` and Tokio runtime through the platform-neutral `servicekit`
//! facade around the same worker and protocol code.
//!
//! Layering (bottom-up):
//! - [`block`]: the integration-point trait [`block::BlockClient`], plus an
//!   in-memory and host file-backed reference device used by tests.
//! - the standalone service's `RemoteBlockIo`: the servicekit client adapter
//!   used by the production process.
//! - [`volume`]: sector-aligned read/write adapter mapping byte-granular
//!   filesystem I/O onto block-client reads/writes, with flush→FUA
//!   mapping and read-only lease enforcement.
//! - [`worker`]: the POSIX-matrix wrapper (`FatWorker`) implementing the
//!   appendix A rows for lookup/read/write/directory cursor/mkdir/rmdir/
//!   unlink/rename and deterministically returning `EOPNOTSUPP` for
//!   hard link/symlink/chmod/chown.
//! - [`errno`]: POSIX errno values shared with the protocol layer.
//!
//! Host-runnable pieces live here (`cargo test -p exfatd`).

extern crate alloc;

pub mod block;
pub mod core;
pub mod errno;
pub mod mount_control;
pub(crate) mod namespace_binding;
pub mod volume;
pub mod worker;
