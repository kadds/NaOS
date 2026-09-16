#![allow(unexpected_cfgs)]

//! Platform-neutral MemoryObject data-plane interface.
//!
//! NaOS implements this with a kernel-created capability that is mapped into
//! the service address space. Linux implements the same service-facing object
//! with a private `memfd` backend; the fd is transferred through servicekit's
//! private `SCM_RIGHTS` registration path. A daemon never sees `BulkRegion`.

use naos_idl::OwnedHandle;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum MemoryError {
    InvalidArgument,
    AccessDenied,
    Io,
    Unsupported,
}

/// NaOS shared mappings are page-backed. Callers that retain a mapping across
/// an IPC transfer must allocate a page-aligned window so the kernel can map
/// the same backing frames into both address spaces.
pub const MEMORY_PAGE_SIZE: usize = 4096;

pub fn page_aligned_size(bytes: usize) -> Result<usize, MemoryError> {
    let bytes = bytes.max(1);
    bytes
        .checked_add(MEMORY_PAGE_SIZE - 1)
        .map(|value| value & !(MEMORY_PAGE_SIZE - 1))
        .ok_or(MemoryError::InvalidArgument)
}

/// Direction of a MemoryObject transfer from the client to the service.
///
/// The transport adapters translate this into a NaOS resource transfer or
/// the private Linux memfd path.  Daemons must not depend on either wire
/// representation.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum MemoryDirection {
    In,
    Out,
    InOut,
}

/// Rights a transferred region must carry for one direction.
///
/// `NaOS` expresses bulk as a capability transfer, so these bits are the
/// resource's `MEMORY_RIGHT_*` set; Linux maps them onto the same descriptor
/// bits so both platforms admit or reject the same request.  A service write
/// reads the caller's buffer and therefore needs READ; a service read writes
/// it and needs WRITE.
pub fn direction_rights(direction: MemoryDirection) -> u64 {
    let (read, write) = (
        crate::sys::MEMORY_RIGHT_READ,
        crate::sys::MEMORY_RIGHT_WRITE,
    );
    match direction {
        MemoryDirection::In => read,
        MemoryDirection::Out => write,
        MemoryDirection::InOut => read | write,
    }
}

/// Rights the region must hold beyond the direction right: the service needs
/// to install a mapping, and the region itself must be transferable.
pub fn direction_map_rights() -> u64 {
    crate::sys::MEMORY_RIGHT_MAP | crate::sys::RIGHT_TRANSFER
}

/// Direction a transport observed for a region request.
///
/// NaOS carries bulk as a capability, which has no direction field, so a NaOS
/// request passes `None` and the granted rights express the same intent: a
/// caller that granted only WRITE cannot ask a service to read its buffer.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum RegionDirection {
    In,
    Out,
    InOut,
}

/// Why a region request was refused.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum RegionRejection {
    /// The request does not have the shape of a single region transfer.
    Protocol,
    /// The caller did not grant the rights this operation needs.
    AccessDenied,
}

/// Single admission point for a service-side region use.
///
/// Both transports call this, so the same semantic request is admitted or
/// refused identically on NaOS and Linux: `service_writes_caller_buffer`
/// selects the required right (a service write reads the caller's buffer and
/// therefore needs READ, and the reverse for a service read), and
/// `observed_direction` adds the wire's own direction check where the
/// transport can express one.  Region identity is not part of this decision:
/// Linux pairs the descriptor's region id with the resource id, while NaOS
/// reads the kernel capability table directly.
pub fn admit_service_region(
    service_writes_caller_buffer: bool,
    binding: u32,
    scope: u64,
    rights: u64,
    observed_direction: Option<RegionDirection>,
) -> Result<(), RegionRejection> {
    if binding != crate::sys::BINDING_MEMORY_OBJECT
        || scope != naos_idl::memory_object::PROTOCOL_SCOPE
    {
        return Err(RegionRejection::Protocol);
    }
    // A service write reads the caller's buffer; a service read writes it.
    let operation = if service_writes_caller_buffer {
        MemoryDirection::In
    } else {
        MemoryDirection::Out
    };
    if let Some(observed) = observed_direction {
        let matches_operation = match observed {
            RegionDirection::InOut => true,
            RegionDirection::In => operation == MemoryDirection::In,
            RegionDirection::Out => operation == MemoryDirection::Out,
        };
        if !matches_operation {
            return Err(RegionRejection::AccessDenied);
        }
    }
    // `ResourceDescriptor.rights` carries the meta rights (DUPLICATE/TRANSFER/
    // WAIT/INSPECT) and the memory protocol rights (READ/WRITE/MAP/INFO) in one
    // word, and TRANSFER and WRITE share bit 1.  The word therefore cannot
    // express "WRITE but not TRANSFER", so this check enforces what it can:
    // MAP, bit 1, and READ for the direction that needs it.  The write leg of
    // the direction is not distinguishable here and is enforced by its own
    // authority instead -- the wire direction where the transport carries one,
    // and on NaOS the kernel's MemoryObject protocol-rights check when the
    // service installs a writable mapping.
    let required = direction_map_rights()
        | if operation == MemoryDirection::In {
            direction_rights(MemoryDirection::In)
        } else {
            0
        };
    if rights & required != required {
        return Err(RegionRejection::AccessDenied);
    }
    Ok(())
}

/// Admit one region request against the direction and the region rights.
///
/// This is the single contract both transports implement: the required right
/// comes from the direction, the service additionally needs MAP and the
/// transfer needs TRANSFER.  When the caller knows the region's extent the
/// range is checked here as well; a transferred capability has no local
/// extent, and the kernel enforces the bound when the service maps it.
pub fn check_direction_rights(
    direction: MemoryDirection,
    rights: u64,
    offset: u64,
    length: u64,
    known_size: Option<u64>,
) -> Result<(), MemoryError> {
    if length == 0 {
        return Err(MemoryError::InvalidArgument);
    }
    let end = offset
        .checked_add(length)
        .ok_or(MemoryError::InvalidArgument)?;
    if let Some(size) = known_size {
        if end > size {
            return Err(MemoryError::InvalidArgument);
        }
    }
    let required = direction_rights(direction) | direction_map_rights();
    if rights & required != required {
        return Err(MemoryError::AccessDenied);
    }
    Ok(())
}

