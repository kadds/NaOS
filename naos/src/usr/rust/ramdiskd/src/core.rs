//! Transport-independent ramdisk state and block-range policy.
//!
//! The Linux UDS and NaOS channel adapters use different ways to carry a
//! buffer, but lease ownership, range validation, read-only policy and flush
//! semantics must be identical on both paths.

extern crate alloc;

use alloc::vec::Vec;
use alloc::boxed::Box;

use crate::lease::{ACQUIRE_FLAG_READ_ONLY, LeaseRange};
use naos_idl::block_device as block_idl;
use naos_idl::block_device_factory as factory_idl;

pub const BLOCK_SIZE: u64 = 512;
pub const MAX_TRANSFER_BLOCKS: u64 = 128;

/// Requests the block service admits at once.
///
/// The advertised `max_in_flight` and the transport's admission bound are the
/// same constant, so a device can never report concurrency the service loop
/// will not honour.  The service genuinely overlaps observations: a read takes
/// the medium's lock shared and runs its body on its own task, while a mutation
/// takes the lock exclusively after the outstanding read bodies have been
/// drained, which is what keeps block ordering.
///
/// Deliberately small.  This bounds how many reads may be in flight, and reads
/// of one medium share its bandwidth and its lock, so a deep pipeline buys
/// queue depth rather than throughput.  Four is enough to keep the device busy
/// across a handful of callers without letting one peer monopolise the medium.
pub const MAX_IN_FLIGHT: u64 = 4;
pub const FEATURES: u64 = 2 | 4 | 8; // FLUSH | FUA | DISCARD
pub const FEATURE_READ_ONLY: u64 = 1;

pub const RIGHT_BLOCK_INSPECT: u64 = 1 << 13;
pub const RIGHT_BLOCK_READ: u64 = 1 << 14;
pub const RIGHT_BLOCK_WRITE: u64 = 1 << 15;
pub const RIGHT_BLOCK_FLUSH: u64 = 1 << 16;
pub const RIGHT_BLOCK_DISCARD: u64 = 1 << 17;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct DeviceLease {
    pub range: LeaseRange,
    pub resource_id: u64,
    pub read_only: bool,
    pub device_id: u64,
}

/// Storage backing for a block manager.  The protocol and lease policy do not
/// depend on whether bytes come from an in-memory test double, a prepared
/// image, or a future persistent backend.
///
/// `Send + Sync`, and `read_at`/`len` take `&self`, because observations are
/// allowed to overlap: the manager shares one `RamDisk` across concurrent
/// readers, so a backend that needed `&mut self` to read could not be admitted
/// at all.  Mutations keep `&mut self`, which is what makes them exclusive.
///
/// The split matters for cost, not just for safety.  Today `RamStore` is a
/// `memcpy`, so the backend work is ~12-25us of a ~730us call and overlapping
/// it saves nothing measurable; a backend whose per-request work dominated
/// (real device I/O, encryption, verification) is exactly where read overlap
/// starts to pay.  Keeping reads `&self` means such a backend needs no
/// structural change here.
pub trait BlockStore: Send + Sync {
    fn len(&self) -> usize;
    fn read_at(&self, offset: usize, output: &mut [u8]) -> Result<(), i64>;
    fn write_at(&mut self, offset: usize, input: &[u8]) -> Result<(), i64>;
    fn discard_at(&mut self, offset: usize, length: usize) -> Result<(), i64>;
}

/// In-memory BlockStore used by host tests and as the prepared-image loader's
/// current implementation.  The image is copied once at boot; all later
/// block accesses go through the BlockStore boundary.
pub struct RamStore {
    bytes: Vec<u8>,
}

impl RamStore {
    fn zeroed(bytes: usize) -> Self {
        Self {
            bytes: alloc::vec![0; bytes],
        }
    }

    fn from_bytes(bytes: Vec<u8>) -> Option<Self> {
        if bytes.is_empty() || bytes.len() as u64 % BLOCK_SIZE != 0 {
            return None;
        }
        Some(Self { bytes })
    }
}

impl BlockStore for RamStore {
    fn len(&self) -> usize {
        self.bytes.len()
    }

    fn read_at(&self, offset: usize, output: &mut [u8]) -> Result<(), i64> {
        let end = offset.checked_add(output.len()).ok_or(-22)?;
        let source = self.bytes.get(offset..end).ok_or(-22)?;
        output.copy_from_slice(source);
        Ok(())
    }

