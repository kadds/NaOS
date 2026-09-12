//! Block-device integration seam between the FAT worker and the NaOS
//! `BlockDevice` object (VFS ADR §6.1).
//!
//! The standalone process hands this worker a generated `BlockDevice` channel
//! client wrapped in an LBD lease. The trait is also exercised on the host
//! with [`RamBlockDevice`], which serves as the reference implementation for
//! flush/FUA bookkeeping.

use core::fmt;

/// Static description returned by `BlockDevice.get_info`.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct BlockInfo {
    pub sector_size: u32,
    pub physical_sector_size: u32,
    pub block_count: u64,
    pub max_transfer_blocks: u64,
    pub max_transfer_bytes: u64,
    pub max_in_flight: u64,
    pub features: u64,
    pub media_generation: u64,
    pub medium_id: u64,
    pub read_only: bool,
}

pub const FEATURE_READ_ONLY: u64 = 1 << 0;
pub const FEATURE_FLUSH: u64 = 1 << 1;
pub const FEATURE_FUA: u64 = 1 << 2;
pub const FEATURE_DISCARD: u64 = 1 << 3;
pub const FEATURE_VOLATILE_WRITE_CACHE: u64 = 1 << 4;

/// Client-side failure modes of a block request.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum BlockError {
    OutOfRange,
    ReadOnly,
    Io,
}

impl fmt::Display for BlockError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let message = match self {
            Self::OutOfRange => "block range out of range or unaligned",
            Self::ReadOnly => "medium is read-only",
            Self::Io => "block I/O failure",
        };
        f.write_str(message)
    }
}

impl core::error::Error for BlockError {}

/// Filesystem worker byte-range adapter.  This is not the IDL protocol
/// client; it is the FAT worker's smaller, sector-aligned I/O seam.
pub trait BlockClient {
    fn get_info(&self) -> Result<BlockInfo, BlockError>;
    fn read(&self, offset: u64, buf: &mut [u8]) -> Result<(), BlockError>;
    fn write(&self, offset: u64, buf: &[u8]) -> Result<(), BlockError>;
    fn flush(&self, fua: bool) -> Result<(), BlockError>;
}

/// Host-only in-memory implementation of the worker's block-client contract.
/// The production worker uses the service's `RemoteBlockIo` adapter over the
/// kernel-issued `BlockDevice` capability.
///
/// Semantics mirror ADR §6.1: reads and writes take a byte range aligned to
/// the medium sector size; `flush(fua)` persists dirty data, with FUA
/// guaranteeing durability at return.
///
/// In-memory block device over a `Vec<u8>` image.
///
/// Counts flush/FUA calls so tests can assert the fsync→flush/FUA mapping
/// end-to-end.
/// Shared mutable state: cloning a `RamBlockDevice` yields another handle
/// onto the *same* image and counters, mirroring how channel-client
/// handles alias one device.
#[derive(Debug)]
struct RamInner {
    data: core::cell::RefCell<alloc::vec::Vec<u8>>,
    plain_flushes: core::cell::Cell<u64>,
    fua_flushes: core::cell::Cell<u64>,
}

#[derive(Debug, Clone)]
pub struct RamBlockDevice {
    inner: alloc::rc::Rc<core::cell::RefCell<RamInner>>,
    sector_size: u32,
    read_only: bool,
    medium_id: u64,
}

impl RamBlockDevice {
    /// Total plain flushes observed so far (host-test observable).
    pub fn plain_flushes(&self) -> u64 {
        self.inner.borrow().plain_flushes.get()
    }

    /// Total FUA flushes observed so far (host-test observable).
    pub fn fua_flushes(&self) -> u64 {
        self.inner.borrow().fua_flushes.get()
    }
}

impl RamBlockDevice {
    pub fn new(data: alloc::vec::Vec<u8>, sector_size: u32) -> Self {
        Self::with_medium(data, sector_size, false, 1)
    }

    pub fn with_medium(
        data: alloc::vec::Vec<u8>,
        sector_size: u32,
        read_only: bool,
        medium_id: u64,
    ) -> Self {
        Self {
            inner: alloc::rc::Rc::new(core::cell::RefCell::new(RamInner {
                data: core::cell::RefCell::new(data),
                plain_flushes: core::cell::Cell::new(0),
                fua_flushes: core::cell::Cell::new(0),
            })),
            sector_size,
            read_only,
            medium_id,
        }
    }