#[cfg(target_os = "naos")]
mod platform {
    use super::{MemoryError, OwnedHandle};
    use crate::sys;

    pub struct MemoryObject {
        handle: OwnedHandle,
        /// Extent when this process created the object.  A capability that
        /// arrived over a transfer has no local extent, and the kernel
        /// enforces the bound when a service maps it.
        size: Option<u64>,
        /// Mapping installed by `map_persistent`, reused by every later
        /// `read_region`/`write_region`.
        region: core::cell::RefCell<Option<MappedMemory>>,
    }

    /// A bounded capability view over a MemoryObject. The handle is a
    /// restricted capability to the same storage identity; creating a view
    /// does not allocate pages or another kernel storage object.
    pub struct MemoryView {
        handle: OwnedHandle,
        size: u64,
        region: core::cell::RefCell<Option<MappedMemory>>,
    }

    impl MemoryObject {
        pub fn new(size: usize) -> Result<Self, MemoryError> {
            if size == 0 {
                return Err(MemoryError::InvalidArgument);
            }
            let mut handle = sys::HANDLE_INVALID;
            let status = unsafe { sys::_na_memory_create(size as u64, 0, &mut handle) };
            if status != sys::STATUS_OK || handle == sys::HANDLE_INVALID {
                return Err(map_status(status));
            }
            Ok(Self {
                handle: unsafe { OwnedHandle::from_raw(handle) },
                size: Some(size as u64),
                region: core::cell::RefCell::new(None),
            })
        }

        pub fn duplicate_from(source: &OwnedHandle) -> Result<Self, MemoryError> {
            let handle = source.duplicate(0).map_err(map_status)?;
            let size = handle_size(&handle);
            Ok(Self {
                handle,
                size,
                region: core::cell::RefCell::new(None),
            })
        }

        pub fn as_handle(&self) -> &OwnedHandle {
            &self.handle
        }

        /// Extent of the region in bytes, when it is known locally.
        pub fn size(&self) -> Option<u64> {
            self.size
        }

        /// Create a bounded view relative to this capability. The returned
        /// capability cannot be widened by choosing a different operation
        /// offset later; nested views are attenuated against the parent.
        pub fn subspan(&self, offset: u64, length: u64) -> Result<MemoryView, MemoryError> {
            if length == 0 {
                return Err(MemoryError::InvalidArgument);
            }
            if let Some(size) = self.size {
                if offset > size || length > size - offset {
                    return Err(MemoryError::InvalidArgument);
                }
            }
            let restricted = self.handle.duplicate(0).map_err(map_status)?;
            let restriction = sys::HandleRestriction {
                struct_size: core::mem::size_of::<sys::HandleRestriction>() as u32,
                flags: sys::RESTRICTION_RANGE,
                view_offset: offset,
                view_length: length,
                ..sys::HandleRestriction::default()
            };
            let handle = restricted.restrict(&restriction).map_err(map_status)?;
            Ok(MemoryView {
                handle,
                size: length,
                region: core::cell::RefCell::new(None),
            })
        }

        pub fn slice(&self, offset: u64, length: u64) -> Result<MemoryView, MemoryError> {
            self.subspan(offset, length)
        }

        /// Return the native capability handle when the transport needs to
        /// put the object into a NaOS resource table.  Linux has no native
        /// handle table, so the platform implementation returns `None`.
        pub fn native_handle(&self) -> Option<&OwnedHandle> {
            Some(&self.handle)
        }

        pub fn into_handle(self) -> OwnedHandle {
            self.handle
        }

        pub fn into_read_only(self) -> Result<OwnedHandle, MemoryError> {
            let restriction = sys::HandleRestriction {
                struct_size: core::mem::size_of::<sys::HandleRestriction>() as u32,
                flags: sys::RESTRICTION_PROTOCOL_RIGHTS,
                scope: 0,
                revision: 0,
                features: 0,
                meta_rights: 0,
                protocol_rights: sys::MEMORY_RIGHT_READ
                    | sys::MEMORY_RIGHT_MAP
                    | sys::MEMORY_RIGHT_INFO,
                ..sys::HandleRestriction::default()
            };
            let restricted = self.handle.duplicate(0).map_err(map_status)?;
            restricted.restrict(&restriction).map_err(map_status)
        }

        pub fn write_at(&self, offset: u64, data: &[u8]) -> Result<(), MemoryError> {
            let mut mapping = map(
                self.handle.get(),
                offset,
                data.len(),
                sys::MEMORY_MAP_READ | sys::MEMORY_MAP_WRITE | sys::MEMORY_MAP_SHARED,
            )?;
            mapping.as_mut_slice().copy_from_slice(data);
            Ok(())
        }

        pub fn read_at(&self, offset: u64, data: &mut [u8]) -> Result<(), MemoryError> {
            let mapping = map(self.handle.get(), offset, data.len(), sys::MEMORY_MAP_READ)?;
            data.copy_from_slice(mapping.as_slice());
            Ok(())
        }

        /// Install the mapping that later `read_region`/`write_region` calls
        /// reuse.  A block service runs one I/O after another through the same
        /// buffer, so keeping the mapping avoids a MEMORY_MAP/MEMORY_UNMAP
        /// pair per transfer; the mapping is torn down with the object.
        pub fn map_persistent(&self, length: usize, writable: bool) -> Result<(), MemoryError> {
            if length == 0 || length % super::MEMORY_PAGE_SIZE != 0 {
                return Err(MemoryError::InvalidArgument);
            }
            let flags = if writable {
                sys::MEMORY_MAP_READ | sys::MEMORY_MAP_WRITE | sys::MEMORY_MAP_SHARED
            } else {
                sys::MEMORY_MAP_READ | sys::MEMORY_MAP_SHARED
            };
            let mapping = map(self.handle.get(), 0, length, flags)?;
            *self.region.borrow_mut() = Some(mapping);
            Ok(())
        }