    fn write_at(&mut self, offset: usize, input: &[u8]) -> Result<(), i64> {
        let end = offset.checked_add(input.len()).ok_or(-22)?;
        let destination = self.bytes.get_mut(offset..end).ok_or(-22)?;
        destination.copy_from_slice(input);
        Ok(())
    }

    fn discard_at(&mut self, offset: usize, length: usize) -> Result<(), i64> {
        let end = offset.checked_add(length).ok_or(-22)?;
        let destination = self.bytes.get_mut(offset..end).ok_or(-22)?;
        destination.fill(0);
        Ok(())
    }
}

pub struct RamDisk {
    store: Box<dyn BlockStore>,
    read_only: bool,
    next_resource: u64,
    next_device_id: u64,
    leases: Vec<DeviceLease>,
    plain_flushes: u64,
    fua_flushes: u64,
}

impl RamDisk {
    pub fn new(bytes: usize, read_only: bool) -> Self {
        assert!(bytes > 0 && bytes as u64 % BLOCK_SIZE == 0);
        Self::with_store(Box::new(RamStore::zeroed(bytes)), read_only)
    }

    pub fn from_bytes(bytes: Vec<u8>, read_only: bool) -> Option<Self> {
        Some(Self::with_store(
            Box::new(RamStore::from_bytes(bytes)?),
            read_only,
        ))
    }

    fn with_store(store: Box<dyn BlockStore>, read_only: bool) -> Self {
        Self {
            store,
            read_only,
            next_resource: 1,
            next_device_id: 1,
            leases: Vec::new(),
            plain_flushes: 0,
            fua_flushes: 0,
        }
    }

    pub fn block_count(&self) -> u64 {
        self.store.len() as u64 / BLOCK_SIZE
    }

    pub fn medium_info(&self) -> factory_idl::BlockMediumInfo {
        factory_idl::BlockMediumInfo {
            medium_id: 1,
            media_generation: 1,
            logical_block_bytes: BLOCK_SIZE,
            physical_block_bytes: BLOCK_SIZE,
            total_block_count: self.block_count(),
            max_transfer_blocks: MAX_TRANSFER_BLOCKS,
            max_transfer_bytes: MAX_TRANSFER_BLOCKS * BLOCK_SIZE,
            // The advertised depth is the same constant the transport enforces,
            // so a client can size its pipeline against a number the service
            // will actually honour.
            max_in_flight: MAX_IN_FLIGHT,
            features: FEATURES | if self.read_only { FEATURE_READ_ONLY } else { 0 },
        }
    }

    pub fn block_info(&self, lease: DeviceLease) -> block_idl::BlockInfo {
        let info = self.medium_info();
        block_idl::BlockInfo {
            device_id: lease.device_id,
            media_generation: info.media_generation,
            logical_block_bytes: info.logical_block_bytes,
            physical_block_bytes: info.physical_block_bytes,
            block_count: lease.range.block_count,
            max_transfer_blocks: info.max_transfer_blocks,
            max_transfer_bytes: info.max_transfer_bytes,
            max_in_flight: info.max_in_flight,
            features: info.features
                | if lease.read_only {
                    FEATURE_READ_ONLY
                } else {
                    0
                },
            medium_id: info.medium_id,
        }
    }

    pub fn acquire(
        &mut self,
        start_lba: u64,
        block_count: u64,
        flags: u64,
    ) -> Result<DeviceLease, i64> {
        if flags & !ACQUIRE_FLAG_READ_ONLY != 0 {
            return Err(-22);
        }
        let range = LeaseRange::new(start_lba, block_count)
            .filter(|range| range.fits_within(self.block_count()))
            .ok_or(-22)?;
        if self.leases.iter().any(|lease| lease.range.overlaps(&range)) {
            return Err(-16);
        }
        let resource_id = self.next_resource;
        self.next_resource = self.next_resource.checked_add(1).ok_or(-75)?;
        let device_id = self.next_device_id;
        self.next_device_id = self.next_device_id.checked_add(1).ok_or(-75)?;
        let lease = DeviceLease {
            range,
            resource_id,
            read_only: self.read_only || flags & ACQUIRE_FLAG_READ_ONLY != 0,
            device_id,
        };
        self.leases.push(lease);
        Ok(lease)
    }

    pub fn authorize(
        &self,
        resource_id: u64,
        rights: u64,
        required_rights: u64,
    ) -> Result<DeviceLease, i64> {
        if rights & required_rights != required_rights {
            return Err(-13);
        }
        self.leases
            .iter()
            .find(|lease| lease.resource_id == resource_id)
            .copied()
            .ok_or(-13)
    }

