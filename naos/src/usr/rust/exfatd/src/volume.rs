//! Volume adapter: maps byte-granular filesystem I/O onto block-client
//! sector I/O (VFS ADR §6.1 → appendix A flush/FUA mapping).
//!
//! The FAT library issues unaligned, byte-granular reads/writes/seeks. The
//! real `BlockDevice` client only accepts sector-aligned ranges. This
//! adapter closes that gap with a single-sector scratch buffer and
//! read-modify-write on partial edge sectors.
//!
//! Mapping decisions recorded here (ADR appendix A "fsync 持久化" row):
//! - filesystem-level flush (`Write::flush`, i.e. FAT metadata sync) maps
//!   to `BlockClient::flush(false)`;
//! - POSIX `fsync`/`fdatasync` on a file or directory maps to
//!   [`VolumeAdapter::sync_fua`] — `flush(true)` — which must not return
//!   before data survives restart.

use crate::block::{BlockClient, BlockError, BlockInfo};
use std::io::{Read, Result as IoResult, Seek, SeekFrom, Write};

/// Sector-aligned block I/O view over a [`BlockClient`], implementing the
/// io traits the FAT library requires.
pub struct VolumeAdapter<D: BlockClient> {
    client: D,
    info: BlockInfo,
    pos: u64,
    scratch: alloc::vec::Vec<u8>,
    pending_offset: Option<u64>,
    pending: alloc::vec::Vec<u8>,
    cached_sectors: [Option<u64>; 2],
    cached_buffers: [alloc::vec::Vec<u8>; 2],
    cached_dirty: [bool; 2],
    write_back: bool,
    max_transfer_bytes: usize,
}

/// Keep the transport request within the block protocol's advertised limit
/// and the MemoryObject implementation's bounded transfer size.
const MEMORY_OBJECT_TRANSFER_LIMIT: usize = 127 * 512;

impl<D: BlockClient> VolumeAdapter<D> {
    pub fn new(client: D) -> Result<Self, BlockError> {
        let info = client.get_info()?;
        let ss = info.sector_size;
        if ss < 512 || ss > 4096 || !ss.is_power_of_two() {
            return Err(BlockError::OutOfRange);
        }
        let block_limit = info
            .max_transfer_blocks
            .checked_mul(u64::from(ss))
            .ok_or(BlockError::OutOfRange)?;
        let max_transfer_bytes = usize::try_from(
            info.max_transfer_bytes
                .min(block_limit)
                .min(MEMORY_OBJECT_TRANSFER_LIMIT as u64),
        )
        .map_err(|_| BlockError::OutOfRange)?
            / ss as usize
            * ss as usize;
        if max_transfer_bytes == 0 {
            return Err(BlockError::OutOfRange);
        }
        Ok(Self {
            client,
            info,
            pos: 0,
            scratch: alloc::vec![0u8; ss as usize],
            pending_offset: None,
            pending: alloc::vec::Vec::new(),
            cached_sectors: [None; 2],
            cached_buffers: [alloc::vec![0u8; ss as usize], alloc::vec![0u8; ss as usize]],
            cached_dirty: [false; 2],
            write_back: false,
            max_transfer_bytes,
        })
    }

    /// Enable write-back for metadata-heavy operations such as formatting.
    ///
    /// The default adapter preserves ordinary `Write` visibility: once a
    /// write returns, the block client has received it.  Formatting is the
    /// one operation that deliberately benefits from coalescing many small
    /// FAT entry writes into sector writes.
    pub fn with_write_back(mut self) -> Self {
        self.write_back = true;
        self
    }

    pub fn info(&self) -> &BlockInfo {
        &self.info
    }

    fn flush_pending(&mut self) -> Result<(), BlockError> {
        let Some(offset) = self.pending_offset.take() else {
            return Ok(());
        };
        let pending = core::mem::take(&mut self.pending);
        if let Err(error) = self.client.write(offset, &pending) {
            self.pending_offset = Some(offset);
            self.pending = pending;
            return Err(error);
        }
        Ok(())
    }