        /// Write through the persistent mapping.  This performs no syscall at
        /// all once `map_persistent` has installed it.
        pub fn write_region(&self, offset: u64, data: &[u8]) -> Result<(), MemoryError> {
            let mut region = self.region.borrow_mut();
            let Some(mapping) = region.as_mut() else {
                return Err(MemoryError::Unsupported);
            };
            let start = usize::try_from(offset).map_err(|_| MemoryError::InvalidArgument)?;
            let end = start
                .checked_add(data.len())
                .ok_or(MemoryError::InvalidArgument)?;
            let target = mapping
                .as_mut_slice()
                .get_mut(start..end)
                .ok_or(MemoryError::InvalidArgument)?;
            target.copy_from_slice(data);
            Ok(())
        }

        /// Read through the persistent mapping; no syscall once installed.
        pub fn read_region(&self, offset: u64, data: &mut [u8]) -> Result<(), MemoryError> {
            let region = self.region.borrow();
            let Some(mapping) = region.as_ref() else {
                return Err(MemoryError::Unsupported);
            };
            let start = usize::try_from(offset).map_err(|_| MemoryError::InvalidArgument)?;
            let end = start
                .checked_add(data.len())
                .ok_or(MemoryError::InvalidArgument)?;
            let source = mapping
                .as_slice()
                .get(start..end)
                .ok_or(MemoryError::InvalidArgument)?;
            data.copy_from_slice(source);
            Ok(())
        }
    }

    impl MemoryView {
        pub fn size(&self) -> u64 {
            self.size
        }

        pub fn map(
            &self,
            offset: u64,
            length: usize,
            flags: u32,
        ) -> Result<MappedMemory, MemoryError> {
            check_view_range(offset, length as u64, self.size)?;
            map(self.handle.get(), offset, length, flags)
        }

        pub fn map_all(&self, flags: u32) -> Result<MappedMemory, MemoryError> {
            self.map(
                0,
                usize::try_from(self.size).map_err(|_| MemoryError::InvalidArgument)?,
                flags,
            )
        }

        pub fn as_handle(&self) -> &OwnedHandle {
            &self.handle
        }

        pub fn into_handle(self) -> OwnedHandle {
            self.handle
        }

        pub fn subspan(&self, offset: u64, length: u64) -> Result<MemoryView, MemoryError> {
            if length == 0 || offset > self.size || length > self.size - offset {
                return Err(MemoryError::InvalidArgument);
            }
            let restricted = self.handle.duplicate(0).map_err(map_status)?;
            let restriction = sys::HandleRestriction {
                struct_size: core::mem::size_of::<sys::HandleRestriction>() as u32,
                flags: sys::RESTRICTION_RANGE,
                view_offset: offset,
                view_length: length,
                ..sys::HandleRestriction::default()
            };
            let handle = restricted.restrict(&restriction).map_err(map_status)?;
            Ok(MemoryView {
                handle,
                size: length,
                region: core::cell::RefCell::new(None),
            })
        }

        pub fn slice(&self, offset: u64, length: u64) -> Result<MemoryView, MemoryError> {
            self.subspan(offset, length)
        }

        pub fn write_at(&self, offset: u64, data: &[u8]) -> Result<(), MemoryError> {
            check_view_range(offset, data.len() as u64, self.size)?;
            let mut mapping = map(
                self.handle.get(),
                offset,
                data.len(),
                sys::MEMORY_MAP_READ | sys::MEMORY_MAP_WRITE | sys::MEMORY_MAP_SHARED,
            )?;
            mapping.as_mut_slice().copy_from_slice(data);
            Ok(())
        }

        pub fn read_at(&self, offset: u64, data: &mut [u8]) -> Result<(), MemoryError> {
            check_view_range(offset, data.len() as u64, self.size)?;
            let mapping = map(self.handle.get(), offset, data.len(), sys::MEMORY_MAP_READ)?;
            data.copy_from_slice(mapping.as_slice());
            Ok(())
        }

        pub fn map_persistent(&self, length: usize, writable: bool) -> Result<(), MemoryError> {
            check_view_range(0, length as u64, self.size)?;
            let flags = if writable {
                sys::MEMORY_MAP_READ | sys::MEMORY_MAP_WRITE | sys::MEMORY_MAP_SHARED
            } else {
                sys::MEMORY_MAP_READ | sys::MEMORY_MAP_SHARED
            };
            *self.region.borrow_mut() = Some(map(self.handle.get(), 0, length, flags)?);
            Ok(())
        }

        pub fn write_region(&self, offset: u64, data: &[u8]) -> Result<(), MemoryError> {
            let mut region = self.region.borrow_mut();
            let Some(mapping) = region.as_mut() else {
                return Err(MemoryError::Unsupported);
            };
            let start = usize::try_from(offset).map_err(|_| MemoryError::InvalidArgument)?;
            let end = start
                .checked_add(data.len())
                .ok_or(MemoryError::InvalidArgument)?;
            let target = mapping
                .as_mut_slice()
                .get_mut(start..end)
                .ok_or(MemoryError::InvalidArgument)?;
            target.copy_from_slice(data);
            Ok(())
        }

        pub fn read_region(&self, offset: u64, data: &mut [u8]) -> Result<(), MemoryError> {
            let region = self.region.borrow();
            let Some(mapping) = region.as_ref() else {
                return Err(MemoryError::Unsupported);
            };
            let start = usize::try_from(offset).map_err(|_| MemoryError::InvalidArgument)?;
            let end = start
                .checked_add(data.len())
                .ok_or(MemoryError::InvalidArgument)?;
            let source = mapping
                .as_slice()
                .get(start..end)
                .ok_or(MemoryError::InvalidArgument)?;
            data.copy_from_slice(source);
            Ok(())
        }
    }

    pub struct MappedMemory {
        address: *mut u8,
        map_address: *mut u8,
        map_length: usize,
        length: usize,
    }

    impl MappedMemory {
        pub fn as_slice(&self) -> &[u8] {
            // SAFETY: the mapping is owned by `self` and remains live for the
            // returned borrow.
            unsafe { core::slice::from_raw_parts(self.address, self.length) }
        }

