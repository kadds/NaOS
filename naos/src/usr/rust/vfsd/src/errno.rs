//! POSIX errno values used by the RAM backend.
//!
//! The numeric values match the kernel's `abi.h` / mlibc mapping so the
//! File/Directory protocol server (next slice) can surface them as negative
//! protocol errors without translation.

#![allow(dead_code)]

/// POSIX error numbers (Linux/x86-64 ABI values).
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(i32)]
pub enum Errno {
    /// Operation not permitted.
    Eperm = 1,
    /// No such file or directory.
    ENoent = 2,
    /// I/O error.
    EIo = 5,
    /// No such device (worker/control endpoint unavailable).
    ENodev = 19,
    /// Bad file descriptor.
    EBadf = 9,
    /// Out of memory.
    ENomem = 12,
    /// Permission denied.
    EAccess = 13,
    /// File exists.
    EExist = 17,
    /// Device or resource busy (mount/mutation reservation conflicts).
    EBusy = 16,
    /// Cross-device link (namespace layer: across mount instances).
    EXdev = 18,
    /// Not a directory.
    ENotDir = 20,
    /// Is a directory.
    EIsDir = 21,
    /// Invalid argument.
    EInval = 22,
    /// Too many open files (open-description table exhausted).
    EMfile = 24,
    /// File too large.
    EFbig = 27,
    /// No space left on device (RAM budget exhausted).
    ENospc = 28,
    /// Illegal seek.
    ESpipe = 29,
    /// Too many levels of symbolic links.
    ELoop = 40,
    /// Filename too long.
    ENameTooLong = 36,
    /// Directory not empty.
    ENotEmpty = 39,
    /// Value too large for defined data type.
    EOverflow = 75,
}

impl Errno {
    pub fn to_i32(self) -> i32 {
        self as i32
    }
}