    fn cached_slot(&self, offset: u64) -> Option<usize> {
        self.cached_sectors
            .iter()
            .position(|sector| *sector == Some(offset))
    }

    fn flush_cached_slot(&mut self, slot: usize) -> Result<(), BlockError> {
        if !self.cached_dirty[slot] {
            return Ok(());
        }
        let offset = self.cached_sectors[slot].expect("dirty sector has an offset");
        self.client.write(offset, &self.cached_buffers[slot])?;
        self.cached_dirty[slot] = false;
        Ok(())
    }

    fn flush_cached_sectors(&mut self) -> Result<(), BlockError> {
        for slot in 0..self.cached_sectors.len() {
            self.flush_cached_slot(slot)?;
        }
        Ok(())
    }

    fn flush_all(&mut self) -> Result<(), BlockError> {
        self.flush_cached_sectors()?;
        self.flush_pending()
    }

    fn load_sector(&mut self, offset: u64) -> Result<usize, BlockError> {
        if let Some(slot) = self.cached_slot(offset) {
            return Ok(slot);
        }
        self.flush_pending()?;
        let slot = self
            .cached_sectors
            .iter()
            .position(Option::is_none)
            .or_else(|| self.cached_dirty.iter().position(|dirty| !dirty))
            .unwrap_or(0);
        self.flush_cached_slot(slot)?;
        self.client.read(offset, &mut self.cached_buffers[slot])?;
        self.cached_sectors[slot] = Some(offset);
        self.cached_dirty[slot] = false;
        Ok(slot)
    }

    fn queue_full_sectors(&mut self, offset: u64, data: &[u8]) -> Result<(), BlockError> {
        if !self.write_back {
            return self.client.write(offset, data);
        }
        self.flush_cached_sectors()?;
        if self.pending_offset.is_none() {
            self.pending_offset = Some(offset);
        } else if self
            .pending_offset
            .unwrap()
            .saturating_add(self.pending.len() as u64)
            != offset
        {
            self.flush_pending()?;
            self.pending_offset = Some(offset);
        }
        self.pending.extend_from_slice(data);
        if self.pending.len() == self.max_transfer_bytes {
            self.flush_pending()?;
        }
        Ok(())
    }

    /// Read exactly `buf.len()` bytes at `offset`; every touched range is
    /// served from full sectors so the block client only ever sees aligned
    /// requests. `offset + buf.len()` must not exceed medium end; the FAT
    /// library never reads past the volume it computed from `get_info`.
    fn read_exact_at(&mut self, mut offset: u64, mut buf: &mut [u8]) -> Result<(), BlockError> {
        if self.write_back {
            // FAT metadata commonly reads an entry immediately after a
            // previous entry write in the same sector. Keep that sector
            // cached; pending full-sector writes still become visible first.
            self.flush_pending()?;
        } else {
            self.flush_all()?;
        }
        while !buf.is_empty() {
            let in_sector = (offset % self.info.sector_size as u64) as usize;
            if !self.write_back && in_sector == 0 {
                let full_bytes = (buf.len() / self.scratch.len() * self.scratch.len())
                    .min(self.max_transfer_bytes);
                if full_bytes != 0 {
                    self.client.read(offset, &mut buf[..full_bytes])?;
                    offset += full_bytes as u64;
                    buf = &mut buf[full_bytes..];
                    continue;
                }
            }
            let take = core::cmp::min(buf.len(), self.scratch.len() - in_sector);
            let sector_base = offset - in_sector as u64;
            if self.write_back {
                let slot = self.load_sector(sector_base)?;
                buf[..take]
                    .copy_from_slice(&self.cached_buffers[slot][in_sector..in_sector + take]);
            } else {
                self.client.read(sector_base, &mut self.scratch)?;
                buf[..take].copy_from_slice(&self.scratch[in_sector..in_sector + take]);
            }
            offset += take as u64;
            buf = &mut buf[take..];
        }
        Ok(())
    }