        pub fn as_mut_slice(&mut self) -> &mut [u8] {
            // SAFETY: the mapping was created with MEMORY_MAP_WRITE and is
            // uniquely owned by this value.
            unsafe { core::slice::from_raw_parts_mut(self.address, self.length) }
        }
    }

    impl Drop for MappedMemory {
        fn drop(&mut self) {
            if self.map_address.is_null() {
                return;
            }
            let mut frame = sys::MemoryUnmapFrame {
                struct_size: core::mem::size_of::<sys::MemoryUnmapFrame>() as u32,
                flags: 0,
                address: self.map_address as u64,
                length: self.map_length as u64,
                reserved0: 0,
                reserved1: 0,
            };
            let _ = unsafe { sys::_na_memory_unmap(&mut frame) };
        }
    }

    fn map(
        handle: sys::Handle,
        offset: u64,
        length: usize,
        flags: u32,
    ) -> Result<MappedMemory, MemoryError> {
        if length == 0 {
            return Err(MemoryError::InvalidArgument);
        }
        let mut frame = sys::MemoryMapFrame {
            struct_size: core::mem::size_of::<sys::MemoryMapFrame>() as u32,
            flags,
            hint: 0,
            object: handle,
            offset,
            length: u64::try_from(length).map_err(|_| MemoryError::InvalidArgument)?,
            address: 0,
            data_offset: 0,
            reserved0: 0,
            reserved1: 0,
        };
        let status = unsafe { sys::_na_memory_map(&mut frame) };
        if status != sys::STATUS_OK || frame.address == 0 {
            return Err(if status == sys::STATUS_OK {
                MemoryError::Io
            } else {
                map_status(status)
            });
        }
        let page = 4096_u64;
        let delta = usize::try_from(frame.data_offset).map_err(|_| MemoryError::InvalidArgument)?;
        let map_length = length
            .checked_add(delta)
            .ok_or(MemoryError::InvalidArgument)?;
        Ok(MappedMemory {
            address: (frame.address as usize + delta) as *mut u8,
            map_address: frame.address as *mut u8,
            map_length: map_length
                .checked_add((page - 1) as usize)
                .ok_or(MemoryError::InvalidArgument)?
                & !(page as usize - 1),
            length,
        })
    }

    pub fn map_read_at(
        handle: &OwnedHandle,
        offset: u64,
        length: usize,
    ) -> Result<MappedMemory, MemoryError> {
        map(handle.get(), offset, length, sys::MEMORY_MAP_READ)
    }

    pub fn map_write_at(
        handle: &OwnedHandle,
        offset: u64,
        length: usize,
    ) -> Result<MappedMemory, MemoryError> {
        map(
            handle.get(),
            offset,
            length,
            sys::MEMORY_MAP_READ | sys::MEMORY_MAP_WRITE | sys::MEMORY_MAP_SHARED,
        )
    }

    /// Run `f` over a request region named by a raw capability handle.
    ///
    /// A server that took the generic `naos_idl::IncomingRequest` path holds
    /// its resources as handles rather than as an owned wrapper, so the region
    /// is mapped by value; the handle stays valid only for the dispatch that
    /// received it.
    pub fn with_region_read<T>(
        handle: sys::Handle,
        offset: u64,
        length: usize,
        f: impl FnOnce(&[u8]) -> T,
    ) -> Result<T, MemoryError> {
        let mapping = map(handle, offset, length, sys::MEMORY_MAP_READ)?;
        Ok(f(mapping.as_slice()))
    }

    /// Run `f` over a request region the service writes into.  The mapping is
    /// shared so the caller observes the writes when the invocation completes.
    pub fn with_region_write<T>(
        handle: sys::Handle,
        offset: u64,
        length: usize,
        f: impl FnOnce(&mut [u8]) -> T,
    ) -> Result<T, MemoryError> {
        let mut mapping = map(
            handle,
            offset,
            length,
            sys::MEMORY_MAP_READ | sys::MEMORY_MAP_WRITE | sys::MEMORY_MAP_SHARED,
        )?;
        Ok(f(mapping.as_mut_slice()))
    }

    pub fn map_read(handle: &OwnedHandle, length: usize) -> Result<MappedMemory, MemoryError> {
        map_read_at(handle, 0, length)
    }

    pub fn map_write(handle: &OwnedHandle, length: usize) -> Result<MappedMemory, MemoryError> {
        map_write_at(handle, 0, length)
    }

    pub fn create_and_fill_read_only(bytes: &[u8]) -> Result<OwnedHandle, MemoryError> {
        let object = MemoryObject::new(bytes.len())?;
        object.write_at(0, bytes)?;
        let mut read_back = alloc::vec![0u8; bytes.len()];
        object.read_at(0, &mut read_back)?;
        if read_back != bytes {
            return Err(MemoryError::Io);
        }
        object.into_read_only()
    }

    fn map_status(status: sys::Status) -> MemoryError {
        match status {
            sys::STATUS_INVALID_ARGUMENT => MemoryError::InvalidArgument,
            sys::STATUS_ACCESS_DENIED => MemoryError::AccessDenied,
            sys::STATUS_NOT_SUPPORTED => MemoryError::Unsupported,
            _ => MemoryError::Io,
        }
    }

    fn check_view_range(offset: u64, length: u64, size: u64) -> Result<(), MemoryError> {
        if length == 0 || offset > size || length > size - offset {
            Err(MemoryError::InvalidArgument)
        } else {
            Ok(())
        }
    }

    fn handle_size(handle: &OwnedHandle) -> Option<u64> {
        let mut info = sys::HandleInfo {
            struct_size: core::mem::size_of::<sys::HandleInfo>() as u32,
            ..sys::HandleInfo::default()
        };
        (unsafe { sys::_na_handle_get_info(handle.get(), &mut info) } == sys::STATUS_OK)
            .then_some(info.view_length)
    }
}

#[cfg(target_os = "linux")]
mod platform {
    use super::{MemoryError, OwnedHandle};
    use crate::linux::bulk::{self, BulkAccessError, LinuxMemoryObject};
    use crate::transport::Endpoint;
    use naos_idl::transport::{BulkBuffer, BulkDirection, ResourceDescriptor};
    use std::collections::BTreeMap;
    use std::sync::{Arc, LazyLock, Mutex};