    /// Full image copy (host tests / diagnostics).
    pub fn snapshot(&self) -> alloc::vec::Vec<u8> {
        self.inner.borrow().data.borrow().clone()
    }

    fn check_range(&self, offset: u64, len: usize) -> Result<(), BlockError> {
        let ss = self.sector_size as u64;
        if ss == 0 || !ss.is_power_of_two() || len == 0 {
            return Err(BlockError::OutOfRange);
        }
        if offset % ss != 0 || len as u64 % ss != 0 {
            return Err(BlockError::OutOfRange);
        }
        if len as u64 / ss > 128 {
            return Err(BlockError::OutOfRange);
        }
        let end = offset
            .checked_add(len as u64)
            .ok_or(BlockError::OutOfRange)?;
        if end > self.inner.borrow().data.borrow().len() as u64 {
            return Err(BlockError::OutOfRange);
        }
        Ok(())
    }
}

impl BlockClient for RamBlockDevice {
    fn get_info(&self) -> Result<BlockInfo, BlockError> {
        Ok(BlockInfo {
            sector_size: self.sector_size,
            physical_sector_size: self.sector_size,
            block_count: (self.inner.borrow().data.borrow().len() / self.sector_size as usize)
                as u64,
            max_transfer_blocks: 128,
            max_transfer_bytes: 128 * self.sector_size as u64,
            max_in_flight: 1,
            features: FEATURE_FLUSH
                | FEATURE_FUA
                | if self.read_only { FEATURE_READ_ONLY } else { 0 },
            media_generation: 1,
            medium_id: self.medium_id,
            read_only: self.read_only,
        })
    }

    fn read(&self, offset: u64, buf: &mut [u8]) -> Result<(), BlockError> {
        self.check_range(offset, buf.len())?;
        let start = offset as usize;
        buf.copy_from_slice(&self.inner.borrow().data.borrow()[start..start + buf.len()]);
        Ok(())
    }

    fn write(&self, offset: u64, buf: &[u8]) -> Result<(), BlockError> {
        if self.read_only {
            return Err(BlockError::ReadOnly);
        }
        self.check_range(offset, buf.len())?;
        let start = offset as usize;
        let inner = self.inner.borrow();
        let mut data = inner.data.borrow_mut();
        data[start..start + buf.len()].copy_from_slice(buf);
        Ok(())
    }

    fn flush(&self, fua: bool) -> Result<(), BlockError> {
        let inner = self.inner.borrow();
        if fua {
            inner.fua_flushes.set(inner.fua_flushes.get() + 1);
        } else {
            inner.plain_flushes.set(inner.plain_flushes.get() + 1);
        }
        Ok(())
    }
}

/// File-backed block client used by the std test and persistence paths.
///
/// This is deliberately kept outside the NaOS target: it gives the FAT
/// worker an actual process/reopen persistence boundary for host tests. The
/// file is treated as a raw medium (no partition parsing or filesystem
/// policy), and `flush(true)` is a real `sync_all` barrier.
#[derive(Debug, Clone)]
pub struct FileBlockDevice {
    inner: alloc::sync::Arc<std::sync::Mutex<FileInner>>,
    sector_size: u32,
    bytes: u64,
    read_only: bool,
    medium_id: u64,
}

#[derive(Debug)]
struct FileInner {
    file: std::fs::File,
    plain_flushes: u64,
    fua_flushes: u64,
}

impl FileBlockDevice {
    /// Create/truncate a raw image to `bytes`.  The size and sector geometry
    /// are validated here so every cloned handle shares one fixed medium.
    pub fn create<P: AsRef<std::path::Path>>(
        path: P,
        bytes: u64,
        sector_size: u32,
        medium_id: u64,
    ) -> Result<Self, BlockError> {
        if sector_size < 512
            || !sector_size.is_power_of_two()
            || bytes == 0
            || bytes % sector_size as u64 != 0
        {
            return Err(BlockError::OutOfRange);
        }
        let file = std::fs::OpenOptions::new()
            .create(true)
            .truncate(true)
            .read(true)
            .write(true)
            .open(path)
            .map_err(|_| BlockError::Io)?;
        file.set_len(bytes).map_err(|_| BlockError::Io)?;
        Self::from_file(file, bytes, sector_size, false, medium_id)
    }