    /// Write exactly `buf.len()` bytes at `offset`. Full interior sectors
    /// go straight through; partial edge sectors are read, merged and
    /// rewritten (read-modify-write).
    fn write_exact_at(&mut self, mut offset: u64, mut buf: &[u8]) -> Result<(), BlockError> {
        if self.info.read_only {
            return Err(BlockError::ReadOnly);
        }
        let ss = self.scratch.len();
        while !buf.is_empty() {
            let in_sector = (offset % ss as u64) as usize;
            if in_sector == 0 {
                let full_bytes = (buf.len() / ss * ss).min(self.max_transfer_bytes);
                if full_bytes != 0 {
                    self.queue_full_sectors(offset, &buf[..full_bytes])?;
                    offset += full_bytes as u64;
                    buf = &buf[full_bytes..];
                    continue;
                }
            }
            let take = core::cmp::min(buf.len(), ss - in_sector);
            let sector_base = offset - in_sector as u64;
            if take == ss {
                // Full sector: no read needed.
                self.client.write(sector_base, &buf[..take])?;
            } else if !self.write_back {
                // Keep the ordinary adapter write-through.  The formatter
                // opts into write-back explicitly below.
                self.client.read(sector_base, &mut self.scratch)?;
                self.scratch[in_sector..in_sector + take].copy_from_slice(&buf[..take]);
                self.client.write(sector_base, &self.scratch)?;
            } else {
                self.flush_pending()?;
                let slot = self.load_sector(sector_base)?;
                self.cached_buffers[slot][in_sector..in_sector + take]
                    .copy_from_slice(&buf[..take]);
                self.cached_dirty[slot] = true;
            }
            offset += take as u64;
            buf = &buf[take..];
        }
        Ok(())
    }

    /// Plain persistence barrier (filesystem metadata sync).
    pub fn flush_plain(&mut self) -> Result<(), BlockError> {
        self.flush_all()?;
        self.client.flush(false)
    }

    /// FUA persistence barrier: POSIX fsync/fdatasync mapping. Must not
    /// return before written data survives restart.
    pub fn sync_fua(&mut self) -> Result<(), BlockError> {
        self.flush_all()?;
        self.client.flush(true)
    }
}

impl<D: BlockClient> Read for VolumeAdapter<D> {
    fn read(&mut self, buf: &mut [u8]) -> IoResult<usize> {
        // The FAT library drives reads through seek+read_exact; a short
        // read at EOF is signalled by returning Ok(0) past medium end.
        let remaining = (self.info.sector_size as u64)
            .checked_mul(self.info.block_count)
            .unwrap_or(u64::MAX)
            .saturating_sub(self.pos);
        if remaining == 0 || buf.is_empty() {
            return Ok(0);
        }
        let take = core::cmp::min(buf.len() as u64, remaining) as usize;
        self.read_exact_at(self.pos, &mut buf[..take])
            .map_err(io_err)?;
        self.pos += take as u64;
        Ok(take)
    }
}

impl<D: BlockClient> Write for VolumeAdapter<D> {
    fn write(&mut self, buf: &[u8]) -> IoResult<usize> {
        self.write_exact_at(self.pos, buf).map_err(io_err)?;
        self.pos += buf.len() as u64;
        Ok(buf.len())
    }

    fn flush(&mut self) -> IoResult<()> {
        self.flush_plain().map_err(io_err)
    }
}

impl<D: BlockClient> Seek for VolumeAdapter<D> {
    fn seek(&mut self, pos: SeekFrom) -> IoResult<u64> {
        let medium_bytes = (self.info.sector_size as u64).saturating_mul(self.info.block_count);
        let new_pos = match pos {
            SeekFrom::Start(p) => p as i64,
            SeekFrom::End(d) => medium_bytes as i64 + d,
            SeekFrom::Current(d) => self.pos as i64 + d,
        };
        if new_pos < 0 {
            return Err(std::io::Error::new(
                std::io::ErrorKind::InvalidInput,
                "Seek to a negative offset",
            ));
        }
        let new_pos = new_pos as u64;
        let sector_base = new_pos - new_pos % self.info.sector_size as u64;
        if self.cached_slot(sector_base).is_none() {
            self.flush_all().map_err(io_err)?;
        }
        self.pos = new_pos;
        Ok(self.pos)
    }
}