    #[doc(hidden)]
    pub const RIGHT_READ: u64 = bulk::BULK_RIGHT_READ;
    #[doc(hidden)]
    pub const RIGHT_WRITE: u64 = bulk::BULK_RIGHT_WRITE;

    /// Low-level Linux transport descriptor used by transport tests. Daemons
    /// should use `Client::invoke_memory_blocking` instead.
    #[doc(hidden)]
    pub type MemoryDescriptor = BulkBuffer;

    pub struct MemoryObject {
        region: LinuxMemoryObject,
    }

    /// A zero-copy, bounded view over one Linux memfd-backed storage identity.
    /// Cloning or nesting a view only copies metadata and an `Arc<File>`; it
    /// never creates another memfd.
    #[derive(Clone)]
    pub struct MemoryView {
        region: LinuxMemoryObject,
        view_offset: u64,
        size: u64,
    }

    impl MemoryObject {
        pub fn new(length: usize) -> Result<Self, MemoryError> {
            LinuxMemoryObject::new(length)
                .map(|region| Self { region })
                .map_err(|_| MemoryError::Io)
        }

        /// Extent of the region in bytes.
        pub fn size(&self) -> Option<u64> {
            Some(self.region.length())
        }

        pub fn subspan(&self, offset: u64, length: u64) -> Result<MemoryView, MemoryError> {
            check_view_range(offset, length, self.region.length())?;
            Ok(MemoryView {
                region: self.region.clone(),
                view_offset: offset,
                size: length,
            })
        }

        pub fn slice(&self, offset: u64, length: u64) -> Result<MemoryView, MemoryError> {
            self.subspan(offset, length)
        }

        pub fn write_at(&self, offset: u64, data: &[u8]) -> Result<(), MemoryError> {
            self.region
                .write_at(offset, data)
                .map_err(|_| MemoryError::Io)
        }

        pub fn read_at(&self, offset: u64, data: &mut [u8]) -> Result<(), MemoryError> {
            self.region
                .read_at(offset, data)
                .map_err(|_| MemoryError::Io)
        }

        /// Install the storage used by later `read_region`/`write_region`
        /// calls.  The Linux region is a memfd that stays directly addressable
        /// for the object's lifetime, so there is nothing to pin here; the
        /// method exists so a daemon can write one code path for both
        /// platforms.
        pub fn map_persistent(&self, length: usize, _writable: bool) -> Result<(), MemoryError> {
            check_view_range(0, length as u64, self.region.length())?;
            Ok(())
        }

        /// Write through the region storage.
        pub fn write_region(&self, offset: u64, data: &[u8]) -> Result<(), MemoryError> {
            self.write_at(offset, data)
        }

        /// Read through the region storage.
        pub fn read_region(&self, offset: u64, data: &mut [u8]) -> Result<(), MemoryError> {
            self.read_at(offset, data)
        }

        /// Linux resources are transferred through the private memfd/UDS
        /// adapter rather than a NaOS capability handle.
        pub fn native_handle(&self) -> Option<&OwnedHandle> {
            None
        }

        #[doc(hidden)]
        pub fn register(&self, endpoint: &Endpoint) -> Result<(), MemoryError> {
            self.region.register(endpoint).map_err(|_| MemoryError::Io)
        }

        #[doc(hidden)]
        pub fn descriptor(
            &self,
            offset: u64,
            length: u64,
            direction: BulkDirection,
            rights: u64,
        ) -> Result<MemoryDescriptor, MemoryError> {
            self.region
                .descriptor(offset, length, direction, rights)
                .map_err(map_bulk_error)
        }

        #[doc(hidden)]
        pub fn resource_descriptor(
            &self,
            descriptor: MemoryDescriptor,
            write: bool,
        ) -> ResourceDescriptor {
            ResourceDescriptor {
                resource_id: descriptor.region_id,
                binding: naos_sys::BINDING_MEMORY_OBJECT,
                scope: naos_idl::memory_object::PROTOCOL_SCOPE,
                rights: if write {
                    naos_sys::MEMORY_RIGHT_WRITE
                        | naos_sys::MEMORY_RIGHT_MAP
                        | naos_sys::RIGHT_TRANSFER
                } else {
                    naos_sys::MEMORY_RIGHT_READ
                        | naos_sys::MEMORY_RIGHT_MAP
                        | naos_sys::RIGHT_TRANSFER
                },
                view_offset: descriptor.offset,
                view_length: descriptor.length,
            }
        }
    }

    impl MemoryView {
        pub fn size(&self) -> u64 {
            self.size
        }

        pub fn subspan(&self, offset: u64, length: u64) -> Result<MemoryView, MemoryError> {
            check_view_range(offset, length, self.size)?;
            let view_offset = self
                .view_offset
                .checked_add(offset)
                .ok_or(MemoryError::InvalidArgument)?;
            Ok(MemoryView {
                region: self.region.clone(),
                view_offset,
                size: length,
            })
        }

        pub fn slice(&self, offset: u64, length: u64) -> Result<MemoryView, MemoryError> {
            self.subspan(offset, length)
        }

        pub fn write_at(&self, offset: u64, data: &[u8]) -> Result<(), MemoryError> {
            check_view_range(offset, data.len() as u64, self.size)?;
            let absolute = self
                .view_offset
                .checked_add(offset)
                .ok_or(MemoryError::InvalidArgument)?;
            self.region
                .write_at(absolute, data)
                .map_err(|_| MemoryError::Io)
        }

        pub fn read_at(&self, offset: u64, data: &mut [u8]) -> Result<(), MemoryError> {
            check_view_range(offset, data.len() as u64, self.size)?;
            let absolute = self
                .view_offset
                .checked_add(offset)
                .ok_or(MemoryError::InvalidArgument)?;
            self.region
                .read_at(absolute, data)
                .map_err(|_| MemoryError::Io)
        }

        pub fn map_persistent(&self, length: usize, _writable: bool) -> Result<(), MemoryError> {
            check_view_range(0, length as u64, self.size)?;
            Ok(())
        }

