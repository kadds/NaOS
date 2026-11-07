//! Native AML region dispatch backed by NaOS capability-relative tokens.
//!
//! This module is the adapter boundary, not a hardware authority.  A
//! `NativeRegionBackend` is supplied by the platform executor and must obtain
//! its lease from the kernel-owned resource ledger.  The adapter never accepts
//! a physical address on its own and never calls the upstream raw `Handler`
//! callbacks.

use crate::HardwareSideEffectProfile;
use crate::region::{AccessMode, RegionAccessToken, RegionAccessor, RegionError};
use acpi::aml::allocator::{self as aml_allocator, Arc as AmlArc, String as AmlString};
use acpi::aml::{
    AmlError, ExecutionMeter,
    op_region::{
        NativeRegionAccessMode, NativeRegionHandler, NativeRegionToken, OpRegion, RegionSpace,
    },
};
use std::{
    collections::HashMap,
    string::String,
    sync::atomic::{AtomicBool, AtomicU8, AtomicU64, Ordering},
    sync::{Arc, Mutex},
};

const MAX_CACHED_REGION_TOKENS: usize = 4096;
const MAX_REGION_PATH_BYTES: usize = 1281;
const EXECUTION_NONE: u32 = 0;
const EXECUTION_OUTCOME_UNKNOWN: u32 = 2;

fn is_unknown_region_outcome(error: RegionError) -> bool {
    match error {
        RegionError::InvocationOutcomeUnknown => true,
        RegionError::InvocationOutcome {
            execution,
            protocol_error,
            ..
        } => {
            execution == EXECUTION_OUTCOME_UNKNOWN
                || (execution == EXECUTION_NONE && protocol_error == 0)
        }
        _ => false,
    }
}

fn map_execution_meter_error(error: AmlError) -> RegionError {
    match error {
        AmlError::BudgetExceeded => RegionError::WorkUnitBudgetExceeded,
        AmlError::DeadlineExceeded => RegionError::DeadlineExceeded,
        AmlError::HostError(message) if message.contains("AML_DEADLINE_EXCEEDED") => {
            RegionError::DeadlineExceeded
        }
        _ => RegionError::NoMatchingLease,
    }
}

/// Kernel-backed operation-region authority.
///
/// `authorize` must resolve the opaque AML parent identity, validate the
/// address-space-specific lease and return a token.  The `RegionAccessor`
/// methods are the data plane: a direct-map backend may perform the volatile
/// load/store there, while a mediated backend may perform a bounded scalar
/// operation.  Neither path receives an unvalidated physical address from
/// the AML interpreter.
pub trait NativeRegionBackend: RegionAccessor + Send + Sync {
    /// Return the currently live resource generation for this exact region
    /// identity. The value is part of the token-cache key, not merely a
    /// late access check.
    fn resource_generation(
        &self,
        region: &OpRegion,
        parent_device_path: &str,
        namespace_generation: u64,
        mode: AccessMode,
    ) -> Result<u64, RegionError>;

    fn authorize(
        &self,
        region: &OpRegion,
        parent_device_path: &str,
        namespace_generation: u64,
        mode: AccessMode,
    ) -> Result<RegionAccessToken, RegionError>;

    /// Drop the authority backing a cached token. Direct mappings must be
    /// unmapped before the lease is revoked; mediated backends may complete a
    /// typed release transaction here.
    fn release(&self, _lease_id: u64) -> Result<(), RegionError> {
        Ok(())
    }

    /// Use the kernel-owned FACS Global Lock service. Implementations must not
    /// map or expose the FACS lock word to the AML interpreter.
    fn acquire_global_lock(&self, _timeout_us: u64) -> Result<(), RegionError> {
        Err(RegionError::GlobalLockUnsupported)
    }

    fn release_global_lock(&self) -> Result<(), RegionError> {
        Err(RegionError::GlobalLockUnsupported)
    }

    fn execution_thread_id(&self) -> Option<u64> {
        None
    }
}

/// Per-method and aggregate work-unit limits for scalar native accesses.
pub struct NativeAccessBudget {
    maximum: u64,
    remaining: AtomicU64,
    work_unit_maximum: u64,
    work_unit_remaining: AtomicU64,
    work_unit_active: AtomicBool,
}

impl NativeAccessBudget {
    pub fn new(maximum: u64, work_unit_maximum: u64) -> Self {
        Self {
            maximum,
            remaining: AtomicU64::new(maximum),
            work_unit_maximum,
            work_unit_remaining: AtomicU64::new(work_unit_maximum),
            work_unit_active: AtomicBool::new(false),
        }
    }

    pub fn reset(&self) {
        self.remaining.store(self.maximum, Ordering::Release);
    }

    pub fn begin_work_unit(&self) -> Result<(), RegionError> {
        if self.work_unit_active.swap(true, Ordering::AcqRel) {
            return Err(RegionError::BudgetExceeded);
        }
        self.work_unit_remaining
            .store(self.work_unit_maximum, Ordering::Release);
        Ok(())
    }

    pub fn end_work_unit(&self) {
        self.work_unit_active.store(false, Ordering::Release);
    }

    fn charge_counter(counter: &AtomicU64) -> Result<(), RegionError> {
        let mut remaining = counter.load(Ordering::Acquire);
        loop {
            if remaining == 0 {
                return Err(RegionError::BudgetExceeded);
            }
            match counter.compare_exchange_weak(
                remaining,
                remaining - 1,
                Ordering::AcqRel,
                Ordering::Acquire,
            ) {
                Ok(_) => return Ok(()),
                Err(observed) => remaining = observed,
            }
        }
    }

    fn charge(&self) -> Result<(), RegionError> {
        Self::charge_counter(&self.remaining)?;
        if self.work_unit_active.load(Ordering::Acquire) {
            Self::charge_counter(&self.work_unit_remaining)
                .map_err(|_| RegionError::WorkUnitBudgetExceeded)?;
        }
        Ok(())
    }
}

#[derive(Clone, Debug, Eq, Hash, Ord, PartialEq, PartialOrd)]
struct RegionKey {
    space: u16,
    base: u64,
    length: u64,
    namespace_generation: u64,
    resource_generation: u64,
    parent_device_path: AmlString,
    mode: u8,
}