    /// Open an existing raw image.  Read-only handles never open the file
    /// writable and therefore exercise the same lease protection as LBD.
    pub fn open<P: AsRef<std::path::Path>>(
        path: P,
        sector_size: u32,
        read_only: bool,
        medium_id: u64,
    ) -> Result<Self, BlockError> {
        if sector_size < 512 || !sector_size.is_power_of_two() {
            return Err(BlockError::OutOfRange);
        }
        let file = std::fs::OpenOptions::new()
            .read(true)
            .write(!read_only)
            .open(path)
            .map_err(|_| BlockError::Io)?;
        let bytes = file.metadata().map_err(|_| BlockError::Io)?.len();
        if bytes == 0 || bytes % sector_size as u64 != 0 {
            return Err(BlockError::OutOfRange);
        }
        Self::from_file(file, bytes, sector_size, read_only, medium_id)
    }

    fn from_file(
        file: std::fs::File,
        bytes: u64,
        sector_size: u32,
        read_only: bool,
        medium_id: u64,
    ) -> Result<Self, BlockError> {
        if sector_size < 512 || !sector_size.is_power_of_two() {
            return Err(BlockError::OutOfRange);
        }
        Ok(Self {
            inner: alloc::sync::Arc::new(std::sync::Mutex::new(FileInner {
                file,
                plain_flushes: 0,
                fua_flushes: 0,
            })),
            sector_size,
            bytes,
            read_only,
            medium_id,
        })
    }

    fn check_range(&self, offset: u64, len: usize) -> Result<(), BlockError> {
        let sector = self.sector_size as u64;
        if offset % sector != 0 || len == 0 || len as u64 % sector != 0 {
            return Err(BlockError::OutOfRange);
        }
        if len as u64 / sector > 128 {
            return Err(BlockError::OutOfRange);
        }
        let end = offset
            .checked_add(len as u64)
            .ok_or(BlockError::OutOfRange)?;
        if end > self.bytes {
            return Err(BlockError::OutOfRange);
        }
        Ok(())
    }

    fn lock(&self) -> Result<std::sync::MutexGuard<'_, FileInner>, BlockError> {
        self.inner.lock().map_err(|_| BlockError::Io)
    }
}

impl BlockClient for FileBlockDevice {
    fn get_info(&self) -> Result<BlockInfo, BlockError> {
        Ok(BlockInfo {
            sector_size: self.sector_size,
            physical_sector_size: self.sector_size,
            block_count: self.bytes / self.sector_size as u64,
            max_transfer_blocks: 128,
            max_transfer_bytes: 128 * self.sector_size as u64,
            max_in_flight: 1,
            features: FEATURE_FLUSH
                | FEATURE_FUA
                | if self.read_only { FEATURE_READ_ONLY } else { 0 },
            media_generation: 1,
            medium_id: self.medium_id,
            read_only: self.read_only,
        })
    }

    fn read(&self, offset: u64, buf: &mut [u8]) -> Result<(), BlockError> {
        self.check_range(offset, buf.len())?;
        let mut inner = self.lock()?;
        use std::io::{Read, Seek, SeekFrom};
        inner
            .file
            .seek(SeekFrom::Start(offset))
            .map_err(|_| BlockError::Io)?;
        inner.file.read_exact(buf).map_err(|_| BlockError::Io)
    }

    fn write(&self, offset: u64, buf: &[u8]) -> Result<(), BlockError> {
        if self.read_only {
            return Err(BlockError::ReadOnly);
        }
        self.check_range(offset, buf.len())?;
        let mut inner = self.lock()?;
        use std::io::{Seek, SeekFrom, Write};
        inner
            .file
            .seek(SeekFrom::Start(offset))
            .map_err(|_| BlockError::Io)?;
        inner.file.write_all(buf).map_err(|_| BlockError::Io)
    }

    fn flush(&self, fua: bool) -> Result<(), BlockError> {
        let mut inner = self.lock()?;
        if fua {
            inner.file.sync_all().map_err(|_| BlockError::Io)?;
            inner.fua_flushes += 1;
        } else {
            inner.file.sync_data().map_err(|_| BlockError::Io)?;
            inner.plain_flushes += 1;
        }
        Ok(())
    }
}