        pub fn write_region(&self, offset: u64, data: &[u8]) -> Result<(), MemoryError> {
            self.write_at(offset, data)
        }

        pub fn read_region(&self, offset: u64, data: &mut [u8]) -> Result<(), MemoryError> {
            self.read_at(offset, data)
        }

        #[doc(hidden)]
        pub fn register(&self, endpoint: &Endpoint) -> Result<(), MemoryError> {
            self.region.register(endpoint).map_err(|_| MemoryError::Io)
        }

        #[doc(hidden)]
        pub fn descriptor(
            &self,
            offset: u64,
            length: u64,
            direction: BulkDirection,
            rights: u64,
        ) -> Result<MemoryDescriptor, MemoryError> {
            check_view_range(offset, length, self.size)?;
            let absolute = self
                .view_offset
                .checked_add(offset)
                .ok_or(MemoryError::InvalidArgument)?;
            self.region
                .descriptor(absolute, length, direction, rights)
                .map_err(map_bulk_error)
        }

        #[doc(hidden)]
        pub fn resource_descriptor(
            &self,
            descriptor: MemoryDescriptor,
            write: bool,
        ) -> ResourceDescriptor {
            ResourceDescriptor {
                resource_id: descriptor.region_id,
                binding: naos_sys::BINDING_MEMORY_OBJECT,
                scope: naos_idl::memory_object::PROTOCOL_SCOPE,
                rights: if write {
                    naos_sys::MEMORY_RIGHT_WRITE
                        | naos_sys::MEMORY_RIGHT_MAP
                        | naos_sys::RIGHT_TRANSFER
                } else {
                    naos_sys::MEMORY_RIGHT_READ
                        | naos_sys::MEMORY_RIGHT_MAP
                        | naos_sys::RIGHT_TRANSFER
                },
                view_offset: descriptor.offset,
                view_length: descriptor.length,
            }
        }
    }

    fn check_view_range(offset: u64, length: u64, size: u64) -> Result<(), MemoryError> {
        if length == 0 || offset > size || length > size - offset {
            Err(MemoryError::InvalidArgument)
        } else {
            Ok(())
        }
    }

    pub(crate) fn read_descriptor_for(
        endpoint: &std::path::Path,
        owner: u64,
        descriptor: MemoryDescriptor,
        data: &mut [u8],
    ) -> Result<(), MemoryError> {
        bulk::read_for(endpoint, owner, descriptor, data).map_err(map_bulk_error)
    }

    pub(crate) fn write_descriptor_for(
        endpoint: &std::path::Path,
        owner: u64,
        descriptor: MemoryDescriptor,
        data: &[u8],
    ) -> Result<(), MemoryError> {
        bulk::write_for(endpoint, owner, descriptor, data).map_err(map_bulk_error)
    }

    pub fn create_and_fill_read_only(_bytes: &[u8]) -> Result<OwnedHandle, MemoryError> {
        Err(MemoryError::Unsupported)
    }

    /// Bytes behind a region named by a raw capability handle.
    ///
    /// A Linux daemon region is identified by a transferred descriptor, so a
    /// raw handle never names one on the real transport.  The in-process IDL
    /// loopback kernel used by host tests is a different case: it has no
    /// mapping syscall, so a server dispatched through `naos_idl` resolves its
    /// request region here.  The key is the kernel object identity, which a
    /// DUPLICATE transfer preserves while it mints a fresh handle.  The
    /// production path never installs the fake kernel, so every lookup
    /// declines before touching a capability.
    type LoopbackRegion = Arc<Mutex<Vec<u8>>>;

    static LOOPBACK_REGIONS: LazyLock<Mutex<BTreeMap<u64, LoopbackRegion>>> =
        LazyLock::new(|| Mutex::new(BTreeMap::new()));

    fn loopback_key(handle: crate::sys::Handle) -> Option<u64> {
        if !naos_idl::host_fake_kernel_installed() {
            return None;
        }
        naos_idl::object_id(handle).ok()
    }

    fn loopback_region(handle: crate::sys::Handle) -> Option<LoopbackRegion> {
        let key = loopback_key(handle)?;
        LOOPBACK_REGIONS.lock().ok()?.get(&key).cloned()
    }

    /// Register the host-test bytes a loopback-dispatched request reaches
    /// through `handle`.  Only the in-process IDL loopback transport has a
    /// handle-addressed region; a real Linux daemon uses a descriptor.
    #[doc(hidden)]
    pub fn register_loopback_region(handle: crate::sys::Handle, bytes: Vec<u8>) {
        let Some(key) = loopback_key(handle) else {
            return;
        };
        if let Ok(mut regions) = LOOPBACK_REGIONS.lock() {
            regions.insert(key, Arc::new(Mutex::new(bytes)));
        }
    }

    /// Run `f` over the host-test bytes registered for a loopback region.
    #[doc(hidden)]
    pub fn with_loopback_region<T>(
        handle: crate::sys::Handle,
        f: impl FnOnce(&mut [u8]) -> T,
    ) -> Option<T> {
        let region = loopback_region(handle)?;
        let mut bytes = region.lock().ok()?;
        Some(f(&mut bytes))
    }

    /// Resolve one loopback region and the requested window inside it.
    /// `Unsupported` means the handle never named a host-test region.
    fn loopback_window(
        handle: crate::sys::Handle,
        offset: u64,
        length: usize,
    ) -> Result<(LoopbackRegion, usize, usize), MemoryError> {
        let region = loopback_region(handle).ok_or(MemoryError::Unsupported)?;
        let start = usize::try_from(offset).map_err(|_| MemoryError::InvalidArgument)?;
        let end = start
            .checked_add(length)
            .ok_or(MemoryError::InvalidArgument)?;
        Ok((region, start, end))
    }

    /// Linux resolves a raw-handle region only through the host-test loopback
    /// registry; the real data plane goes through
    /// `Request::with_read_memory`/`with_write_memory`.
    pub fn with_region_read<T>(
        handle: crate::sys::Handle,
        offset: u64,
        length: usize,
        f: impl FnOnce(&[u8]) -> T,
    ) -> Result<T, MemoryError> {
        let (region, start, end) = loopback_window(handle, offset, length)?;
        let bytes = region.lock().map_err(|_| MemoryError::Io)?;
        let window = bytes.get(start..end).ok_or(MemoryError::InvalidArgument)?;
        Ok(f(window))
    }

