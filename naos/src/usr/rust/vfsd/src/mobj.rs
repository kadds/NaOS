//! MemoryObject materialization boundary for vfsd.
//!
//! The implementation lives in servicekit so all services use the same
//! direct data-plane contract on NaOS and the same platform abstraction on
//! Linux.

use naos_idl::OwnedHandle;

use crate::errno::Errno;

pub fn create_and_fill_read_only(bytes: &[u8]) -> Result<OwnedHandle, Errno> {
    servicekit::memory::create_and_fill_read_only(bytes).map_err(|error| match error {
        servicekit::memory::MemoryError::InvalidArgument => Errno::EInval,
        servicekit::memory::MemoryError::AccessDenied => Errno::EAccess,
        servicekit::memory::MemoryError::Unsupported => Errno::EInval,
        servicekit::memory::MemoryError::Io => Errno::EIo,
    })
}