    /// Release a device endpoint after its peer closes. The backing medium
    /// remains resident; only the leased LBA interval is returned to the
    /// allocator.
    pub fn release(&mut self, resource_id: u64) -> bool {
        let Some(index) = self
            .leases
            .iter()
            .position(|lease| lease.resource_id == resource_id)
        else {
            return false;
        };
        self.leases.swap_remove(index);
        true
    }

    fn storage_span(
        &self,
        lease: DeviceLease,
        lba: u64,
        count: u64,
    ) -> Result<(usize, usize), i64> {
        if count == 0 || count > MAX_TRANSFER_BLOCKS {
            return Err(-22);
        }
        let end = lba.checked_add(count).ok_or(-22)?;
        if end > lease.range.block_count {
            return Err(-22);
        }
        let absolute = lease.range.start_lba.checked_add(lba).ok_or(-22)?;
        let start = absolute.checked_mul(BLOCK_SIZE).ok_or(-22)?;
        let bytes = count.checked_mul(BLOCK_SIZE).ok_or(-22)?;
        let end = start.checked_add(bytes).ok_or(-22)?;
        let start = usize::try_from(start).map_err(|_| -22)?;
        let end = usize::try_from(end).map_err(|_| -22)?;
        if end > self.store.len() {
            return Err(-22);
        }
        Ok((start, end))
    }

    pub fn read_blocks(
        &self,
        lease: DeviceLease,
        lba: u64,
        count: u64,
        flags: u64,
        output: &mut [u8],
    ) -> Result<(), i64> {
        if flags != 0 {
            return Err(-22);
        }
        let (start, end) = self.storage_span(lease, lba, count)?;
        if output.len() != end - start {
            return Err(-22);
        }
        self.store.read_at(start, output)
    }

    pub fn write_blocks(
        &mut self,
        lease: DeviceLease,
        lba: u64,
        count: u64,
        flags: u64,
        input: &[u8],
    ) -> Result<(), i64> {
        if lease.read_only || flags & !1 != 0 || flags & 1 != 0 && FEATURES & 4 == 0 {
            return Err(if lease.read_only { -13 } else { -22 });
        }
        let (start, end) = self.storage_span(lease, lba, count)?;
        if input.len() != end - start {
            return Err(-22);
        }
        self.store.write_at(start, input)?;
        if flags & 1 != 0 {
            self.fua_flushes = self.fua_flushes.saturating_add(1);
        }
        Ok(())
    }

    pub fn flush(&mut self, lease: DeviceLease) -> Result<(), i64> {
        if lease.read_only {
            return Err(-13);
        }
        if FEATURES & 2 == 0 {
            return Err(-95);
        }
        self.plain_flushes = self.plain_flushes.saturating_add(1);
        Ok(())
    }

    pub fn discard(&mut self, lease: DeviceLease, lba: u64, count: u64) -> Result<(), i64> {
        if lease.read_only {
            return Err(-13);
        }
        if FEATURES & 8 == 0 {
            return Err(-95);
        }
        let (start, end) = self.storage_span(lease, lba, count)?;
        self.store.discard_at(start, end - start)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn medium_advertises_implemented_block_features() {
        let disk = RamDisk::new(16 * BLOCK_SIZE as usize, false);
        let info = disk.medium_info();
        assert_eq!(info.features & (2 | 4 | 8), 2 | 4 | 8);
        assert_eq!(info.logical_block_bytes, BLOCK_SIZE);
        assert_eq!(
            info.max_transfer_bytes,
            info.max_transfer_blocks * BLOCK_SIZE
        );
    }

    #[test]
    fn write_flags_are_validated_and_fua_is_recorded() {
        let mut disk = RamDisk::new(16 * BLOCK_SIZE as usize, false);
        let lease = disk.acquire(0, 16, 0).expect("lease");
        let data = [0x5a; BLOCK_SIZE as usize];
        assert_eq!(disk.write_blocks(lease, 0, 1, 2, &data), Err(-22));
        assert!(disk.write_blocks(lease, 0, 1, 1, &data).is_ok());
        assert_eq!(disk.fua_flushes, 1);
    }

    #[test]
    fn released_lease_can_be_acquired_again_without_recreating_medium() {
        let mut disk = RamDisk::new(16 * BLOCK_SIZE as usize, false);
        let lease = disk.acquire(0, 16, 0).expect("first lease");
        assert_eq!(disk.acquire(0, 16, 0), Err(-16));
        assert!(disk.release(lease.resource_id));
        assert!(!disk.release(lease.resource_id));
        assert!(disk.acquire(0, 16, 0).is_ok());
    }
}