    /// See [`with_region_read`].
    pub fn with_region_write<T>(
        handle: crate::sys::Handle,
        offset: u64,
        length: usize,
        f: impl FnOnce(&mut [u8]) -> T,
    ) -> Result<T, MemoryError> {
        let (region, start, end) = loopback_window(handle, offset, length)?;
        let mut bytes = region.lock().map_err(|_| MemoryError::Io)?;
        let window = bytes
            .get_mut(start..end)
            .ok_or(MemoryError::InvalidArgument)?;
        Ok(f(window))
    }

    fn map_bulk_error(error: BulkAccessError) -> MemoryError {
        match error {
            BulkAccessError::OutOfRange => MemoryError::InvalidArgument,
            BulkAccessError::WrongDirection => MemoryError::AccessDenied,
            _ => MemoryError::Io,
        }
    }
}

#[cfg(target_os = "naos")]
pub use platform::{
    MappedMemory, MemoryObject, MemoryView, create_and_fill_read_only, map_read, map_read_at,
    map_write, map_write_at, with_region_read, with_region_write,
};

#[cfg(target_os = "linux")]
pub use platform::{
    MemoryObject, MemoryView, create_and_fill_read_only, register_loopback_region,
    with_loopback_region, with_region_read, with_region_write,
};

#[cfg(target_os = "linux")]
pub(crate) use platform::{read_descriptor_for, write_descriptor_for};

#[cfg(target_os = "linux")]
#[doc(hidden)]
pub use platform::{MemoryDescriptor, RIGHT_READ, RIGHT_WRITE};

#[cfg(test)]
mod region_contract_tests {
    //! W2 contract: the same semantic region request is admitted or refused
    //! identically on both transports.  Both `Request::validate_memory`
    //! implementations call `admit_service_region`, so this table is the
    //! shared decision; `check_direction_rights` is the client-side half used
    //! by both `memory_transfer` implementations.

    use super::{
        MEMORY_PAGE_SIZE, MemoryDirection, MemoryError, RegionDirection, RegionRejection,
        admit_service_region, check_direction_rights, direction_map_rights, direction_rights,
        page_aligned_size,
    };
    use crate::sys;

    const MAP_AND_TRANSFER: u64 = sys::MEMORY_RIGHT_MAP | sys::RIGHT_TRANSFER;

    /// Rights a caller grants when data flows *to* the service: the service
    /// reads the caller's buffer, so it needs READ, MAP and TRANSFER.  This is
    /// the direction a caller uses for a service write.
    fn caller_grants_read() -> u64 {
        direction_rights(MemoryDirection::In) | direction_map_rights()
    }

    #[test]
    fn persistent_region_sizes_are_page_aligned() {
        assert_eq!(page_aligned_size(1), Ok(MEMORY_PAGE_SIZE));
        assert_eq!(page_aligned_size(MEMORY_PAGE_SIZE), Ok(MEMORY_PAGE_SIZE));
        assert_eq!(page_aligned_size(MEMORY_PAGE_SIZE + 1), Ok(2 * MEMORY_PAGE_SIZE));
        assert_eq!(page_aligned_size(usize::MAX), Err(MemoryError::InvalidArgument));
    }

    /// Rights a caller grants when data flows *from* the service: the service
    /// writes the caller's buffer, so it needs WRITE, MAP and TRANSFER.  This
    /// is the direction a caller uses for a service read.
    fn caller_grants_write() -> u64 {
        direction_rights(MemoryDirection::Out) | direction_map_rights()
    }

    #[test]
    fn service_admission_matches_the_shared_decision_table() {
        let scope = naos_idl::memory_object::PROTOCOL_SCOPE;
        let binding = sys::BINDING_MEMORY_OBJECT;

        // A legal service write: the caller granted READ (the service reads
        // its buffer) plus MAP and TRANSFER.
        assert_eq!(
            admit_service_region(
                true,
                binding,
                scope,
                caller_grants_read(),
                Some(RegionDirection::In)
            ),
            Ok(())
        );
        // InOut is legal for either operation.
        assert_eq!(
            admit_service_region(
                false,
                binding,
                scope,
                direction_rights(MemoryDirection::InOut) | direction_map_rights(),
                Some(RegionDirection::InOut)
            ),
            Ok(())
        );
        // A legal service read: the caller granted WRITE.
        assert_eq!(
            admit_service_region(
                false,
                binding,
                scope,
                caller_grants_write(),
                Some(RegionDirection::Out)
            ),
            Ok(())
        );

        // The combined rights word can enforce the READ leg (bit 0), MAP and
        // bit 1; it cannot express WRITE-without-TRANSFER because TRANSFER and
        // WRITE share bit 1, so a request without READ is refused for a service
        // write and the write leg is left to its own authority.
        assert_eq!(
            admit_service_region(
                true,
                binding,
                scope,
                sys::MEMORY_RIGHT_WRITE | MAP_AND_TRANSFER,
                Some(RegionDirection::In)
            ),
            Err(RegionRejection::AccessDenied)
        );
        // A service read needs no READ bit of its own, so granting it is legal
        // and the direction field is what proves the intent.
        assert_eq!(
            admit_service_region(
                false,
                binding,
                scope,
                sys::MEMORY_RIGHT_READ | MAP_AND_TRANSFER,
                Some(RegionDirection::Out)
            ),
            Ok(())
        );

        // The region must be transferable and mappable.
        assert_eq!(
            admit_service_region(
                true,
                binding,
                scope,
                sys::MEMORY_RIGHT_READ,
                Some(RegionDirection::In)
            ),
            Err(RegionRejection::AccessDenied)
        );
        assert_eq!(
            admit_service_region(
                true,
                binding,
                scope,
                sys::MEMORY_RIGHT_READ | sys::MEMORY_RIGHT_MAP,
                Some(RegionDirection::In)
            ),
            Err(RegionRejection::AccessDenied)
        );

        // A direction that contradicts the operation is refused where the
        // wire can express one.
        assert_eq!(
            admit_service_region(
                true,
                binding,
                scope,
                caller_grants_read(),
                Some(RegionDirection::Out)
            ),
            Err(RegionRejection::AccessDenied)
        );
        assert_eq!(
            admit_service_region(
                false,
                binding,
                scope,
                caller_grants_write(),
                Some(RegionDirection::In)
            ),
            Err(RegionRejection::AccessDenied)
        );

        // A capability that is not a MemoryObject is a protocol error, not a
        // rights failure.
        assert_eq!(
            admit_service_region(
                true,
                sys::BINDING_CLIENT_END,
                scope,
                caller_grants_read(),
                Some(RegionDirection::In)
            ),
            Err(RegionRejection::Protocol)
        );
        assert_eq!(
            admit_service_region(
                true,
                binding,
                naos_idl::block_device::PROTOCOL_SCOPE,
                caller_grants_read(),
                Some(RegionDirection::In)
            ),
            Err(RegionRejection::Protocol)
        );
    }

