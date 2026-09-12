//! Pure LBD lease-range arithmetic (VFS ADR §6.1) and the protocol flag and
//! error constants `ramdiskd` reasons about. Everything here is host-testable:
//! no syscalls, no capability handles.

/// `BlockDeviceFactory.acquire.flags` v1 domain: only READ_ONLY is defined
/// (VFS ADR §6.1 flag table; UAPI name
/// `NA_BLOCK_DEVICE_FACTORY_ACQUIRE_FLAG_READ_ONLY`).
pub const ACQUIRE_FLAG_READ_ONLY: u64 = 1 << 0;

/// Domain errors declared by `BlockDeviceFactory.acquire` in the frozen IDL;
/// they travel as negative POSIX errnos on the invocation result.
pub const ACQUIRE_ERROR_EBUSY: i64 = -16;

/// An exclusive logical-block-device interval `[start_lba, start_lba +
/// block_count)` as leased from a [`crate::lease`] factory.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct LeaseRange {
    pub start_lba: u64,
    pub block_count: u64,
}

impl LeaseRange {
    /// A non-empty lease range; rejects `block_count == 0`, which every block
    /// request must also reject (§6.1 request rules).
    pub fn new(start_lba: u64, block_count: u64) -> Option<Self> {
        if block_count == 0 {
            return None;
        }
        Some(Self {
            start_lba,
            block_count,
        })
    }

    /// The whole-disk v1 configuration lease `[0, total_block_count)`.
    pub fn whole(total_block_count: u64) -> Option<Self> {
        Self::new(0, total_block_count)
    }

    /// The last `block_count` blocks of a disk with `total_block_count`
    /// blocks — the scratch region this module self-tests on.
    pub fn tail(total_block_count: u64, block_count: u64) -> Option<Self> {
        if block_count > total_block_count {
            return None;
        }
        Self::new(total_block_count - block_count, block_count)
    }

    /// Exclusive end offset, `None` on u64 overflow (`acquire` must reject
    /// `start_lba + block_count` overflow the same way).
    pub fn end(&self) -> Option<u64> {
        self.start_lba.checked_add(self.block_count)
    }

    /// Whether this range is fully contained in `[0, total_block_count)`.
    pub fn fits_within(&self, total_block_count: u64) -> bool {
        self.end().is_some_and(|end| end <= total_block_count)
    }

    /// Exclusive-interval overlap: adjacent ranges do not intersect, so two
    /// neighbouring partitions can be leased concurrently (ADR §6.1).
    pub fn overlaps(&self, other: &LeaseRange) -> bool {
        match (self.end(), other.end()) {
            (Some(self_end), Some(other_end)) => {
                self.start_lba < other_end && other.start_lba < self_end
            }
            _ => true,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn zero_block_count_is_rejected() {
        assert!(LeaseRange::new(0, 0).is_none());
        assert!(LeaseRange::whole(0).is_none());
    }

    #[test]
    fn end_overflow_is_detected() {
        let range = LeaseRange::new(u64::MAX - 1, 4).unwrap();
        assert_eq!(range.end(), None);
        assert!(!range.fits_within(u64::MAX));
        // A valid range ending exactly at the medium end still fits.
        let exact = LeaseRange::new(u64::MAX - 4, 4).unwrap();
        assert_eq!(exact.end(), Some(u64::MAX));
        assert!(exact.fits_within(u64::MAX));
    }

    #[test]
    fn overlapping_ranges_intersect() {
        let whole = LeaseRange::whole(65536).unwrap();
        let partition = LeaseRange::new(2048, 4096).unwrap();
        assert!(partition.overlaps(&whole));
        assert!(whole.overlaps(&partition));
        assert!(
            LeaseRange::new(0, 1)
                .unwrap()
                .overlaps(&LeaseRange::new(0, 1).unwrap())
        );
    }

    #[test]
    fn adjacent_ranges_do_not_overlap() {
        let first = LeaseRange::new(0, 2048).unwrap();
        let second = LeaseRange::new(2048, 2048).unwrap();
        assert!(!first.overlaps(&second));
        assert!(!second.overlaps(&first));
    }

    #[test]
    fn disjoint_ranges_do_not_overlap() {
        let low = LeaseRange::new(0, 16).unwrap();
        let high = LeaseRange::new(1024, 16).unwrap();
        assert!(!low.overlaps(&high));
    }

    #[test]
    fn tail_scratch_range_sits_at_disk_end() {
        let scratch = LeaseRange::tail(65536, 8).unwrap();
        assert_eq!(scratch.start_lba, 65528);
        assert_eq!(scratch.block_count, 8);
        assert_eq!(scratch.end(), Some(65536));
        assert!(scratch.fits_within(65536));
        // Oversized tails are rejected rather than wrapping around.
        assert!(LeaseRange::tail(4, 8).is_none());
    }

    #[test]
    fn read_only_flag_matches_frozen_bit() {
        assert_eq!(ACQUIRE_FLAG_READ_ONLY, 1);
    }
}
