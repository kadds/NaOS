//! POSIX errno values used by the FAT worker.
//!
//! Numeric values match the Linux/x86-64 ABI (same mapping as the kernel's
//! `abi.h` and mlibc), so the File/Directory protocol server can surface
//! them as negative protocol errors without translation. Mirrors the
//! `Errno` vocabulary already established in `vfsd`.

/// POSIX error numbers (Linux/x86-64 ABI values).
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(i32)]
pub enum Errno {
    /// No such file or directory.
    ENoent = 2,
    /// I/O error.
    EIo = 5,
    /// Bad file descriptor.
    EBadf = 9,
    /// Permission denied.
    EAccess = 13,
    /// Resource or mount is busy.
    EBusy = 16,
    /// File exists.
    EExist = 17,
    /// Cross-device link — namespace layer only; this worker never sees a
    /// cross-mount rename/link because `vfsd` rejects it with `EXDEV`
    /// before dispatching to the backend (ADR appendix A rule table).
    EXdev = 18,
    /// Device or worker endpoint disappeared while routing a request.
    ENodev = 19,
    /// Not a directory.
    ENotDir = 20,
    /// Is a directory.
    EIsDir = 21,
    /// Invalid argument.
    EInval = 22,
    /// File too large for a MemoryObject snapshot.
    EFbig = 27,
    /// No space left on device (FAT full / cluster chain exhausted).
    ENospc = 28,
    /// Read-only filesystem (read-only lease held on the LBD).
    ERofs = 30,
    /// Filename too long (exceeds FAT LFN limits).
    ENameTooLong = 36,
    /// Directory not empty.
    ENotEmpty = 39,
    /// Operation not supported — hard link, symlink/readlink, chmod/chown
    /// per ADR appendix A; must be deterministic and contract-tested.
    EOpNotSupp = 95,
}

impl Errno {
    pub fn to_i32(self) -> i32 {
        self as i32
    }
}

/// A filesystem operation failure carrying the POSIX errno the protocol
/// layer must report.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct FsError {
    pub errno: Errno,
}

impl FsError {
    pub fn new(errno: Errno) -> Self {
        Self { errno }
    }
}