impl RegionKey {
    fn new(
        region: &OpRegion,
        namespace_generation: u64,
        resource_generation: u64,
        mode: AccessMode,
        parent_device_path: AmlString,
    ) -> Self {
        let space = match region.space {
            acpi::aml::op_region::RegionSpace::SystemMemory => 0,
            acpi::aml::op_region::RegionSpace::SystemIO => 1,
            acpi::aml::op_region::RegionSpace::PciConfig => 2,
            acpi::aml::op_region::RegionSpace::EmbeddedControl => 3,
            acpi::aml::op_region::RegionSpace::SmBus => 4,
            acpi::aml::op_region::RegionSpace::SystemCmos => 5,
            acpi::aml::op_region::RegionSpace::PciBarTarget => 6,
            acpi::aml::op_region::RegionSpace::Ipmi => 7,
            acpi::aml::op_region::RegionSpace::GeneralPurposeIo => 8,
            acpi::aml::op_region::RegionSpace::GenericSerialBus => 9,
            acpi::aml::op_region::RegionSpace::Pcc => 10,
            acpi::aml::op_region::RegionSpace::Oem(value) => 0x100 + u16::from(value),
        };
        Self {
            space,
            base: region.base,
            length: region.length,
            namespace_generation,
            resource_generation,
            parent_device_path,
            mode: match mode {
                AccessMode::Read => 0,
                AccessMode::ReadWrite => 1,
            },
        }
    }
}

struct CachedRegionToken {
    token: Arc<RegionAccessToken>,
    heap_bytes: u64,
    usable: bool,
}

struct MeteredHeapReservation {
    meter: Option<AmlArc<dyn ExecutionMeter>>,
    bytes: u64,
    working: bool,
    generation: Option<u64>,
    retained: bool,
}

impl MeteredHeapReservation {
    fn working(meter: Option<AmlArc<dyn ExecutionMeter>>, bytes: u64) -> Result<Self, RegionError> {
        let generation = meter
            .as_ref()
            .map(|meter| {
                meter
                    .reserve_working_heap_scoped(bytes)
                    .map_err(|_| RegionError::HeapBudgetExceeded)
            })
            .transpose()?;
        Ok(Self {
            meter,
            bytes,
            working: true,
            generation,
            retained: false,
        })
    }

    fn persistent(
        meter: Option<AmlArc<dyn ExecutionMeter>>,
        bytes: u64,
    ) -> Result<Self, RegionError> {
        if let Some(meter) = &meter {
            meter
                .reserve_heap(bytes)
                .map_err(|_| RegionError::HeapBudgetExceeded)?;
        }
        Ok(Self {
            meter,
            bytes,
            working: false,
            generation: None,
            retained: false,
        })
    }

    fn commit_working(&mut self) -> Result<(), RegionError> {
        if self.working {
            if let Some(meter) = &self.meter {
                meter
                    .commit_working_heap_scoped(self.bytes, self.generation.unwrap_or(0))
                    .map_err(|_| RegionError::HeapBudgetExceeded)?;
            }
            self.working = false;
        }
        Ok(())
    }

    fn retain(mut self) {
        self.retained = true;
    }
}

impl Drop for MeteredHeapReservation {
    fn drop(&mut self) {
        if self.retained {
            return;
        }
        if let Some(meter) = &self.meter {
            if self.working {
                meter.release_working_heap_scoped(self.bytes, self.generation.unwrap_or(0));
            } else {
                let _ = meter.release_heap(self.bytes);
            }
        }
    }
}

fn region_token_heap_bytes() -> Option<u64> {
    // HashMap growth can temporarily keep both the old and new table alive;
    // reserve generous per-entry slack for load-factor and resize overhead.
    let entry_bytes = core::mem::size_of::<(RegionKey, CachedRegionToken)>()
        .checked_mul(12)?
        .checked_add(128)?;
    u64::try_from(entry_bytes).ok()
}

/// The cached token is an internal capability reference.  It is never
/// serialized or handed to AML callers.  A stale/revoked token is removed,
/// and the next access must perform a fresh authorization transaction.
pub struct NativeRegionAdapter<B: NativeRegionBackend + ?Sized + 'static> {
    backend: Arc<B>,
    state: Mutex<NativeRegionState>,
    native_access_budget: Arc<NativeAccessBudget>,
    native_access_attempted: Arc<AtomicBool>,
    native_access_authorized: Arc<AtomicBool>,
    hardware_side_effect_profile: Arc<AtomicU8>,
    heap_meter: Option<AmlArc<dyn ExecutionMeter>>,
}

struct NativeRegionState {
    namespace_generation: u64,
    invalidating: bool,
    outcome_unknown: bool,
    tokens: HashMap<RegionKey, CachedRegionToken>,
}

struct InvalidationGuard<'a> {
    state: &'a Mutex<NativeRegionState>,
}

impl Drop for InvalidationGuard<'_> {
    fn drop(&mut self) {
        if let Ok(mut state) = self.state.lock() {
            state.invalidating = false;
        }
    }
}