    #[test]
    fn naos_capability_direction_is_expressed_by_the_granted_rights() {
        // A NaOS capability has no direction field, so a request arrives with
        // no observed direction.  The READ bit is the enforce leg: a grant that
        // omits it cannot satisfy a service write, because the service must
        // read the caller's buffer.
        let binding = sys::BINDING_MEMORY_OBJECT;
        let scope = naos_idl::memory_object::PROTOCOL_SCOPE;
        assert_eq!(
            admit_service_region(
                true,
                binding,
                scope,
                sys::MEMORY_RIGHT_READ | MAP_AND_TRANSFER,
                None
            ),
            Ok(())
        );
        assert_eq!(
            admit_service_region(
                true,
                binding,
                scope,
                sys::MEMORY_RIGHT_WRITE | MAP_AND_TRANSFER,
                None
            ),
            Err(RegionRejection::AccessDenied)
        );
        assert_eq!(
            admit_service_region(
                false,
                binding,
                scope,
                sys::MEMORY_RIGHT_WRITE | MAP_AND_TRANSFER,
                None
            ),
            Ok(())
        );
    }

    #[test]
    fn client_range_and_rights_are_checked_before_the_transfer() {
        // Direction In is the direction a caller uses for a service write.
        let rights = caller_grants_read();
        assert_eq!(
            check_direction_rights(MemoryDirection::In, rights, 0, 4096, Some(4096)),
            Ok(())
        );
        // A window past the region end is a bad argument.
        assert_eq!(
            check_direction_rights(MemoryDirection::In, rights, 4096, 4096, Some(4096)),
            Err(MemoryError::InvalidArgument)
        );
        // So is an offset plus length that overflows.
        assert_eq!(
            check_direction_rights(MemoryDirection::In, rights, u64::MAX, 2, None),
            Err(MemoryError::InvalidArgument)
        );
        // An empty window is never a valid transfer.
        assert_eq!(
            check_direction_rights(MemoryDirection::In, rights, 0, 0, None),
            Err(MemoryError::InvalidArgument)
        );
        // Rights that do not cover the direction are a denial.
        assert_eq!(
            check_direction_rights(
                MemoryDirection::In,
                sys::MEMORY_RIGHT_WRITE | MAP_AND_TRANSFER,
                0,
                4096,
                Some(4096)
            ),
            Err(MemoryError::AccessDenied)
        );
        // A transferred capability has no local extent, so only the rights are
        // checked here; the kernel enforces the bound at map time.
        assert_eq!(
            check_direction_rights(MemoryDirection::In, rights, 1 << 40, 4096, None),
            Ok(())
        );
    }
}

#[cfg(all(test, target_os = "linux"))]
mod tests {
    use std::vec;
    use std::vec::Vec;

    use super::{MemoryError, MemoryObject};
    use naos_idl::transport::BulkDirection;

    #[test]
    fn linux_memory_object_round_trip_uses_direct_userland_storage() {
        let _accounting = crate::linux::bulk::lock_region_accounting();
        let object = MemoryObject::new(8192).expect("memory object");
        let input: Vec<u8> = (0..1024).map(|value| value as u8).collect();
        object.write_at(257, &input).expect("memory write");

        let mut output = vec![0; input.len()];
        object.read_at(257, &mut output).expect("memory read");
        assert_eq!(output, input);
    }

    #[test]
    fn linux_memory_view_is_bounded_and_zero_copy() {
        let _accounting = crate::linux::bulk::lock_region_accounting();
        let object = MemoryObject::new(4 * 1024 * 1024).expect("memory object");
        let view = object.subspan(3, 17).expect("unaligned view");
        assert_eq!(view.size(), 17);
        let object_descriptor = object
            .descriptor(
                0,
                4 * 1024 * 1024,
                BulkDirection::InOut,
                crate::linux::bulk::BULK_RIGHT_READ | crate::linux::bulk::BULK_RIGHT_WRITE,
            )
            .expect("object descriptor");
        let view_descriptor = view
            .descriptor(
                0,
                17,
                BulkDirection::InOut,
                crate::linux::bulk::BULK_RIGHT_READ | crate::linux::bulk::BULK_RIGHT_WRITE,
            )
            .expect("view descriptor");
        assert_eq!(view_descriptor.region_id, object_descriptor.region_id);
        assert_eq!(view_descriptor.offset, 3);
        assert_eq!(view_descriptor.length, 17);

        view.write_at(0, b"bounded-memory").expect("view write");
        let mut output = [0_u8; 14];
        view.read_at(0, &mut output).expect("view read");
        assert_eq!(&output, b"bounded-memory");

        let mut backing = [0_u8; 14];
        object.read_at(3, &mut backing).expect("backing read");
        assert_eq!(&backing, b"bounded-memory");

        assert!(matches!(
            view.subspan(17, 1),
            Err(MemoryError::InvalidArgument)
        ));
        assert!(matches!(
            view.subspan(u64::MAX, 1),
            Err(MemoryError::InvalidArgument)
        ));
        assert!(matches!(
            view.subspan(16, 2),
            Err(MemoryError::InvalidArgument)
        ));
    }
}
