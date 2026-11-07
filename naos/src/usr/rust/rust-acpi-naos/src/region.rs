//! Capability-relative OperationRegion admission.
//!
//! This module is deliberately independent from the upstream AML `Handler`.
//! A caller can only obtain a token from a validated lease, and every logical
//! access is checked against the OperationRegion byte range again.

const PAGE_SIZE: u64 = 4096;

fn exact_page_cover(base: u64, end: u64) -> Result<(u64, u64), RegionError> {
    let page_base = base & !(PAGE_SIZE - 1);
    let end_offset = end & (PAGE_SIZE - 1);
    let padding = if end_offset == 0 {
        0
    } else {
        PAGE_SIZE - end_offset
    };
    let page_end = end.checked_add(padding).ok_or(RegionError::Overflow)?;
    Ok((page_base, page_end))
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum RegionSpace {
    SystemMemory,
    SystemIo,
    PciConfig,
    Unsupported(u8),
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum LeaseKind {
    DeviceMmio,
    FirmwareReserved,
    FirmwareStateReserved,
    AcpiFixedHardware,
    IoPort,
    PciConfig,
    NormalRam,
    KernelMemory,
    TableSnapshot,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum CacheType {
    Uncacheable,
    WriteBack,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum AccessMode {
    Read,
    ReadWrite,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct OperationRegion {
    pub space: RegionSpace,
    pub base: u64,
    pub length: u64,
    pub parent_device: u64,
    pub generation: u64,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct SystemIoResource {
    pub parent_device_path: std::string::String,
    pub base: u64,
    pub length: u64,
}

/// Normalize only an unambiguous, currently assigned `_CRS` I/O range.
/// Flexible base-address windows are deliberately ignored: they do not prove
/// which physical ports belong to this device at runtime.
pub fn system_io_resource_from_crs(
    parent_device_path: &str,
    minimum_base: u64,
    maximum_base: u64,
    translation_offset: u64,
    length: u64,
    is_consumer: bool,
) -> Option<SystemIoResource> {
    try_system_io_resource_from_crs(
        parent_device_path,
        minimum_base,
        maximum_base,
        translation_offset,
        length,
        is_consumer,
    )
    .ok()
    .flatten()
}

pub fn try_system_io_resource_from_crs(
    parent_device_path: &str,
    minimum_base: u64,
    maximum_base: u64,
    translation_offset: u64,
    length: u64,
    is_consumer: bool,
) -> Result<Option<SystemIoResource>, RegionError> {
    if !parent_device_path.starts_with('\\')
        || parent_device_path.len() > 255
        || parent_device_path.as_bytes().contains(&0)
        || minimum_base != maximum_base
        || translation_offset != 0
        || !is_consumer
        || length == 0
    {
        return Ok(None);
    }
    let Some(end) = minimum_base.checked_add(length) else {
        return Ok(None);
    };
    if end > 0x1_0000 {
        return Ok(None);
    }
    let mut path = std::string::String::new();
    path.try_reserve_exact(parent_device_path.len())
        .map_err(|_| RegionError::HeapBudgetExceeded)?;
    path.push_str(parent_device_path);
    Ok(Some(SystemIoResource {
        parent_device_path: path,
        base: minimum_base,
        length,
    }))
}

/// Normalize an Extended Address Space `_CRS` descriptor into a direct I/O
/// range. WORD/DWORD descriptors do not carry consumer/producer semantics in
/// their General Flags, so callers must only pass an explicitly classified
/// Extended descriptor here.
pub fn system_io_address_space_resource_from_crs(
    parent_device_path: &str,
    minimum: u64,
    maximum: u64,
    translation_offset: u64,
    length: u64,
    is_consumer: bool,
    minimum_and_maximum_fixed: bool,
    type_specific_flags: u8,
) -> Option<SystemIoResource> {
    try_system_io_address_space_resource_from_crs(
        parent_device_path,
        minimum,
        maximum,
        translation_offset,
        length,
        is_consumer,
        minimum_and_maximum_fixed,
        type_specific_flags,
    )
    .ok()
    .flatten()
}

pub fn try_system_io_address_space_resource_from_crs(
    parent_device_path: &str,
    minimum: u64,
    maximum: u64,
    translation_offset: u64,
    length: u64,
    is_consumer: bool,
    minimum_and_maximum_fixed: bool,
    type_specific_flags: u8,
) -> Result<Option<SystemIoResource>, RegionError> {
    if !minimum_and_maximum_fixed || type_specific_flags != 0x03 || length == 0 {
        return Ok(None);
    }
    let Some(last_byte_offset) = length.checked_sub(1) else {
        return Ok(None);
    };
    let Some(last) = minimum.checked_add(last_byte_offset) else {
        return Ok(None);
    };
    if last != maximum {
        return Ok(None);
    }

    try_system_io_resource_from_crs(
        parent_device_path,
        minimum,
        minimum,
        translation_offset,
        length,
        is_consumer,
    )
}

pub fn system_io_region_is_authorized(
    parent_device_path: &str,
    base: u64,
    length: u64,
    resources: &[SystemIoResource],
) -> bool {
    let Some(end) = base.checked_add(length) else {
        return false;
    };
    if length == 0 || end > 0x1_0000 {
        return false;
    }

    resources.iter().any(|resource| {
        resource.parent_device_path == parent_device_path
            && resource.length != 0
            && resource
                .base
                .checked_add(resource.length)
                .is_some_and(|resource_end| {
                    resource.base <= base && end <= resource_end && resource_end <= 0x1_0000
                })
    })
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ResourceLease {
    pub lease_id: u64,
    pub parent_device: u64,
    pub base: u64,
    pub length: u64,
    pub page_base: u64,
    pub page_end: u64,
    pub kind: LeaseKind,
    pub cache: CacheType,
    pub generation: u64,
    pub mode: AccessMode,
}

impl ResourceLease {
    pub fn new(
        lease_id: u64,
        parent_device: u64,
        base: u64,
        length: u64,
        page_base: u64,
        page_end: u64,
        kind: LeaseKind,
        cache: CacheType,
        generation: u64,
        mode: AccessMode,
    ) -> Result<Self, RegionError> {
        let end = base.checked_add(length).ok_or(RegionError::Overflow)?;
        if length == 0
            || page_base > base
            || page_end < end
            || page_base % PAGE_SIZE != 0
            || page_end % PAGE_SIZE != 0
            || page_base >= page_end
        {
            return Err(RegionError::InvalidLease);
        }
        if kind != LeaseKind::IoPort {
            let expected = exact_page_cover(base, end)?;
            if (page_base, page_end) != expected {
                return Err(RegionError::InvalidLease);
            }
        }
        Ok(Self {
            lease_id,
            parent_device,
            base,
            length,
            page_base,
            page_end,
            kind,
            cache,
            generation,
            mode,
        })
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum RegionError {
    UnsupportedSpace,
    Overflow,
    InvalidLease,
    StaleGeneration,
    BudgetExceeded,
    WorkUnitBudgetExceeded,
    DeadlineExceeded,
    HeapBudgetExceeded,
    NoMatchingLease,
    PageCoverUnavailable,
    RightsDenied,
    InvalidWidth,
    OutOfBounds,
    Alignment,
    InvocationOutcomeUnknown,
    InvocationEndpointClosed,
    GlobalLockUnsupported,
    GlobalLockTimeout,
    GlobalLockIoError,
    InvocationReadinessError,
    InvocationUnexpectedReadiness,
    InvocationWaitStatus(u32),
    InvocationOutcome {
        execution: u32,
        reason: u32,
        protocol_error: i64,
    },
    InvocationStatus(u32),
    InvalidInvocation,
    InvalidInvocationResult,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct RegionAccessToken {
    lease_id: u64,
    space: RegionSpace,
    logical_base: u64,
    logical_length: u64,
    generation: u64,
    mode: AccessMode,
}

pub trait RegionAccessor {
    /// Return the currently live generation for this lease.  The authority
    /// must return `StaleGeneration`/`NoMatchingLease` after revoke rather
    /// than relying on the token's cached generation alone.
    fn lease_generation(&self, lease_id: u64) -> Result<u64, RegionError>;
    fn read(&self, lease_id: u64, address: u64, width: u8) -> Result<u64, RegionError>;
    fn write(&self, lease_id: u64, address: u64, width: u8, value: u64) -> Result<(), RegionError>;
}

impl RegionAccessToken {
    pub fn lease_id(&self) -> u64 {
        self.lease_id
    }

    pub fn generation(&self) -> u64 {
        self.generation
    }

    pub fn read<A: RegionAccessor + ?Sized>(
        &self,
        accessor: &A,
        offset: u64,
        width: u8,
    ) -> Result<u64, RegionError> {
        self.check_generation(accessor)?;
        let address = self.checked_address(offset, width)?;
        accessor.read(self.lease_id, address, width)
    }

    pub fn write<A: RegionAccessor + ?Sized>(
        &self,
        accessor: &A,
        offset: u64,
        width: u8,
        value: u64,
    ) -> Result<(), RegionError> {
        self.check_generation(accessor)?;
        if self.mode != AccessMode::ReadWrite {
            return Err(RegionError::RightsDenied);
        }
        let address = self.checked_address(offset, width)?;
        accessor.write(self.lease_id, address, width, value)
    }

    fn check_generation<A: RegionAccessor + ?Sized>(
        &self,
        accessor: &A,
    ) -> Result<(), RegionError> {
        match accessor.lease_generation(self.lease_id)? {
            generation if generation == self.generation => Ok(()),
            _ => Err(RegionError::StaleGeneration),
        }
    }

    fn checked_address(&self, offset: u64, width: u8) -> Result<u64, RegionError> {
        let valid_width = match self.space {
            RegionSpace::SystemIo | RegionSpace::PciConfig => matches!(width, 1 | 2 | 4),
            RegionSpace::SystemMemory => matches!(width, 1 | 2 | 4 | 8),
            RegionSpace::Unsupported(_) => false,
        };
        if !valid_width {
            return Err(RegionError::InvalidWidth);
        }
        let width = u64::from(width);
        if offset % width != 0 {
            return Err(RegionError::Alignment);
        }
        let end = offset.checked_add(width).ok_or(RegionError::Overflow)?;
        if end > self.logical_length {
            return Err(RegionError::OutOfBounds);
        }
        self.logical_base
            .checked_add(offset)
            .ok_or(RegionError::Overflow)
    }
}

pub fn authorize_region(
    region: OperationRegion,
    leases: &[ResourceLease],
    mode: AccessMode,
) -> Result<RegionAccessToken, RegionError> {
    let region_end = region
        .base
        .checked_add(region.length)
        .ok_or(RegionError::Overflow)?;
    if region.length == 0 {
        return Err(RegionError::OutOfBounds);
    }
    let page_base = region.base / PAGE_SIZE * PAGE_SIZE;
    let page_end = region_end
        .checked_add(PAGE_SIZE - 1)
        .ok_or(RegionError::Overflow)?
        / PAGE_SIZE
        * PAGE_SIZE;

    let lease = leases
        .iter()
        .find(|lease| {
            lease.generation == region.generation
                && lease.parent_device == region.parent_device
                && lease.base <= region.base
                && lease
                    .base
                    .checked_add(lease.length)
                    .is_some_and(|end| region_end <= end)
        })
        .ok_or(RegionError::NoMatchingLease)?;
    if !matches!(region.space, RegionSpace::SystemIo)
        && (lease.page_base > page_base || page_end > lease.page_end)
    {
        return Err(RegionError::PageCoverUnavailable);
    }

    let kind_allowed = match region.space {
        RegionSpace::SystemMemory => matches!(
            lease.kind,
            LeaseKind::DeviceMmio | LeaseKind::FirmwareReserved
        ),
        RegionSpace::SystemIo => matches!(lease.kind, LeaseKind::IoPort) && region_end <= 0x1_0000,
        RegionSpace::PciConfig => {
            matches!(lease.kind, LeaseKind::PciConfig) && region_end <= 0x1000
        }
        RegionSpace::Unsupported(_) => return Err(RegionError::UnsupportedSpace),
    };
    if !kind_allowed {
        return Err(RegionError::NoMatchingLease);
    }
    if mode == AccessMode::ReadWrite && lease.mode != AccessMode::ReadWrite {
        return Err(RegionError::RightsDenied);
    }
    Ok(RegionAccessToken {
        lease_id: lease.lease_id,
        space: region.space,
        logical_base: region.base,
        logical_length: region.length,
        generation: region.generation,
        mode,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn lease(kind: LeaseKind, mode: AccessMode) -> ResourceLease {
        ResourceLease::new(
            7,
            1,
            0xfee0_0000,
            0x1000,
            0xfee0_0000,
            0xfee0_1000,
            kind,
            CacheType::Uncacheable,
            4,
            mode,
        )
        .unwrap()
    }

    #[test]
    fn rejects_normal_ram_and_stale_generation() {
        let region = OperationRegion {
            space: RegionSpace::SystemMemory,
            base: 0xfee0_0000,
            length: 4,
            parent_device: 1,
            generation: 4,
        };
        let ram = ResourceLease {
            kind: LeaseKind::NormalRam,
            ..lease(LeaseKind::DeviceMmio, AccessMode::Read)
        };
        assert_eq!(
            authorize_region(region, &[ram], AccessMode::Read),
            Err(RegionError::NoMatchingLease)
        );
        let stale = ResourceLease {
            generation: 3,
            ..lease(LeaseKind::DeviceMmio, AccessMode::Read)
        };
        assert_eq!(
            authorize_region(region, &[stale], AccessMode::Read),
            Err(RegionError::NoMatchingLease)
        );
    }

    #[test]
    fn rejects_page_cover_escape_and_write_attenuation() {
        let region = OperationRegion {
            space: RegionSpace::SystemMemory,
            base: 0xfee0_0ffc,
            length: 8,
            parent_device: 1,
            generation: 4,
        };
        let split_lease = ResourceLease {
            lease_id: 7,
            parent_device: 1,
            base: 0xfee0_0ffc,
            length: 8,
            page_base: 0xfee0_0000,
            page_end: 0xfee0_1000,
            kind: LeaseKind::DeviceMmio,
            cache: CacheType::Uncacheable,
            generation: 4,
            mode: AccessMode::Read,
        };
        assert_eq!(
            authorize_region(region, &[split_lease], AccessMode::Read),
            Err(RegionError::PageCoverUnavailable)
        );
        let writable = authorize_region(
            OperationRegion {
                base: 0xfee0_0000,
                length: 4,
                ..region
            },
            &[lease(LeaseKind::DeviceMmio, AccessMode::Read)],
            AccessMode::ReadWrite,
        );
        assert_eq!(writable, Err(RegionError::RightsDenied));
    }

    #[test]
    fn page_rounded_lease_admits_a_cross_page_logical_region() {
        let region = OperationRegion {
            space: RegionSpace::SystemMemory,
            base: 0xfee0_0ffc,
            length: 8,
            parent_device: 1,
            generation: 4,
        };
        let direct_lease = ResourceLease::new(
            9,
            1,
            0xfee0_0ffc,
            8,
            0xfee0_0000,
            0xfee0_2000,
            LeaseKind::DeviceMmio,
            CacheType::Uncacheable,
            4,
            AccessMode::Read,
        )
        .unwrap();
        assert!(authorize_region(region, &[direct_lease], AccessMode::Read).is_ok());
    }

    #[test]
    fn resource_lease_rejects_non_exact_physical_page_covers() {
        let cases = [
            (0xfee0_1003, 8, 0xfee0_0000, 0xfee0_2000),
            (0xfee0_1003, 8, 0xfee0_1000, 0xfee0_3000),
            (0xfee0_1003, 8, 0xfee0_1000, 0xfee0_1000),
        ];
        for (base, length, page_base, page_end) in cases {
            assert_eq!(
                ResourceLease::new(
                    12,
                    1,
                    base,
                    length,
                    page_base,
                    page_end,
                    LeaseKind::DeviceMmio,
                    CacheType::Uncacheable,
                    4,
                    AccessMode::Read,
                ),
                Err(RegionError::InvalidLease),
            );
        }
        assert_eq!(
            ResourceLease::new(
                12,
                1,
                u64::MAX - 3,
                8,
                u64::MAX - PAGE_SIZE + 1,
                u64::MAX & !(PAGE_SIZE - 1),
                LeaseKind::DeviceMmio,
                CacheType::Uncacheable,
                4,
                AccessMode::Read,
            ),
            Err(RegionError::Overflow),
        );
    }

    #[test]
    fn token_rechecks_logical_bounds_and_width() {
        let region = OperationRegion {
            space: RegionSpace::SystemMemory,
            base: 0xfee0_0000,
            length: 8,
            parent_device: 1,
            generation: 4,
        };
        let token = authorize_region(
            region,
            &[lease(LeaseKind::DeviceMmio, AccessMode::Read)],
            AccessMode::Read,
        )
        .unwrap();
        struct Null;
        impl RegionAccessor for Null {
            fn lease_generation(&self, _: u64) -> Result<u64, RegionError> {
                Ok(4)
            }
            fn read(&self, _: u64, _: u64, _: u8) -> Result<u64, RegionError> {
                Ok(0)
            }
            fn write(&self, _: u64, _: u64, _: u8, _: u64) -> Result<(), RegionError> {
                Ok(())
            }
        }
        assert_eq!(token.read(&Null, 8, 1), Err(RegionError::OutOfBounds));
        assert_eq!(token.read(&Null, 0, 3), Err(RegionError::InvalidWidth));
    }

    #[test]
    fn pci_config_requires_a_read_only_function_page_lease() {
        let region = OperationRegion {
            space: RegionSpace::PciConfig,
            base: 0x40,
            length: 4,
            parent_device: 1,
            generation: 4,
        };
        let lease = ResourceLease::new(
            11,
            1,
            0,
            0x1000,
            0,
            PAGE_SIZE,
            LeaseKind::PciConfig,
            CacheType::Uncacheable,
            4,
            AccessMode::Read,
        )
        .unwrap();

        assert!(authorize_region(region, &[lease], AccessMode::Read).is_ok());
        assert_eq!(
            authorize_region(region, &[lease], AccessMode::ReadWrite),
            Err(RegionError::RightsDenied)
        );
        let outside_config = OperationRegion {
            base: 0xfff,
            length: 2,
            ..region
        };
        assert_eq!(
            authorize_region(outside_config, &[lease], AccessMode::Read),
            Err(RegionError::NoMatchingLease)
        );
    }

    #[test]
    fn revoked_generation_invalidates_an_existing_token() {
        struct Revoked;
        impl RegionAccessor for Revoked {
            fn lease_generation(&self, _: u64) -> Result<u64, RegionError> {
                Err(RegionError::StaleGeneration)
            }
            fn read(&self, _: u64, _: u64, _: u8) -> Result<u64, RegionError> {
                Ok(0)
            }
            fn write(&self, _: u64, _: u64, _: u8, _: u64) -> Result<(), RegionError> {
                Ok(())
            }
        }
        let token = authorize_region(
            OperationRegion {
                space: RegionSpace::SystemMemory,
                base: 0xfee0_0000,
                length: 8,
                parent_device: 1,
                generation: 4,
            },
            &[lease(LeaseKind::DeviceMmio, AccessMode::Read)],
            AccessMode::Read,
        )
        .unwrap();
        assert_eq!(
            token.read(&Revoked, 0, 1),
            Err(RegionError::StaleGeneration)
        );
    }

    #[test]
    fn system_io_uses_port_widths_without_page_cover_and_facs_is_denied() {
        let io_region = OperationRegion {
            space: RegionSpace::SystemIo,
            base: 0xfff8,
            length: 8,
            parent_device: 1,
            generation: 4,
        };
        let io_lease = ResourceLease::new(
            8,
            1,
            0xfff8,
            8,
            0xf000,
            0x10000,
            LeaseKind::IoPort,
            CacheType::Uncacheable,
            4,
            AccessMode::Read,
        )
        .unwrap();
        let token = authorize_region(io_region, &[io_lease], AccessMode::Read).unwrap();
        struct Null;
        impl RegionAccessor for Null {
            fn lease_generation(&self, _: u64) -> Result<u64, RegionError> {
                Ok(4)
            }
            fn read(&self, _: u64, _: u64, _: u8) -> Result<u64, RegionError> {
                Ok(0)
            }
            fn write(&self, _: u64, _: u64, _: u8, _: u64) -> Result<(), RegionError> {
                Ok(())
            }
        }
        assert_eq!(token.read(&Null, 0, 8), Err(RegionError::InvalidWidth));
        assert_eq!(
            token.write(&Null, 0, 1, 0xff),
            Err(RegionError::RightsDenied)
        );

        let facs = ResourceLease {
            kind: LeaseKind::FirmwareStateReserved,
            ..lease(LeaseKind::FirmwareStateReserved, AccessMode::Read)
        };
        assert_eq!(
            authorize_region(
                OperationRegion {
                    space: RegionSpace::SystemMemory,
                    base: 0xfee0_0000,
                    length: 4,
                    parent_device: 1,
                    generation: 4,
                },
                &[facs],
                AccessMode::Read,
            ),
            Err(RegionError::NoMatchingLease)
        );
    }

    #[test]
    fn system_io_authority_requires_parent_and_full_crs_range_containment() {
        let resources = [SystemIoResource {
            parent_device_path: "\\_SB.PCI0.COM1".to_string(),
            base: 0x3f8,
            length: 8,
        }];

        assert!(system_io_region_is_authorized(
            "\\_SB.PCI0.COM1",
            0x3f9,
            2,
            &resources
        ));
        assert!(!system_io_region_is_authorized(
            "\\_SB.PCI0.KBD_",
            0x3f9,
            2,
            &resources
        ));
        assert!(!system_io_region_is_authorized(
            "\\_SB.PCI0.COM1",
            0x3ff,
            2,
            &resources
        ));
        assert!(!system_io_region_is_authorized(
            "\\_SB.PCI0.COM1",
            0xffff,
            2,
            &resources
        ));
        assert!(!system_io_region_is_authorized(
            "\\_SB.PCI0.COM1",
            0x3f8,
            0,
            &resources
        ));
    }

    #[test]
    fn system_io_crs_normalization_rejects_ambiguous_or_invalid_ranges() {
        let resource =
            system_io_resource_from_crs("\\_SB.PCI0.COM1", 0x3f8, 0x3f8, 0, 8, true).unwrap();
        assert_eq!(resource.base, 0x3f8);
        assert_eq!(resource.length, 8);
        assert_eq!(resource.parent_device_path, "\\_SB.PCI0.COM1");

        assert!(system_io_resource_from_crs("\\_SB.PCI0.COM1", 0x3f8, 0x400, 0, 8, true).is_none());
        assert!(system_io_resource_from_crs("\\_SB.PCI0.COM1", 0x3f8, 0x3f8, 1, 8, true).is_none());
        assert!(
            system_io_resource_from_crs("\\_SB.PCI0.COM1", 0x3f8, 0x3f8, 0, 8, false).is_none()
        );
        assert!(
            system_io_resource_from_crs("\\_SB.PCI0.COM1", 0xffff, 0xffff, 0, 2, true).is_none()
        );
        assert!(system_io_resource_from_crs("\\_SB.PCI0.COM1", 0x3f8, 0x3f8, 0, 0, true).is_none());
        assert!(system_io_resource_from_crs("COM1", 0x3f8, 0x3f8, 0, 8, true).is_none());
    }

    #[test]
    fn fallible_system_io_crs_normalization_preserves_validity_rules() {
        let resource =
            try_system_io_resource_from_crs("\\_SB_.PCI0.COM1", 0x3f8, 0x3f8, 0, 8, true)
                .unwrap()
                .unwrap();
        assert_eq!(resource.parent_device_path, "\\_SB_.PCI0.COM1");
        assert_eq!(resource.base, 0x3f8);
        assert_eq!(resource.length, 8);
        assert_eq!(
            try_system_io_resource_from_crs("COM1", 0x3f8, 0x3f8, 0, 8, true),
            Ok(None)
        );
    }

    #[test]
    fn system_io_address_space_crs_requires_fixed_consumer_direct_full_range() {
        let resource = system_io_address_space_resource_from_crs(
            "\\_SB.PCI0.COM1",
            0x3f8,
            0x3ff,
            0,
            8,
            true,
            true,
            0x03,
        )
        .unwrap();
        assert_eq!(resource.base, 0x3f8);
        assert_eq!(resource.length, 8);

        assert!(
            system_io_address_space_resource_from_crs(
                "\\_SB.PCI0.COM1",
                0x3f8,
                0x3ff,
                0,
                8,
                false,
                true,
                0x03,
            )
            .is_none()
        );
        assert!(
            system_io_address_space_resource_from_crs(
                "\\_SB.PCI0.COM1",
                0x3f8,
                0x3ff,
                0,
                8,
                true,
                false,
                0x03,
            )
            .is_none()
        );
        assert!(
            system_io_address_space_resource_from_crs(
                "\\_SB.PCI0.COM1",
                0x3f8,
                0x3ff,
                0,
                8,
                true,
                true,
                0x13,
            )
            .is_none()
        );
        assert!(
            system_io_address_space_resource_from_crs(
                "\\_SB.PCI0.COM1",
                0x3f8,
                0x3ff,
                1,
                8,
                true,
                true,
                0x03,
            )
            .is_none()
        );
        assert!(
            system_io_address_space_resource_from_crs(
                "\\_SB.PCI0.COM1",
                0x3f8,
                0x0400,
                0,
                8,
                true,
                true,
                0x03,
            )
            .is_none()
        );
        assert!(
            system_io_address_space_resource_from_crs(
                "\\_SB.PCI0.COM1",
                0xffff,
                0xffff,
                0,
                2,
                true,
                true,
                0x03,
            )
            .is_none()
        );
    }
}