impl<B> NativeRegionAdapter<B>
where
    B: NativeRegionBackend + ?Sized + 'static,
{
    #[cfg(test)]
    fn new_with_profile(
        backend: Arc<B>,
        namespace_generation: u64,
        native_access_budget: Arc<NativeAccessBudget>,
        native_access_attempted: Arc<AtomicBool>,
        native_access_authorized: Arc<AtomicBool>,
        hardware_side_effect_profile: HardwareSideEffectProfile,
    ) -> Self {
        Self::new_with_profile_cell_and_meter(
            backend,
            namespace_generation,
            native_access_budget,
            native_access_attempted,
            native_access_authorized,
            Arc::new(AtomicU8::new(hardware_side_effect_profile.to_bits())),
            None,
        )
    }

    pub fn new_with_profile_and_meter(
        backend: Arc<B>,
        namespace_generation: u64,
        native_access_budget: Arc<NativeAccessBudget>,
        native_access_attempted: Arc<AtomicBool>,
        native_access_authorized: Arc<AtomicBool>,
        hardware_side_effect_profile: HardwareSideEffectProfile,
        heap_meter: AmlArc<dyn ExecutionMeter>,
    ) -> Self {
        Self::new_with_profile_cell_and_meter(
            backend,
            namespace_generation,
            native_access_budget,
            native_access_attempted,
            native_access_authorized,
            Arc::new(AtomicU8::new(hardware_side_effect_profile.to_bits())),
            Some(heap_meter),
        )
    }

    pub(crate) fn new_with_profile_cell_and_meter(
        backend: Arc<B>,
        namespace_generation: u64,
        native_access_budget: Arc<NativeAccessBudget>,
        native_access_attempted: Arc<AtomicBool>,
        native_access_authorized: Arc<AtomicBool>,
        hardware_side_effect_profile: Arc<AtomicU8>,
        heap_meter: Option<AmlArc<dyn ExecutionMeter>>,
    ) -> Self {
        Self {
            backend,
            state: Mutex::new(NativeRegionState {
                namespace_generation,
                invalidating: false,
                outcome_unknown: false,
                tokens: HashMap::new(),
            }),
            native_access_budget,
            native_access_attempted,
            native_access_authorized,
            hardware_side_effect_profile,
            heap_meter,
        }
    }

    fn allows_access(&self, region: &OpRegion, mode: AccessMode) -> bool {
        let profile = HardwareSideEffectProfile::from_bits(
            self.hardware_side_effect_profile.load(Ordering::Acquire),
        );
        let address_space_allowed = match region.space {
            RegionSpace::SystemMemory => profile.allow_system_memory,
            RegionSpace::SystemIO => profile.allow_system_io,
            RegionSpace::PciConfig => profile.allow_pci_config,
            _ => false,
        };
        address_space_allowed && (mode == AccessMode::Read || profile.allow_writes)
    }

    fn charge_native_access(&self) -> Result<(), RegionError> {
        self.native_access_budget.charge()
    }

    fn check_deadline(&self) -> Result<(), RegionError> {
        if let Some(meter) = &self.heap_meter {
            meter.check_deadline().map_err(map_execution_meter_error)?;
        }
        Ok(())
    }

    fn check_outcome_unknown(&self) -> Result<(), AmlError> {
        let state = self
            .state
            .lock()
            .map_err(|_| AmlError::NativeRegionOutcomeUnknown)?;
        if state.outcome_unknown {
            Err(AmlError::NativeRegionOutcomeUnknown)
        } else {
            Ok(())
        }
    }

    fn mark_outcome_unknown(&self) {
        if let Ok(mut state) = self.state.lock() {
            state.outcome_unknown = true;
            for cached in state.tokens.values_mut() {
                cached.usable = false;
            }
        }
    }

    fn release_heap(&self, bytes: u64) -> Result<(), RegionError> {
        if let Some(meter) = &self.heap_meter {
            meter
                .release_heap(bytes)
                .map_err(|_| RegionError::HeapBudgetExceeded)?;
        }
        Ok(())
    }

    fn begin_invalidation(&self) -> Result<InvalidationGuard<'_>, RegionError> {
        let mut state = self
            .state
            .lock()
            .map_err(|_| RegionError::NoMatchingLease)?;
        if state.invalidating {
            return Err(RegionError::NoMatchingLease);
        }
        if state.outcome_unknown {
            return Err(RegionError::InvocationOutcomeUnknown);
        }
        state.invalidating = true;
        Ok(InvalidationGuard { state: &self.state })
    }

    /// Invalidate all cached region tokens after namespace recovery or a
    /// resource-generation transition. Call this before dropping the adapter
    /// while its backend remains live; `Drop` does not issue blocking IPC.
    pub fn invalidate_all(&self) -> Result<(), RegionError> {
        let _invalidation = self.begin_invalidation()?;
        self.invalidate_tokens_while_exclusive()
    }

    fn restore_invalidation_entries(
        &self,
        current: Option<(RegionKey, CachedRegionToken)>,
        remaining: impl Iterator<Item = (RegionKey, CachedRegionToken)>,
    ) -> Result<(), RegionError> {
        let mut state = self
            .state
            .lock()
            .map_err(|_| RegionError::NoMatchingLease)?;
        if let Some((key, cached)) = current {
            state.tokens.insert(key, cached);
        }
        state.tokens.extend(remaining);
        Ok(())
    }

    fn invalidate_tokens_while_exclusive(&self) -> Result<(), RegionError> {
        let (entries, _scratch_reservation) = {
            let mut state = self
                .state
                .lock()
                .map_err(|_| RegionError::NoMatchingLease)?;
            let count = state.tokens.len();
            if count == 0 {
                return Ok(());
            }
            let scratch_bytes = count
                .checked_mul(core::mem::size_of::<(RegionKey, CachedRegionToken)>())
                .and_then(|bytes| {
                    bytes.checked_add(core::mem::size_of::<Vec<(RegionKey, CachedRegionToken)>>())
                })
                .and_then(|bytes| bytes.checked_add(24))
                .and_then(|bytes| u64::try_from(bytes).ok())
                .ok_or(RegionError::HeapBudgetExceeded)?;
            let scratch_reservation =
                MeteredHeapReservation::working(self.heap_meter.clone(), scratch_bytes)?;
            let mut entries = Vec::new();
            entries
                .try_reserve_exact(count)
                .map_err(|_| RegionError::HeapBudgetExceeded)?;
            entries.extend(state.tokens.drain());
            (entries, scratch_reservation)
        };

        let mut entries = entries.into_iter();
        while let Some((key, cached)) = entries.next() {
            if let Some(meter) = &self.heap_meter {
                if let Err(error) = meter
                    .loop_iteration()
                    .map_err(map_execution_meter_error)
                    .and_then(|()| meter.check_deadline().map_err(map_execution_meter_error))
                {
                    self.restore_invalidation_entries(Some((key, cached)), entries)?;
                    return Err(error);
                }
            }
            if let Err(error) = self.backend.release(cached.token.lease_id()) {
                if is_unknown_region_outcome(error) {
                    self.mark_outcome_unknown();
                }
                self.restore_invalidation_entries(Some((key, cached)), entries)?;
                return Err(error);
            }
            if let Err(error) = self.release_heap(cached.heap_bytes) {
                self.restore_invalidation_entries(None, entries)?;
                return Err(error);
            }
            if let Some(meter) = &self.heap_meter {
                if let Err(error) = meter.check_deadline().map_err(map_execution_meter_error) {
                    self.restore_invalidation_entries(None, entries)?;
                    return Err(error);
                }
            }
        }
        Ok(())
    }

    /// Switch to a recovered namespace generation and discard all tokens from
    /// the previous namespace before any new native access is admitted.
    pub fn set_namespace_generation(&self, generation: u64) -> Result<(), RegionError> {
        let _invalidation = self.begin_invalidation()?;
        self.invalidate_tokens_while_exclusive()?;
        self.state
            .lock()
            .map_err(|_| RegionError::NoMatchingLease)?
            .namespace_generation = generation;
        Ok(())
    }

    fn token_for(
        &self,
        region: &OpRegion,
        mode: AccessMode,
    ) -> Result<Arc<RegionAccessToken>, RegionError> {
        if !self.allows_access(region, mode) {
            return Err(RegionError::RightsDenied);
        }
        let mut state = self
            .state
            .lock()
            .map_err(|_| RegionError::NoMatchingLease)?;
        if state.invalidating {
            return Err(RegionError::NoMatchingLease);
        }
        let namespace_generation = state.namespace_generation;
        let path_length = region
            .parent_device_path
            .encoded_len()
            .filter(|length| *length <= MAX_REGION_PATH_BYTES)
            .ok_or(RegionError::HeapBudgetExceeded)?;
        let path_heap_bytes = path_length
            .checked_add(if path_length == 0 { 0 } else { 24 })
            .and_then(|bytes| u64::try_from(bytes).ok())
            .ok_or(RegionError::HeapBudgetExceeded)?;
        let mut path_reservation =
            MeteredHeapReservation::working(self.heap_meter.clone(), path_heap_bytes)?;
        let parent_device_path = region
            .parent_device_path
            .try_to_string_bounded(MAX_REGION_PATH_BYTES)
            .map_err(|_| RegionError::HeapBudgetExceeded)?
            .ok_or(RegionError::HeapBudgetExceeded)?;
        self.check_deadline()?;
        let resource_generation = match self.backend.resource_generation(
            region,
            &parent_device_path,
            namespace_generation,
            mode,
        ) {
            Ok(generation) => generation,
            Err(error) if is_unknown_region_outcome(error) => {
                state.outcome_unknown = true;
                return Err(RegionError::InvocationOutcomeUnknown);
            }
            Err(error) => return Err(error),
        };
        self.check_deadline()?;
        let key = RegionKey::new(
            region,
            namespace_generation,
            resource_generation,
            mode,
            parent_device_path,
        );
        if let Some(cached) = state.tokens.get(&key) {
            if cached.usable {
                return Ok(Arc::clone(&cached.token));
            }
            let cached = state
                .tokens
                .remove(&key)
                .ok_or(RegionError::NoMatchingLease)?;
            if let Err(error) = self.backend.release(cached.token.lease_id()) {
                if is_unknown_region_outcome(error) {
                    state.outcome_unknown = true;
                }
                state.tokens.insert(key, cached);
                return Err(error);
            }
            self.release_heap(cached.heap_bytes)?;
        }

        if state.tokens.len() >= MAX_CACHED_REGION_TOKENS {
            return Err(RegionError::HeapBudgetExceeded);
        }
        let cache_heap_bytes = region_token_heap_bytes().ok_or(RegionError::HeapBudgetExceeded)?;
        let heap_bytes = path_heap_bytes
            .checked_add(cache_heap_bytes)
            .ok_or(RegionError::HeapBudgetExceeded)?;
        let persistent_reservation =
            MeteredHeapReservation::persistent(self.heap_meter.clone(), cache_heap_bytes)?;
        state
            .tokens
            .try_reserve(1)
            .map_err(|_| RegionError::HeapBudgetExceeded)?;
        self.check_deadline()?;
        path_reservation.commit_working()?;
        let token = match self.backend.authorize(
            region,
            &key.parent_device_path,
            namespace_generation,
            mode,
        ) {
            Ok(token) => Arc::new(token),
            Err(error) if is_unknown_region_outcome(error) => {
                state.outcome_unknown = true;
                return Err(RegionError::InvocationOutcomeUnknown);
            }
            Err(error) => return Err(error),
        };
        self.native_access_authorized.store(true, Ordering::Release);

        if token.generation() != resource_generation {
            if let Err(error) = self.backend.release(token.lease_id()) {
                if is_unknown_region_outcome(error) {
                    state.outcome_unknown = true;
                }
                path_reservation.retain();
                persistent_reservation.retain();
                state.tokens.insert(
                    key,
                    CachedRegionToken {
                        token,
                        heap_bytes,
                        usable: false,
                    },
                );
                return Err(error);
            }
            return Err(RegionError::StaleGeneration);
        }

        persistent_reservation.retain();
        path_reservation.retain();
        state.tokens.insert(
            key,
            CachedRegionToken {
                token: Arc::clone(&token),
                heap_bytes,
                usable: true,
            },
        );
        Ok(token)
    }

    fn invalidate_token(&self, token: &RegionAccessToken) -> Result<(), RegionError> {
        let _invalidation = self.begin_invalidation()?;
        let exists = self
            .state
            .lock()
            .map_err(|_| RegionError::NoMatchingLease)?
            .tokens
            .values()
            .any(|cached| cached.token.as_ref() == token);
        if !exists {
            return Ok(());
        }
        if let Err(error) = self.backend.release(token.lease_id()) {
            if is_unknown_region_outcome(error) {
                self.mark_outcome_unknown();
            }
            return Err(error);
        }
        let removed_heap_bytes = {
            let mut state = self
                .state
                .lock()
                .map_err(|_| RegionError::NoMatchingLease)?;
            let mut removed_heap_bytes = None;
            state.tokens.retain(|_, cached| {
                if cached.token.as_ref() == token {
                    removed_heap_bytes = Some(cached.heap_bytes);
                    false
                } else {
                    true
                }
            });
            removed_heap_bytes
        };
        if let Some(heap_bytes) = removed_heap_bytes {
            self.release_heap(heap_bytes)?;
        }
        Ok(())
    }

    fn width(length: usize) -> Result<u8, AmlError> {
        match length {
            1 | 2 | 4 | 8 => Ok(length as u8),
            _ => Err(AmlError::HostError(format!(
                "NaOS native region width is unsupported: {length}"
            ))),
        }
    }

    fn map_error(error: RegionError) -> AmlError {
        match error {
            error if is_unknown_region_outcome(error) => {
                return AmlError::NativeRegionOutcomeUnknown;
            }
            RegionError::DeadlineExceeded => return AmlError::DeadlineExceeded,
            RegionError::WorkUnitBudgetExceeded | RegionError::HeapBudgetExceeded => {
                return AmlError::BudgetExceeded;
            }
            RegionError::BudgetExceeded => return AmlError::NativeAccessBudgetExceeded,
            RegionError::GlobalLockTimeout => return AmlError::MutexAcquireTimeout,
            RegionError::GlobalLockUnsupported => {
                return AmlError::HostError("NOT_SUPPORTED_GLOBAL_LOCK".to_string());
            }
            RegionError::GlobalLockIoError => return AmlError::GlobalLockOutcomeUnknown,
            _ => {}
        }
        let message = match error {
            other => format!("NaOS native region access rejected: {other:?}"),
        };
        AmlError::HostError(message)
    }
}