/// Map a block-client failure onto the io error surface the FAT library
/// understands.
fn io_err(err: BlockError) -> std::io::Error {
    let kind = match err {
        BlockError::OutOfRange => std::io::ErrorKind::InvalidData,
        BlockError::ReadOnly => std::io::ErrorKind::PermissionDenied,
        BlockError::Io => std::io::ErrorKind::Other,
    };
    std::io::Error::new(kind, "block device failure")
}

#[cfg(test)]
mod tests {
    extern crate std;

    use super::*;
    use crate::block::RamBlockDevice;
    use alloc::vec;

    fn device() -> RamBlockDevice {
        RamBlockDevice::new(vec![0u8; 8 * 512], 512)
    }

    #[test]
    fn rejects_bad_sector_size() {
        let dev = RamBlockDevice::new(vec![0u8; 4096], 100);
        assert!(VolumeAdapter::new(dev).is_err());
        let dev = RamBlockDevice::new(vec![0u8; 4096], 128);
        assert!(VolumeAdapter::new(dev).is_err()); // < 512
    }

    #[test]
    fn unaligned_write_does_read_modify_write() {
        let dev = device();
        // Pre-fill sector 0 pattern through the raw client.
        let pattern: alloc::vec::Vec<u8> = (0..512u32).map(|i| i as u8).collect();
        crate::block::BlockClient::write(&dev, 0, &pattern).unwrap();

        let mut ad = VolumeAdapter::new(dev.clone()).unwrap();
        Seek::seek(&mut ad, SeekFrom::Start(510)).unwrap();
        Write::write(&mut ad, &[0xAA, 0xBB, 0xCC]).unwrap();

        let img = dev.snapshot();
        assert_eq!(&img[508..510], &[252, 253]); // preserved tail of pattern
        assert_eq!(&img[510..513], &[0xAA, 0xBB, 0xCC]);
        assert_eq!(img[513], 0); // next sector byte untouched
    }

    #[test]
    fn unaligned_read_spans_sectors() {
        let dev = device();
        crate::block::BlockClient::write(&dev, 0, &[1u8; 512]).unwrap();
        crate::block::BlockClient::write(&dev, 512, &[2u8; 512]).unwrap();

        let mut ad = VolumeAdapter::new(dev.clone()).unwrap();
        Seek::seek(&mut ad, SeekFrom::Start(511)).unwrap();
        let mut buf = [0u8; 3];
        assert_eq!(Read::read(&mut ad, &mut buf).unwrap(), 3);
        assert_eq!(&buf, &[1, 2, 2]);
    }

    #[test]
    fn read_at_eof_returns_zero() {
        let dev = device();
        let mut ad = VolumeAdapter::new(dev).unwrap();
        Seek::seek(&mut ad, SeekFrom::End(-4)).unwrap();
        let mut buf = [0u8; 8];
        assert_eq!(Read::read(&mut ad, &mut buf).unwrap(), 4);
        assert_eq!(Read::read(&mut ad, &mut buf).unwrap(), 0);
    }

    #[test]
    fn write_on_read_only_medium_fails() {
        let dev = RamBlockDevice::with_medium(vec![0u8; 1024], 512, true, 1);
        let mut ad = VolumeAdapter::new(dev).unwrap();
        Seek::seek(&mut ad, SeekFrom::Start(0)).unwrap();
        assert!(Write::write(&mut ad, &[0u8; 512]).is_err());
    }

    #[test]
    fn fua_barrier_reaches_client() {
        let dev = device();
        let before = dev.fua_flushes();
        let mut ad = VolumeAdapter::new(dev.clone()).unwrap();
        ad.sync_fua().unwrap();
        assert_eq!(dev.fua_flushes(), before + 1);
    }
}
