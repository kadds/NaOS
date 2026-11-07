use naos_idl::acpi_fixed_hardware::{AcpiFixedHardwareInfo, AcpiSciAck, AcpiSciEvent};

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum SciWaitOutcome {
    Readable,
    TimerElapsed,
    Failed,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum SciWaitAction {
    Drain,
    Continue,
    DisableAndExit,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub(crate) struct SciGenerationLineage {
    pub(crate) firmware_version: String,
    pub(crate) aml_namespace_generation: u64,
    pub(crate) platform_topology_generation: u64,
    pub(crate) pci_fabric_generation: u64,
}

impl SciGenerationLineage {
    pub(crate) fn from_info(info: &AcpiFixedHardwareInfo, firmware_version: &str) -> Self {
        Self {
            firmware_version: firmware_version.to_owned(),
            aml_namespace_generation: info.aml_namespace_generation,
            platform_topology_generation: info.platform_topology_generation,
            pci_fabric_generation: info.pci_fabric_generation,
        }
    }
    pub(crate) fn matches_event(&self, event: &AcpiSciEvent, firmware_version: &str) -> bool {
        self.firmware_version == firmware_version
            && self.aml_namespace_generation == event.aml_namespace_generation
            && self.platform_topology_generation == event.platform_topology_generation
            && self.pci_fabric_generation == event.pci_fabric_generation
    }

    pub(crate) fn matches_ack(&self, ack: &AcpiSciAck, firmware_version: &str) -> bool {
        self.firmware_version == firmware_version
            && self.aml_namespace_generation == ack.aml_namespace_generation
            && self.platform_topology_generation == ack.platform_topology_generation
            && self.pci_fabric_generation == ack.pci_fabric_generation
    }
}

pub(crate) const fn classify_sci_wait(outcome: SciWaitOutcome) -> SciWaitAction {
    match outcome {
        SciWaitOutcome::Readable => SciWaitAction::Drain,
        SciWaitOutcome::TimerElapsed => SciWaitAction::Continue,
        SciWaitOutcome::Failed => SciWaitAction::DisableAndExit,
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum GpeBank {
    Gpe0,
    Gpe1,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) struct GpeEvent {
    pub(crate) bank: GpeBank,
    pub(crate) number: u32,
    pub(crate) mask: u64,
}

#[derive(Debug)]
pub(crate) struct GpeEventIter {
    bank: GpeBank,
    pending: u64,
    number_base: u32,
}

impl GpeEventIter {
    pub(crate) fn all_events(
        bank: GpeBank,
        block_bytes: u32,
        number_base: u32,
    ) -> Result<Self, GpeEnumerationError> {
        if block_bytes % 2 != 0 || block_bytes / 2 > 8 {
            return Err(GpeEnumerationError::InvalidBlockLength);
        }
        let status_bits = (block_bytes / 2) * 8;
        let status = match status_bits {
            0 => 0,
            64 => u64::MAX,
            bits => (1_u64 << bits) - 1,
        };
        Self::new(bank, status, block_bytes, number_base)
    }

    pub(crate) fn new(
        bank: GpeBank,
        status: u64,
        block_bytes: u32,
        number_base: u32,
    ) -> Result<Self, GpeEnumerationError> {
        if block_bytes % 2 != 0 || block_bytes / 2 > 8 {
            return Err(GpeEnumerationError::InvalidBlockLength);
        }
        let status_bytes = block_bytes / 2;
        let status_bits = status_bytes * 8;
        if status_bits < 64 && status >> status_bits != 0 {
            return Err(GpeEnumerationError::StatusOutsideBlock);
        }

        Ok(Self {
            bank,
            pending: status,
            number_base,
        })
    }

    pub(crate) fn next_event(&mut self) -> Result<Option<GpeEvent>, GpeEnumerationError> {
        if self.pending == 0 {
            return Ok(None);
        }
        let bit = self.pending.trailing_zeros();
        let number = self
            .number_base
            .checked_add(bit)
            .filter(|number| *number <= u32::from(u8::MAX))
            .ok_or(GpeEnumerationError::NumberOutOfRange)?;
        let mask = 1_u64 << bit;
        self.pending &= !mask;
        Ok(Some(GpeEvent {
            bank: self.bank,
            number,
            mask,
        }))
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum GpeEnumerationError {
    InvalidBlockLength,
    StatusOutsideBlock,
    NumberOutOfRange,
}

pub(crate) const PM1_TIMER_STATUS_MASK: u64 = 1;
pub(crate) const PM1_GLOBAL_LOCK_STATUS_MASK: u64 = 1 << 5;
const PM1_SAFE_ACK_STATUS_MASK: u64 = PM1_TIMER_STATUS_MASK | PM1_GLOBAL_LOCK_STATUS_MASK;

pub(crate) const fn pm1_safe_ack_status(status: u64) -> u64 {
    status & PM1_SAFE_ACK_STATUS_MASK
}

pub(crate) const fn pm1_has_unhandled_status(pm1_a: u64, pm1_b: u64) -> bool {
    (pm1_a & !PM1_SAFE_ACK_STATUS_MASK) != 0 || (pm1_b & !PM1_SAFE_ACK_STATUS_MASK) != 0
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum GpeEoiTiming {
    BeforeHandler,
    AfterHandler,
}

pub(crate) const fn gpe_eoi_timing(edge_triggered: bool) -> GpeEoiTiming {
    if edge_triggered {
        GpeEoiTiming::BeforeHandler
    } else {
        GpeEoiTiming::AfterHandler
    }
}

#[cfg(test)]
mod tests {
    use super::{
        GpeBank, GpeEnumerationError, GpeEoiTiming, GpeEventIter, PM1_GLOBAL_LOCK_STATUS_MASK,
        PM1_TIMER_STATUS_MASK, SciGenerationLineage, SciWaitAction, SciWaitOutcome,
        classify_sci_wait, gpe_eoi_timing, pm1_has_unhandled_status, pm1_safe_ack_status,
    };

    #[test]
    fn selector_failure_requires_fail_stop_exit_instead_of_retrying_or_parking() {
        assert_eq!(
            classify_sci_wait(SciWaitOutcome::Failed),
            SciWaitAction::DisableAndExit
        );
    }

    #[test]
    fn sci_event_and_ack_must_match_every_incarnation_generation() {
        use naos_idl::acpi_fixed_hardware::{AcpiSciAck, AcpiSciEvent};

        let lineage = SciGenerationLineage {
            firmware_version: "test-firmware-v1".to_owned(),
            aml_namespace_generation: 13,
            platform_topology_generation: 14,
            pci_fabric_generation: 15,
        };
        let event = AcpiSciEvent {
            sequence: 1,
            flags: 0,
            pm1_event_status: 0,
            gpe0_status: 0,
            gpe1_status: 0,
            pm1_event_status_b: 0,
            aml_namespace_generation: lineage.aml_namespace_generation,
            platform_topology_generation: lineage.platform_topology_generation,
            pci_fabric_generation: lineage.pci_fabric_generation,
        };
        let ack = AcpiSciAck {
            sequence: 1,
            state: 0,
            flags: 0,
            aml_namespace_generation: lineage.aml_namespace_generation,
            platform_topology_generation: lineage.platform_topology_generation,
            pci_fabric_generation: lineage.pci_fabric_generation,
        };
        assert!(lineage.matches_event(&event, "test-firmware-v1"));
        assert!(lineage.matches_ack(&ack, "test-firmware-v1"));

        for field in 0..4 {
            let mut stale_event = event.clone();
            let mut stale_ack = ack.clone();
            match field {
                0 => {
                    stale_event.aml_namespace_generation += 1;
                    stale_ack.aml_namespace_generation += 1;
                }
                1 => {
                    stale_event.platform_topology_generation += 1;
                    stale_ack.platform_topology_generation += 1;
                }
                2 => {
                    stale_event.pci_fabric_generation += 1;
                    stale_ack.pci_fabric_generation += 1;
                }
                3 => {}
                _ => unreachable!(),
            }
            let version = if field == 3 {
                "stale-firmware-v1"
            } else {
                "test-firmware-v1"
            };
            assert!(!lineage.matches_event(&stale_event, version));
            assert!(!lineage.matches_ack(&stale_ack, version));
        }
    }

    #[test]
    fn readable_interrupt_drains_and_timeout_keeps_waiting() {
        assert_eq!(
            classify_sci_wait(SciWaitOutcome::Readable),
            SciWaitAction::Drain
        );
        assert_eq!(
            classify_sci_wait(SciWaitOutcome::TimerElapsed),
            SciWaitAction::Continue
        );
    }

    #[test]
    fn gpe_status_bits_map_to_bounded_numbers_for_each_bank() {
        let mut gpe0 = GpeEventIter::new(GpeBank::Gpe0, (1_u64 << 63) | 0x5, 16, 0).unwrap();
        assert_eq!(gpe0.next_event().unwrap().unwrap().number, 0);
        assert_eq!(gpe0.next_event().unwrap().unwrap().mask, 4);
        let last = gpe0.next_event().unwrap().unwrap();
        assert_eq!(last.bank, GpeBank::Gpe0);
        assert_eq!(last.number, 63);
        assert_eq!(last.mask, 1_u64 << 63);
        assert_eq!(gpe0.next_event().unwrap(), None);

        let mut gpe1 = GpeEventIter::new(GpeBank::Gpe1, 0x14, 8, 0x40).unwrap();
        assert_eq!(gpe1.next_event().unwrap().unwrap().number, 0x42);
        let last = gpe1.next_event().unwrap().unwrap();
        assert_eq!(last.number, 0x44);
        assert_eq!(last.bank, GpeBank::Gpe1);
        assert_eq!(gpe1.next_event().unwrap(), None);
    }

    #[test]
    fn gpe_event_iterator_is_bounded_by_the_two_fixed_status_banks() {
        let mut events = GpeEventIter::new(GpeBank::Gpe0, u64::MAX, 16, 0).unwrap();
        let mut count = 0;
        while events.next_event().unwrap().is_some() {
            count += 1;
        }
        assert_eq!(count, 64);
    }

    #[test]
    fn all_gpe_candidates_cover_only_the_declared_status_bits() {
        let mut candidates = GpeEventIter::all_events(GpeBank::Gpe1, 8, 0x40).unwrap();
        let first = candidates.next_event().unwrap().unwrap();
        assert_eq!(first.number, 0x40);
        assert_eq!(first.mask, 1);

        let mut last = first;
        let mut count = 1;
        while let Some(event) = candidates.next_event().unwrap() {
            last = event;
            count += 1;
        }
        assert_eq!(count, 32);
        assert_eq!(last.number, 0x5f);
        assert_eq!(last.mask, 1_u64 << 31);

        assert_eq!(
            GpeEventIter::all_events(GpeBank::Gpe0, 3, 0).unwrap_err(),
            GpeEnumerationError::InvalidBlockLength
        );
        assert_eq!(
            GpeEventIter::all_events(GpeBank::Gpe0, 18, 0).unwrap_err(),
            GpeEnumerationError::InvalidBlockLength
        );
    }

    #[test]
    fn gpe_edge_status_is_cleared_before_and_level_status_after_handler() {
        assert_eq!(gpe_eoi_timing(true), GpeEoiTiming::BeforeHandler);
        assert_eq!(gpe_eoi_timing(false), GpeEoiTiming::AfterHandler);
    }

    #[test]
    fn gpe_status_mapping_rejects_malformed_widths_and_unrepresentable_numbers() {
        assert_eq!(
            GpeEventIter::new(GpeBank::Gpe0, 1, 3, 0).unwrap_err(),
            GpeEnumerationError::InvalidBlockLength
        );
        assert_eq!(
            GpeEventIter::new(GpeBank::Gpe0, 1_u64 << 16, 4, 0).unwrap_err(),
            GpeEnumerationError::StatusOutsideBlock
        );
        let mut out_of_range = GpeEventIter::new(GpeBank::Gpe1, 1 << 2, 8, 0xfe).unwrap();
        assert_eq!(
            out_of_range.next_event(),
            Err(GpeEnumerationError::NumberOutOfRange)
        );
    }

    #[test]
    fn only_pm1_timer_and_kernel_global_lock_status_are_safely_acknowledgeable_without_a_fixed_handler()
     {
        assert!(!pm1_has_unhandled_status(0, 0));
        assert!(!pm1_has_unhandled_status(PM1_TIMER_STATUS_MASK, 0));
        assert!(!pm1_has_unhandled_status(0, PM1_TIMER_STATUS_MASK));
        assert!(!pm1_has_unhandled_status(PM1_GLOBAL_LOCK_STATUS_MASK, 0));
        assert!(!pm1_has_unhandled_status(0, PM1_GLOBAL_LOCK_STATUS_MASK));
        assert!(pm1_has_unhandled_status(1 << 4, 0));
        assert!(pm1_has_unhandled_status(0, 1 << 8));
        assert!(pm1_has_unhandled_status(
            PM1_TIMER_STATUS_MASK | (1 << 4),
            0
        ));
        assert_eq!(
            pm1_safe_ack_status(PM1_TIMER_STATUS_MASK | PM1_GLOBAL_LOCK_STATUS_MASK | (1 << 4)),
            PM1_TIMER_STATUS_MASK | PM1_GLOBAL_LOCK_STATUS_MASK
        );
    }
}