impl<B> NativeRegionHandler for NativeRegionAdapter<B>
where
    B: NativeRegionBackend + ?Sized + 'static,
{
    fn acquire_global_lock(&self, timeout_us: u64) -> Result<(), AmlError> {
        self.check_deadline().map_err(Self::map_error)?;
        self.backend
            .acquire_global_lock(timeout_us)
            .map_err(Self::map_error)
    }

    fn release_global_lock(&self) -> Result<(), AmlError> {
        let result = self.backend.release_global_lock().map_err(Self::map_error);
        self.check_deadline().map_err(Self::map_error)?;
        result
    }

    fn execution_thread_id(&self) -> Option<u64> {
        self.backend.execution_thread_id()
    }

    fn invalidate_all(&self) -> Result<(), AmlError> {
        NativeRegionAdapter::invalidate_all(self).map_err(Self::map_error)
    }

    fn authorize(
        &self,
        region: &OpRegion,
        mode: NativeRegionAccessMode,
    ) -> Result<NativeRegionToken, AmlError> {
        self.check_outcome_unknown()?;
        self.native_access_attempted.store(true, Ordering::Release);
        self.charge_native_access().map_err(Self::map_error)?;
        self.check_deadline().map_err(Self::map_error)?;
        let mode = match mode {
            NativeRegionAccessMode::Read => AccessMode::Read,
            NativeRegionAccessMode::ReadWrite => AccessMode::ReadWrite,
        };
        let token = self.token_for(region, mode).map_err(Self::map_error)?;
        self.check_deadline().map_err(Self::map_error)?;
        Ok(NativeRegionToken::from_arc(token))
    }

    fn read(
        &self,
        token: &NativeRegionToken,
        offset: usize,
        length: usize,
    ) -> Result<u64, AmlError> {
        self.check_outcome_unknown()?;
        self.native_access_attempted.store(true, Ordering::Release);
        self.charge_native_access().map_err(Self::map_error)?;
        self.check_deadline().map_err(Self::map_error)?;
        let width = Self::width(length)?;
        let token = token
            .downcast_ref::<RegionAccessToken>()
            .ok_or_else(|| AmlError::HostError("NaOS invalid native region token".to_string()))?;
        let result = token.read(self.backend.as_ref(), offset as u64, width);
        if matches!(result, Err(error) if is_unknown_region_outcome(error)) {
            self.mark_outcome_unknown();
            return Err(AmlError::NativeRegionOutcomeUnknown);
        }
        if matches!(
            result,
            Err(RegionError::StaleGeneration | RegionError::NoMatchingLease)
        ) {
            self.invalidate_token(token).map_err(Self::map_error)?;
        }
        self.check_deadline().map_err(Self::map_error)?;
        result.map_err(Self::map_error)
    }

    fn write(
        &self,
        token: &NativeRegionToken,
        offset: usize,
        length: usize,
        value: u64,
    ) -> Result<(), AmlError> {
        self.check_outcome_unknown()?;
        self.native_access_attempted.store(true, Ordering::Release);
        self.charge_native_access().map_err(Self::map_error)?;
        self.check_deadline().map_err(Self::map_error)?;
        let width = Self::width(length)?;
        let token = token
            .downcast_ref::<RegionAccessToken>()
            .ok_or_else(|| AmlError::HostError("NaOS invalid native region token".to_string()))?;
        let result = token.write(self.backend.as_ref(), offset as u64, width, value);
        if matches!(result, Err(error) if is_unknown_region_outcome(error)) {
            self.mark_outcome_unknown();
            return Err(AmlError::NativeRegionOutcomeUnknown);
        }
        if matches!(
            result,
            Err(RegionError::StaleGeneration | RegionError::NoMatchingLease)
        ) {
            self.invalidate_token(token).map_err(Self::map_error)?;
        }
        self.check_deadline().map_err(Self::map_error)?;
        result.map_err(Self::map_error)
    }
}

impl<B> Drop for NativeRegionAdapter<B>
where
    B: NativeRegionBackend + ?Sized + 'static,
{
    fn drop(&mut self) {
        let tokens = match self.state.get_mut() {
            Ok(state) => core::mem::take(&mut state.tokens),
            Err(_) => return,
        };
        // Protocol teardown is explicit in deactivate_native_regions. A Drop
        // path cannot report failure and must not perform up to 4096 blocking
        // release RPCs; the kernel revokes owner-scoped leases if the service
        // exits after explicit teardown fails.
        let heap_bytes = tokens.into_values().fold(0_u64, |total, cached| {
            total.saturating_add(cached.heap_bytes)
        });
        let _ = self.release_heap(heap_bytes);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::region::{CacheType, LeaseKind, OperationRegion, ResourceLease, authorize_region};
    use acpi::aml::ExecutionMeter;
    use acpi::aml::namespace::AmlName;
    use core::str::FromStr;
    use std::sync::{
        Arc,
        atomic::{AtomicUsize, Ordering},
    };

    fn native_read(
        adapter: &dyn NativeRegionHandler,
        region: &OpRegion,
        offset: usize,
        length: usize,
    ) -> Result<u64, AmlError> {
        let token = adapter.authorize(region, NativeRegionAccessMode::Read)?;
        adapter.read(&token, offset, length)
    }

    fn native_write(
        adapter: &dyn NativeRegionHandler,
        region: &OpRegion,
        offset: usize,
        length: usize,
        value: u64,
    ) -> Result<(), AmlError> {
        let token = adapter.authorize(region, NativeRegionAccessMode::ReadWrite)?;
        adapter.write(&token, offset, length, value)
    }

    struct HeapMeter {
        current_bytes: AtomicU64,
        working_bytes: AtomicU64,
        maximum_bytes: u64,
        loop_iterations: AtomicUsize,
        maximum_loop_iterations: usize,
        reject_deadline: AtomicBool,
    }

    impl HeapMeter {
        fn reserve(&self, bytes: u64) -> Result<(), AmlError> {
            let mut observed = self.current_bytes.load(Ordering::Acquire);
            loop {
                let Some(next) = observed.checked_add(bytes) else {
                    return Err(AmlError::HostError(
                        "AML_BUDGET_EXHAUSTED: test heap overflow".into(),
                    ));
                };
                if next > self.maximum_bytes {
                    return Err(AmlError::HostError(
                        "AML_BUDGET_EXHAUSTED: test heap limit".into(),
                    ));
                }
                match self.current_bytes.compare_exchange_weak(
                    observed,
                    next,
                    Ordering::AcqRel,
                    Ordering::Acquire,
                ) {
                    Ok(_) => return Ok(()),
                    Err(next_observed) => observed = next_observed,
                }
            }
        }
    }

    impl ExecutionMeter for HeapMeter {
        fn charge_opcode(&self) -> Result<(), AmlError> {
            Ok(())
        }

        fn enter_method(&self) -> Result<(), AmlError> {
            Ok(())
        }

        fn leave_method(&self) {}

        fn loop_iteration(&self) -> Result<(), AmlError> {
            let iteration = self.loop_iterations.fetch_add(1, Ordering::AcqRel);
            if iteration >= self.maximum_loop_iterations {
                return Err(AmlError::BudgetExceeded);
            }
            Ok(())
        }

        fn check_deadline(&self) -> Result<(), AmlError> {
            if self.reject_deadline.load(Ordering::Acquire) {
                Err(AmlError::DeadlineExceeded)
            } else {
                Ok(())
            }
        }

        fn reserve_heap(&self, bytes: u64) -> Result<(), AmlError> {
            self.reserve(bytes)
        }

        fn release_heap(&self, bytes: u64) -> Result<(), AmlError> {
            self.current_bytes.fetch_sub(bytes, Ordering::AcqRel);
            Ok(())
        }

        fn reserve_working_heap(&self, bytes: u64) -> Result<(), AmlError> {
            self.reserve(bytes)?;
            self.working_bytes.fetch_add(bytes, Ordering::AcqRel);
            Ok(())
        }

        fn commit_working_heap(&self, bytes: u64) -> Result<(), AmlError> {
            self.working_bytes.fetch_sub(bytes, Ordering::AcqRel);
            Ok(())
        }

        fn release_working_heap(&self, bytes: u64) {
            self.working_bytes.fetch_sub(bytes, Ordering::AcqRel);
            self.current_bytes.fetch_sub(bytes, Ordering::AcqRel);
        }
    }

    fn heap_meter(maximum_bytes: u64) -> AmlArc<HeapMeter> {
        aml_allocator::arc_new(HeapMeter {
            current_bytes: AtomicU64::new(0),
            working_bytes: AtomicU64::new(0),
            maximum_bytes,
            loop_iterations: AtomicUsize::new(0),
            maximum_loop_iterations: usize::MAX,
            reject_deadline: AtomicBool::new(false),
        })
    }

    fn heap_meter_with_loop_limit(
        maximum_bytes: u64,
        maximum_loop_iterations: usize,
    ) -> AmlArc<HeapMeter> {
        aml_allocator::arc_new(HeapMeter {
            current_bytes: AtomicU64::new(0),
            working_bytes: AtomicU64::new(0),
            maximum_bytes,
            loop_iterations: AtomicUsize::new(0),
            maximum_loop_iterations,
            reject_deadline: AtomicBool::new(false),
        })
    }

    struct ReleaseBackend {
        fail_release: AtomicBool,
        change_generation_on_authorize: AtomicBool,
        release_calls: AtomicUsize,
        authorization_calls: AtomicUsize,
        current_generation: AtomicU64,
    }

    impl ReleaseBackend {
        fn new(fail_release: bool) -> Self {
            Self {
                fail_release: AtomicBool::new(fail_release),
                change_generation_on_authorize: AtomicBool::new(false),
                release_calls: AtomicUsize::new(0),
                authorization_calls: AtomicUsize::new(0),
                current_generation: AtomicU64::new(5),
            }
        }
    }

    impl RegionAccessor for ReleaseBackend {
        fn lease_generation(&self, _lease_id: u64) -> Result<u64, RegionError> {
            Ok(self.current_generation.load(Ordering::Acquire))
        }

        fn read(&self, _lease_id: u64, _address: u64, _width: u8) -> Result<u64, RegionError> {
            Ok(0)
        }

        fn write(
            &self,
            _lease_id: u64,
            _address: u64,
            _width: u8,
            _value: u64,
        ) -> Result<(), RegionError> {
            Ok(())
        }
    }

    impl NativeRegionBackend for ReleaseBackend {
        fn resource_generation(
            &self,
            _region: &OpRegion,
            _parent_device_path: &str,
            _namespace_generation: u64,
            _mode: AccessMode,
        ) -> Result<u64, RegionError> {
            Ok(self.current_generation.load(Ordering::Acquire))
        }

        fn authorize(
            &self,
            _region: &OpRegion,
            _parent_device_path: &str,
            _namespace_generation: u64,
            mode: AccessMode,
        ) -> Result<RegionAccessToken, RegionError> {
            self.authorization_calls.fetch_add(1, Ordering::AcqRel);
            if self
                .change_generation_on_authorize
                .swap(false, Ordering::AcqRel)
            {
                self.current_generation.fetch_add(1, Ordering::AcqRel);
            }
            let generation = self.current_generation.load(Ordering::Acquire);
            let region = OperationRegion {
                space: crate::region::RegionSpace::SystemMemory,
                base: 0x1000,
                length: 4,
                parent_device: 3,
                generation,
            };
            let lease = ResourceLease::new(
                7,
                3,
                0x1000,
                0x1000,
                0x1000,
                0x2000,
                LeaseKind::DeviceMmio,
                CacheType::Uncacheable,
                generation,
                mode,
            )?;
            authorize_region(region, &[lease], mode)
        }

        fn release(&self, _lease_id: u64) -> Result<(), RegionError> {
            self.release_calls.fetch_add(1, Ordering::AcqRel);
            if self.fail_release.load(Ordering::Acquire) {
                Err(RegionError::NoMatchingLease)
            } else {
                self.current_generation.fetch_add(1, Ordering::AcqRel);
                Ok(())
            }
        }
    }

    #[test]
    fn generation_race_releases_authorized_lease_and_rolls_back_cache_budget() {
        let backend = Arc::new(ReleaseBackend::new(false));
        backend
            .change_generation_on_authorize
            .store(true, Ordering::Release);
        let meter = heap_meter(u64::MAX);
        let adapter = NativeRegionAdapter::new_with_profile_and_meter(
            backend.clone(),
            5,
            Arc::new(NativeAccessBudget::new(8, 32)),
            Arc::new(AtomicBool::new(false)),
            Arc::new(AtomicBool::new(false)),
            HardwareSideEffectProfile::controlled_read_only(),
            meter.clone(),
        );
        let region = OpRegion {
            space: acpi::aml::op_region::RegionSpace::SystemMemory,
            base: 0x1000,
            length: 4,
            parent_device_path: AmlName::from_str("\\_SB.DEV0").unwrap(),
        };

        let error = native_read(&adapter, &region, 0, 1).unwrap_err();
        assert!(format!("{error:?}").contains("StaleGeneration"));
        assert_eq!(backend.release_calls.load(Ordering::Acquire), 1);
        assert_eq!(meter.current_bytes.load(Ordering::Acquire), 0);
        assert_eq!(meter.working_bytes.load(Ordering::Acquire), 0);

        assert_eq!(native_read(&adapter, &region, 0, 1), Ok(0));
        assert_eq!(backend.authorization_calls.load(Ordering::Acquire), 2);
        assert_eq!(adapter.invalidate_all(), Ok(()));
        assert_eq!(meter.current_bytes.load(Ordering::Acquire), 0);
    }

    #[test]
    fn failed_generation_race_release_keeps_lease_tracked_for_teardown_retry() {
        let backend = Arc::new(ReleaseBackend::new(true));
        backend
            .change_generation_on_authorize
            .store(true, Ordering::Release);
        let meter = heap_meter(u64::MAX);
        let adapter = NativeRegionAdapter::new_with_profile_and_meter(
            backend.clone(),
            5,
            Arc::new(NativeAccessBudget::new(8, 32)),
            Arc::new(AtomicBool::new(false)),
            Arc::new(AtomicBool::new(false)),
            HardwareSideEffectProfile::controlled_read_only(),
            meter.clone(),
        );
        let region = OpRegion {
            space: acpi::aml::op_region::RegionSpace::SystemMemory,
            base: 0x1000,
            length: 4,
            parent_device_path: AmlName::from_str("\\_SB.DEV0").unwrap(),
        };

        assert!(native_read(&adapter, &region, 0, 1).is_err());
        assert_eq!(backend.release_calls.load(Ordering::Acquire), 1);
        assert!(meter.current_bytes.load(Ordering::Acquire) > 0);

        backend.fail_release.store(false, Ordering::Release);
        assert_eq!(adapter.invalidate_all(), Ok(()));
        assert_eq!(backend.release_calls.load(Ordering::Acquire), 2);
        assert_eq!(meter.current_bytes.load(Ordering::Acquire), 0);
    }

    #[test]
    fn failed_release_remains_visible_and_can_be_retried_during_teardown() {
        let backend = Arc::new(ReleaseBackend::new(true));
        let meter = heap_meter(u64::MAX);
        let adapter = NativeRegionAdapter::new_with_profile_and_meter(
            backend.clone(),
            5,
            Arc::new(NativeAccessBudget::new(8, 32)),
            Arc::new(AtomicBool::new(false)),
            Arc::new(AtomicBool::new(false)),
            HardwareSideEffectProfile::controlled_read_only(),
            meter.clone(),
        );
        let region = OpRegion {
            space: acpi::aml::op_region::RegionSpace::SystemMemory,
            base: 0x1000,
            length: 4,
            parent_device_path: AmlName::from_str("\\_SB.DEV0").unwrap(),
        };

        assert_eq!(native_read(&adapter, &region, 0, 1), Ok(0));
        let cached_heap_bytes = meter.current_bytes.load(Ordering::Acquire);
        assert!(cached_heap_bytes > 0);
        assert_eq!(adapter.invalidate_all(), Err(RegionError::NoMatchingLease));
        assert_eq!(backend.release_calls.load(Ordering::Acquire), 1);
        assert_eq!(
            meter.current_bytes.load(Ordering::Acquire),
            cached_heap_bytes
        );

        backend.fail_release.store(false, Ordering::Release);
        assert_eq!(adapter.invalidate_all(), Ok(()));
        assert_eq!(backend.release_calls.load(Ordering::Acquire), 2);
        assert_eq!(meter.current_bytes.load(Ordering::Acquire), 0);
    }

    #[test]
    fn revoked_opaque_region_token_cannot_be_used_after_invalidation() {
        let backend = Arc::new(ReleaseBackend::new(false));
        let adapter = NativeRegionAdapter::new_with_profile(
            backend.clone(),
            5,
            Arc::new(NativeAccessBudget::new(8, 32)),
            Arc::new(AtomicBool::new(false)),
            Arc::new(AtomicBool::new(false)),
            HardwareSideEffectProfile::controlled_read_only(),
        );
        let region = OpRegion {
            space: RegionSpace::SystemMemory,
            base: 0x1000,
            length: 4,
            parent_device_path: AmlName::from_str("\\_SB.DEV0").unwrap(),
        };
        let token = adapter
            .authorize(&region, NativeRegionAccessMode::Read)
            .expect("region authorization");

        adapter.invalidate_all().expect("token invalidation");

        let error = adapter
            .read(&token, 0, 1)
            .expect_err("a retained token must fail after lease revoke");
        assert!(format!("{error:?}").contains("StaleGeneration"));
        assert_eq!(backend.release_calls.load(Ordering::Acquire), 1);
    }

    #[test]
    fn invalidation_charges_each_token_release_to_the_work_unit_loop_budget() {
        let backend = Arc::new(ReleaseBackend::new(false));
        let meter = heap_meter_with_loop_limit(u64::MAX, 0);
        let adapter = NativeRegionAdapter::new_with_profile_and_meter(
            backend.clone(),
            5,
            Arc::new(NativeAccessBudget::new(8, 32)),
            Arc::new(AtomicBool::new(false)),
            Arc::new(AtomicBool::new(false)),
            HardwareSideEffectProfile::controlled_read_only(),
            meter.clone(),
        );
        let region = OpRegion {
            space: acpi::aml::op_region::RegionSpace::SystemMemory,
            base: 0x1000,
            length: 4,
            parent_device_path: AmlName::from_str("\\_SB.DEV0").unwrap(),
        };
        native_read(&adapter, &region, 0, 1).unwrap();

        assert_eq!(
            adapter.invalidate_all(),
            Err(RegionError::WorkUnitBudgetExceeded)
        );
        assert_eq!(backend.release_calls.load(Ordering::Acquire), 0);
        assert!(meter.current_bytes.load(Ordering::Acquire) > 0);
    }

    #[test]
    fn invalidation_checks_deadline_before_backend_release() {
        let backend = Arc::new(ReleaseBackend::new(false));
        let meter = heap_meter(u64::MAX);
        let adapter = NativeRegionAdapter::new_with_profile_and_meter(
            backend.clone(),
            5,
            Arc::new(NativeAccessBudget::new(8, 32)),
            Arc::new(AtomicBool::new(false)),
            Arc::new(AtomicBool::new(false)),
            HardwareSideEffectProfile::controlled_read_only(),
            meter.clone(),
        );
        let region = OpRegion {
            space: acpi::aml::op_region::RegionSpace::SystemMemory,
            base: 0x1000,
            length: 4,
            parent_device_path: AmlName::from_str("\\_SB.DEV0").unwrap(),
        };
        native_read(&adapter, &region, 0, 1).unwrap();
        meter.reject_deadline.store(true, Ordering::Release);

        assert_eq!(adapter.invalidate_all(), Err(RegionError::DeadlineExceeded));
        assert_eq!(
            NativeRegionAdapter::<ReleaseBackend>::map_error(RegionError::DeadlineExceeded),
            AmlError::DeadlineExceeded
        );
        assert_eq!(backend.release_calls.load(Ordering::Acquire), 0);
        assert!(meter.current_bytes.load(Ordering::Acquire) > 0);
    }

    #[test]
    fn execution_meter_deadline_stays_typed_through_region_dispatch() {
        assert_eq!(
            map_execution_meter_error(AmlError::DeadlineExceeded),
            RegionError::DeadlineExceeded
        );
        assert_eq!(
            NativeRegionAdapter::<ReleaseBackend>::map_error(RegionError::DeadlineExceeded),
            AmlError::DeadlineExceeded
        );
    }

    #[test]
    fn token_cache_heap_is_reserved_before_authorization_and_released_on_invalidation() {
        let denied_backend = Arc::new(ReleaseBackend::new(false));
        let parent_device_path = AmlName::from_str("\\_SB.DEV0").unwrap();
        let path_bytes = parent_device_path.encoded_len().unwrap() + 24;
        let denied_meter = heap_meter(path_bytes as u64);
        let denied_adapter = NativeRegionAdapter::new_with_profile_and_meter(
            denied_backend.clone(),
            5,
            Arc::new(NativeAccessBudget::new(8, 32)),
            Arc::new(AtomicBool::new(false)),
            Arc::new(AtomicBool::new(false)),
            HardwareSideEffectProfile::controlled_read_only(),
            denied_meter.clone(),
        );
        let region = OpRegion {
            space: acpi::aml::op_region::RegionSpace::SystemMemory,
            base: 0x1000,
            length: 4,
            parent_device_path: parent_device_path.clone(),
        };

        let denied = native_read(&denied_adapter, &region, 0, 1).unwrap_err();
        assert_eq!(denied, AmlError::BudgetExceeded);
        assert_eq!(
            denied_backend.authorization_calls.load(Ordering::Acquire),
            0
        );
        assert_eq!(denied_meter.current_bytes.load(Ordering::Acquire), 0);

        let backend = Arc::new(ReleaseBackend::new(false));
        let meter = heap_meter(u64::MAX);
        let adapter = NativeRegionAdapter::new_with_profile_and_meter(
            backend.clone(),
            5,
            Arc::new(NativeAccessBudget::new(8, 32)),
            Arc::new(AtomicBool::new(false)),
            Arc::new(AtomicBool::new(false)),
            HardwareSideEffectProfile::controlled_read_only(),
            meter.clone(),
        );

        assert_eq!(native_read(&adapter, &region, 0, 1), Ok(0));
        assert!(meter.current_bytes.load(Ordering::Acquire) > 0);
        assert_eq!(adapter.invalidate_all(), Ok(()));
        assert_eq!(meter.current_bytes.load(Ordering::Acquire), 0);
        assert_eq!(meter.working_bytes.load(Ordering::Acquire), 0);
        assert_eq!(backend.authorization_calls.load(Ordering::Acquire), 1);
    }

    #[test]
    fn deny_all_hardware_profile_rejects_before_kernel_region_authorization() {
        let backend = Arc::new(ReleaseBackend::new(false));
        let adapter = NativeRegionAdapter::new_with_profile(
            backend.clone(),
            5,
            Arc::new(NativeAccessBudget::new(8, 32)),
            Arc::new(AtomicBool::new(false)),
            Arc::new(AtomicBool::new(false)),
            crate::HardwareSideEffectProfile::deny_all(),
        );
        let region = OpRegion {
            space: acpi::aml::op_region::RegionSpace::SystemMemory,
            base: 0x1000,
            length: 4,
            parent_device_path: AmlName::from_str("\\_SB.DEV0").unwrap(),
        };

        assert!(native_read(&adapter, &region, 0, 1).is_err());
        assert_eq!(backend.authorization_calls.load(Ordering::Acquire), 0);
    }

    #[test]
    fn controlled_profile_rejects_pci_config_and_writes_before_kernel_region_authorization() {
        let backend = Arc::new(ReleaseBackend::new(false));
        let adapter = NativeRegionAdapter::new_with_profile(
            backend.clone(),
            5,
            Arc::new(NativeAccessBudget::new(8, 32)),
            Arc::new(AtomicBool::new(false)),
            Arc::new(AtomicBool::new(false)),
            HardwareSideEffectProfile::controlled_read_only(),
        );
        let memory_region = OpRegion {
            space: RegionSpace::SystemMemory,
            base: 0x1000,
            length: 4,
            parent_device_path: AmlName::from_str("\\_SB.DEV0").unwrap(),
        };
        let pci_config_region = OpRegion {
            space: RegionSpace::PciConfig,
            base: 0x1000,
            length: 4,
            parent_device_path: AmlName::from_str("\\_SB.DEV0").unwrap(),
        };

        assert_eq!(native_read(&adapter, &memory_region, 0, 1), Ok(0));
        assert!(native_write(&adapter, &memory_region, 0, 1, 1).is_err());
        assert!(native_read(&adapter, &pci_config_region, 0, 1).is_err());
        assert_eq!(backend.authorization_calls.load(Ordering::Acquire), 1);
    }

    #[test]
    fn native_access_fuel_resets_only_when_the_next_top_level_operation_starts() {
        let budget = NativeAccessBudget::new(2, 3);
        budget.charge().unwrap();
        budget.charge().unwrap();
        assert_eq!(budget.charge(), Err(RegionError::BudgetExceeded));

        budget.reset();
        budget.charge().unwrap();
        assert_eq!(budget.charge(), Ok(()));
        assert_eq!(budget.charge(), Err(RegionError::BudgetExceeded));
    }

    #[test]
    fn work_unit_native_access_fuel_accumulates_across_method_resets() {
        let budget = NativeAccessBudget::new(2, 3);
        budget.begin_work_unit().unwrap();

        budget.charge().unwrap();
        budget.charge().unwrap();
        budget.reset();
        budget.charge().unwrap();
        budget.reset();
        assert_eq!(budget.charge(), Err(RegionError::WorkUnitBudgetExceeded));

        budget.end_work_unit();
        budget.begin_work_unit().unwrap();
        budget.reset();
        budget.charge().unwrap();

        assert_eq!(
            NativeRegionAdapter::<ReleaseBackend>::map_error(RegionError::WorkUnitBudgetExceeded),
            AmlError::BudgetExceeded
        );
        assert_eq!(
            NativeRegionAdapter::<ReleaseBackend>::map_error(RegionError::HeapBudgetExceeded),
            AmlError::BudgetExceeded
        );
        assert_eq!(
            NativeRegionAdapter::<ReleaseBackend>::map_error(RegionError::BudgetExceeded),
            AmlError::NativeAccessBudgetExceeded
        );
    }
}
