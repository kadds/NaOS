//! The private NaOS boundary around the pinned ACPI/AML implementation.
//!
//! This crate intentionally exposes no upstream `Handler`, physical address,
//! AML path, or capability type.  The first stage only loads immutable table
//! bytes into an offline namespace.  The native dispatch hook is installed
//! with a deny-all backend in this mode, so no hardware region is admitted;
//! any attempted access is recorded as a fail-closed boundary violation.

use std::{
    string::String,
    sync::atomic::{AtomicU64, Ordering},
    vec::Vec,
};

use acpi::aml::allocator::{
    self as aml_allocator, Arc as AmlArc, String as AmlString, Vec as AmlVec,
};

pub mod region;
pub mod sci;

#[cfg(any(target_os = "naos", test))]
pub mod native;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct CompatibilityProfile {
    /// The immutable value exposed to AML `_OS`.
    pub os_name: &'static str,
    pub osi_windows: bool,
    pub osi_linux: bool,
    pub os_revision: u64,
    pub pic_mode: u8,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct FirmwareIdentity {
    pub firmware_version: String,
    /// Monotonic kernel process ID. It scopes every executor incarnation so
    /// a restarted platformd cannot recreate an old namespace generation.
    pub process_id: u64,
}

impl FirmwareIdentity {
    pub fn new(firmware_version: impl Into<String>, process_id: u64) -> Self {
        Self {
            firmware_version: firmware_version.into(),
            process_id,
        }
    }

    fn valid(&self) -> bool {
        !self.firmware_version.is_empty()
            && self.process_id != 0
            && self.process_id <= u64::from(u32::MAX)
    }
}

static NEXT_SERVICE_GENERATION: AtomicU64 = AtomicU64::new(1);

/// Mint a monotonic generation scoped by a kernel-assigned process ID.
/// Process IDs are never reused within one boot; pairing them with a checked
/// per-process sequence keeps the value ordered across service restarts and
/// repeated namespace/topology initialization in one process.
pub fn next_instance_generation(process_id: u64) -> Result<u64, LoadError> {
    if process_id == 0 || process_id > u64::from(u32::MAX) {
        return Err(LoadError::InvalidIdentity);
    }
    let mut sequence = NEXT_SERVICE_GENERATION.load(Ordering::Acquire);
    loop {
        if sequence == 0 || sequence > u64::from(u32::MAX) {
            return Err(LoadError::GenerationExhausted);
        }
        match NEXT_SERVICE_GENERATION.compare_exchange_weak(
            sequence,
            sequence + 1,
            Ordering::AcqRel,
            Ordering::Acquire,
        ) {
            Ok(_) => break,
            Err(observed) => sequence = observed,
        }
    }
    Ok((process_id << 32) | sequence)
}

impl Default for CompatibilityProfile {
    fn default() -> Self {
        Self {
            os_name: "Microsoft Windows NT",
            osi_windows: true,
            osi_linux: false,
            os_revision: 2,
            pic_mode: 1,
        }
    }
}

impl CompatibilityProfile {
    /// Stable, process-independent profile identity for namespace generation.
    pub fn stable_hash(self) -> u64 {
        let mut hash = 0xcbf29ce484222325_u64;
        for byte in self.os_name.as_bytes() {
            hash ^= u64::from(*byte);
            hash = hash.wrapping_mul(0x100000001b3);
        }
        hash ^= 0xff;
        hash = hash.wrapping_mul(0x100000001b3);
        for byte in [self.osi_windows as u8, self.osi_linux as u8, self.pic_mode]
            .into_iter()
            .chain(self.os_revision.to_le_bytes())
        {
            hash ^= u64::from(byte);
            hash = hash.wrapping_mul(0x100000001b3);
        }
        hash
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ExecutionBudget {
    pub opcode_fuel: u64,
    pub native_access_fuel: u64,
    /// Aggregate limits for one bounded high-level AML operation.
    pub max_operation_opcode_fuel: u64,
    pub max_operation_native_access_fuel: u64,
    pub max_call_depth: u32,
    pub max_loop_iterations: u64,
    pub max_operation_loop_iterations: u64,
    pub max_namespace_objects: u32,
    pub max_package_bytes: u64,
    pub max_buffer_bytes: u64,
    pub max_heap_bytes: u64,
    /// Wall-clock limit for one top-level AML method and its nested calls.
    /// Zero disables only this last-resort watchdog; opcode/resource fuel
    /// remains enforced independently.
    pub max_method_duration_ns: u64,
    /// Zero disables only the operation deadline, not its fuel limits.
    pub max_operation_duration_ns: u64,
}

/// Hard firmware input limits shared with platformd's FirmwareRoot catalog
/// validation and enforced by the executor even when called directly.
pub const MAX_AML_TABLE_COUNT: usize = 1024;
pub const MAX_AML_TABLE_BYTES: u64 = 1 << 20;
pub const MAX_AML_CATALOG_BYTES: u64 = 16 << 20;

const DEFAULT_METHOD_DEADLINE_NS: u64 = 10_000_000_000;
const DEADLINE_OPCODE_SAMPLE_INTERVAL: u16 = 1024;

impl ExecutionBudget {
    pub const fn offline_default() -> Self {
        Self {
            opcode_fuel: 8_000_000,
            native_access_fuel: 0,
            max_operation_opcode_fuel: 64_000_000,
            max_operation_native_access_fuel: 65_536,
            max_call_depth: 64,
            max_loop_iterations: 1_000_000,
            max_operation_loop_iterations: 8_000_000,
            max_namespace_objects: 16_384,
            // FirmwareRoot admits up to a 16 MiB immutable table catalog.
            // Keep package/table input bounded at that same aggregate limit;
            // AML Buffer allocations have their own, smaller budget below.
            max_package_bytes: 16 << 20,
            max_buffer_bytes: 1 << 20,
            max_heap_bytes: 32 << 20,
            max_method_duration_ns: DEFAULT_METHOD_DEADLINE_NS,
            max_operation_duration_ns: 30_000_000_000,
        }
    }

    /// Bounded profile for the platformd-only native-region phase. The
    /// backend still decides which address spaces and write modes are
    /// admitted; this value only supplies a finite native-access budget.
    pub const fn controlled_read_only() -> Self {
        Self {
            native_access_fuel: 4096,
            ..Self::offline_default()
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct TableDescriptor {
    pub signature: [u8; 4],
    pub revision: u8,
    pub length: u32,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct NamespaceSummary {
    pub generation: u64,
    pub tables_loaded: u32,
    pub opcode_fuel_remaining: u64,
    pub global_lock_dependency: bool,
    pub native_access_attempted: bool,
    pub native_access_authorized: bool,
    pub pic_evaluated: bool,
    pub pic_degraded: bool,
    pub region_registration_degraded: bool,
    pub deadline_exceeded: bool,
}

/// A policy-filtered device result produced by the private ACPI executor.
/// This intentionally contains no AML object, reference, region token, or
/// physical address; platformd may turn it into its own bounded snapshot.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct DeviceSnapshot {
    /// Executor-issued identity; callers cannot use it to evaluate arbitrary
    /// namespace paths because this crate exposes no lookup operation.
    pub device_id: u64,
    pub namespace_generation: u64,
    pub path: String,
    pub identifiers: Vec<DeviceIdentifier>,
    pub identifiers_evaluated: bool,
    pub status: u64,
    pub status_evaluated: bool,
    pub initialized: bool,
    pub initialization_evaluated: bool,
    pub degraded: bool,
    pub resources: Vec<ResourceSnapshot>,
    /// Full normalized ACPI address-space descriptors, retained separately
    /// from the compact generic resource summary for later kernel-attested
    /// PCI function resource grants.
    pub address_spaces: Vec<PciRootResourceWindow>,
    pub resources_evaluated: bool,
    pub routing_entries: u32,
    pub routing: Vec<PciRoutingEntry>,
    pub routing_evaluated: bool,
    pub routing_complete: bool,
}

pub const MAX_DEVICE_CIDS: usize = 16;
pub const MAX_DEVICE_IDENTIFIERS: usize = MAX_DEVICE_CIDS + 2;
const MAX_IDENTIFIER_REFERENCE_DEPTH: u8 = 16;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(u32)]
pub enum DeviceIdentifierKind {
    Hid = 1,
    Cid = 2,
    Uid = 3,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum DeviceIdentifierValue {
    String(String),
    Integer(u64),
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct DeviceIdentifier {
    pub kind: DeviceIdentifierKind,
    pub value: DeviceIdentifierValue,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum DeviceIdentifierError {
    AllocationFailed,
    InvalidValue,
    StringTooLong,
    TooManyCids,
}

#[cfg(any(target_os = "naos", test))]
fn try_reserve_device_identifier_output<T>(
    output: &mut Vec<T>,
    additional: usize,
) -> Result<(), DeviceIdentifierError> {
    output
        .try_reserve_exact(additional)
        .map_err(|_| DeviceIdentifierError::AllocationFailed)
}

#[cfg(any(target_os = "naos", test))]
fn try_clone_device_identifier_string(value: &str) -> Result<String, DeviceIdentifierError> {
    let mut copy = String::new();
    copy.try_reserve_exact(value.len())
        .map_err(|_| DeviceIdentifierError::AllocationFailed)?;
    copy.push_str(value);
    Ok(copy)
}

#[cfg(any(target_os = "naos", test))]
pub fn decode_device_identifiers(
    hid: Option<&acpi::aml::object::Object>,
    cid: Option<&acpi::aml::object::Object>,
    uid: Option<&acpi::aml::object::Object>,
) -> Result<Vec<DeviceIdentifier>, DeviceIdentifierError> {
    fn value_at_depth(
        object: &acpi::aml::object::Object,
        depth: u8,
    ) -> Result<DeviceIdentifierValue, DeviceIdentifierError> {
        use acpi::aml::object::Object;
        match object {
            Object::String(text) if text.as_bytes().len() <= 255 => Ok(
                DeviceIdentifierValue::String(try_clone_device_identifier_string(text)?),
            ),
            Object::String(_) => Err(DeviceIdentifierError::StringTooLong),
            Object::Integer(integer) => Ok(DeviceIdentifierValue::Integer(*integer)),
            Object::Reference { inner, .. } if depth < MAX_IDENTIFIER_REFERENCE_DEPTH => {
                value_at_depth(&**inner, depth + 1)
            }
            Object::Reference { .. } => Err(DeviceIdentifierError::InvalidValue),
            _ => Err(DeviceIdentifierError::InvalidValue),
        }
    }

    fn append_one(
        output: &mut Vec<DeviceIdentifier>,
        kind: DeviceIdentifierKind,
        object: &acpi::aml::object::Object,
    ) -> Result<(), DeviceIdentifierError> {
        output.push(DeviceIdentifier {
            kind,
            value: value_at_depth(object, 0)?,
        });
        Ok(())
    }

    let mut identifiers = Vec::new();
    try_reserve_device_identifier_output(&mut identifiers, MAX_DEVICE_IDENTIFIERS)?;
    if let Some(hid) = hid {
        append_one(&mut identifiers, DeviceIdentifierKind::Hid, hid)?;
    }
    if let Some(cid) = cid {
        match cid {
            acpi::aml::object::Object::Package(values) => {
                if values.len() > MAX_DEVICE_CIDS {
                    return Err(DeviceIdentifierError::TooManyCids);
                }
                for value in values {
                    append_one(&mut identifiers, DeviceIdentifierKind::Cid, &**value)?;
                }
            }
            _ => append_one(&mut identifiers, DeviceIdentifierKind::Cid, cid)?,
        }
    }
    if let Some(uid) = uid {
        append_one(&mut identifiers, DeviceIdentifierKind::Uid, uid)?;
    }
    if identifiers.len() > MAX_DEVICE_IDENTIFIERS {
        return Err(DeviceIdentifierError::TooManyCids);
    }
    Ok(identifiers)
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ResourceSnapshot {
    pub kind: u8,
    pub base: u64,
    pub length: u64,
    pub flags: u64,
}

fn snapshot_entry_size_bytes(
    path: &str,
    identifiers: &[DeviceIdentifier],
    resources: &[ResourceSnapshot],
    address_spaces: &[PciRootResourceWindow],
    routing: &[PciRoutingEntry],
) -> Option<usize> {
    let identifier_bytes = identifiers.iter().try_fold(0_usize, |total, identifier| {
        let value_bytes = match &identifier.value {
            DeviceIdentifierValue::String(text) => text.len(),
            DeviceIdentifierValue::Integer(_) => 8,
        };
        total.checked_add(16)?.checked_add(value_bytes)
    })?;

    path.len()
        .checked_add(32)?
        .checked_add(identifier_bytes)?
        .checked_add(resources.len().checked_mul(32)?)?
        .checked_add(address_spaces.len().checked_mul(64)?)?
        .checked_add(routing.len().checked_mul(32)?)
}

fn snapshot_working_heap_bytes(device_limit: usize, snapshot_byte_limit: usize) -> Option<u64> {
    let identifier_overhead = MAX_DEVICE_IDENTIFIERS
        .checked_mul(core::mem::size_of::<DeviceIdentifier>().checked_add(24)?)?;
    let per_device_overhead = core::mem::size_of::<DeviceSnapshot>()
        .checked_add(identifier_overhead)?
        .checked_add(5_usize.checked_mul(24)?)?
        .checked_add(128)?;
    let bytes = snapshot_byte_limit
        .checked_mul(2)?
        .checked_add(device_limit.checked_mul(per_device_overhead)?)?
        .checked_add(4096)?;
    u64::try_from(bytes).ok()
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum PciRoutingSource {
    DirectGsi(u32),
    LinkDevice { device_id: u64, source_index: u32 },
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct PciRoutingEntry {
    /// `_PRT` Address in the ACPI `_ADR` DWORD encoding. The function field
    /// is `0xffff`, as required for PCI interrupt routing entries.
    pub address: u32,
    pub pin: u8,
    pub source: PciRoutingSource,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum PciRoutingError {
    AllocationFailed,
    BudgetExceeded,
    DeadlineExceeded,
    MeterFailure,
    InvalidTable,
    InvalidEntry,
    DuplicatePin,
    TooManyEntries,
}

pub const MAX_PCI_ROUTING_ENTRIES: usize = 256;

/// Address-space side effects that an ACPI snapshot operation is allowed to
/// request.  The first-stage backend deliberately supports only the
/// fail-closed profile; keeping the dimensions explicit prevents a future
/// caller from turning on native execution by adding a single boolean.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct HardwareSideEffectProfile {
    pub allow_system_memory: bool,
    pub allow_system_io: bool,
    pub allow_pci_config: bool,
    pub allow_writes: bool,
}

impl HardwareSideEffectProfile {
    pub const fn deny_all() -> Self {
        Self {
            allow_system_memory: false,
            allow_system_io: false,
            allow_pci_config: false,
            allow_writes: false,
        }
    }

    pub const fn controlled_read_only() -> Self {
        Self {
            allow_system_memory: true,
            allow_system_io: true,
            allow_pci_config: false,
            allow_writes: false,
        }
    }

    pub(crate) const fn to_bits(self) -> u8 {
        (self.allow_system_memory as u8)
            | ((self.allow_system_io as u8) << 1)
            | ((self.allow_pci_config as u8) << 2)
            | ((self.allow_writes as u8) << 3)
    }

    pub(crate) const fn from_bits(bits: u8) -> Self {
        Self {
            allow_system_memory: bits & 1 != 0,
            allow_system_io: bits & 2 != 0,
            allow_pci_config: bits & 4 != 0,
            allow_writes: bits & 8 != 0,
        }
    }

    pub(crate) const fn is_subset_of(self, maximum: Self) -> bool {
        (!self.allow_system_memory || maximum.allow_system_memory)
            && (!self.allow_system_io || maximum.allow_system_io)
            && (!self.allow_pci_config || maximum.allow_pci_config)
            && (!self.allow_writes || maximum.allow_writes)
    }
}

/// Explicit entry allowlist for the bounded ACPI device snapshot operation.
/// These methods remain AML control methods: the allowlist does not make them
/// side-effect free.  Their native access is governed independently by
/// `hardware`, which is deny-all for the current offline namespace.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct DeviceSnapshotPolicy {
    pub evaluate_identity: bool,
    pub evaluate_sta: bool,
    pub evaluate_ini: bool,
    pub evaluate_crs: bool,
    pub evaluate_prt: bool,
    pub hardware: HardwareSideEffectProfile,
    pub max_resources_per_device: usize,
    pub max_snapshot_bytes: usize,
}

impl DeviceSnapshotPolicy {
    pub const fn offline_default() -> Self {
        Self {
            evaluate_identity: true,
            evaluate_sta: true,
            evaluate_ini: true,
            evaluate_crs: true,
            evaluate_prt: true,
            hardware: HardwareSideEffectProfile::deny_all(),
            max_resources_per_device: 256,
            max_snapshot_bytes: 64 * 1024,
        }
    }

    fn validate(self) -> Result<(), LoadError> {
        if !self.evaluate_sta && (self.evaluate_ini || self.evaluate_crs || self.evaluate_prt) {
            return Err(LoadError::InvalidProfile);
        }
        if self.max_resources_per_device > 256 || self.max_snapshot_bytes > 64 * 1024 {
            return Err(LoadError::BudgetExceeded);
        }
        Ok(())
    }
}

/// A bounded execution accounting object owned by one AML ordering domain.
///
/// The fork's interpreter invokes this state through opcode, call, loop, and
/// namespace insertion hooks. Allocation dimensions are charged before their
/// bounded allocation and the state is retained by the private executor.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ExecutionState {
    opcode_fuel_remaining: u64,
    max_opcode_fuel: u64,
    operation_opcode_fuel_remaining: u64,
    max_operation_opcode_fuel: u64,
    native_access_remaining: u64,
    deadline_ns: u64,
    operation_deadline_ns: u64,
    operation_active: bool,
    deadline_failed: bool,
    max_method_duration_ns: u64,
    max_operation_duration_ns: u64,
    deadline_check_countdown: u16,
    max_call_depth: u32,
    call_depth: u32,
    max_loop_iterations: u64,
    loop_iterations: u64,
    max_operation_loop_iterations: u64,
    operation_loop_iterations: u64,
    max_namespace_objects: u32,
    namespace_objects: u32,
    max_package_bytes: u64,
    package_bytes: u64,
    working_package_bytes: u64,
    stale_working_package_bytes: u64,
    max_buffer_bytes: u64,
    buffer_bytes: u64,
    working_buffer_bytes: u64,
    stale_working_buffer_bytes: u64,
    max_heap_bytes: u64,
    heap_bytes: u64,
    working_heap_bytes: u64,
    // Reservations whose originating work unit ended before their RAII guard
    // dropped remain live and continue to count against the global heap cap.
    stale_working_heap_bytes: u64,
    work_unit_generation: u64,
}

impl ExecutionState {
    pub fn new(budget: ExecutionBudget) -> Result<Self, LoadError> {
        Ok(Self {
            opcode_fuel_remaining: budget.opcode_fuel,
            max_opcode_fuel: budget.opcode_fuel,
            operation_opcode_fuel_remaining: budget.max_operation_opcode_fuel,
            max_operation_opcode_fuel: budget.max_operation_opcode_fuel,
            native_access_remaining: budget.native_access_fuel,
            deadline_ns: 0,
            operation_deadline_ns: 0,
            operation_active: false,
            deadline_failed: false,
            max_method_duration_ns: budget.max_method_duration_ns,
            max_operation_duration_ns: budget.max_operation_duration_ns,
            deadline_check_countdown: 0,
            max_call_depth: budget.max_call_depth,
            call_depth: 0,
            max_loop_iterations: budget.max_loop_iterations,
            loop_iterations: 0,
            max_operation_loop_iterations: budget.max_operation_loop_iterations,
            operation_loop_iterations: 0,
            max_namespace_objects: budget.max_namespace_objects,
            namespace_objects: 0,
            max_package_bytes: budget.max_package_bytes,
            package_bytes: 0,
            working_package_bytes: 0,
            stale_working_package_bytes: 0,
            max_buffer_bytes: budget.max_buffer_bytes,
            buffer_bytes: 0,
            working_buffer_bytes: 0,
            stale_working_buffer_bytes: 0,
            max_heap_bytes: budget.max_heap_bytes,
            heap_bytes: 0,
            working_heap_bytes: 0,
            stale_working_heap_bytes: 0,
            work_unit_generation: 0,
        })
    }

    pub fn check_deadline(&mut self, now_ns: u64) -> Result<(), LoadError> {
        if self.deadline_failed {
            return Err(LoadError::DeadlineExceeded);
        }
        if (self.deadline_ns != 0 && now_ns >= self.deadline_ns)
            || (self.operation_deadline_ns != 0 && now_ns >= self.operation_deadline_ns)
        {
            self.deadline_failed = true;
            return Err(LoadError::DeadlineExceeded);
        }
        Ok(())
    }

    pub fn remaining_deadline_ns(&mut self, now_ns: u64) -> Result<Option<u64>, LoadError> {
        self.check_deadline(now_ns)?;
        let deadline = match (self.deadline_ns, self.operation_deadline_ns) {
            (0, 0) => return Ok(None),
            (0, operation) => operation,
            (method, 0) => method,
            (method, operation) => method.min(operation),
        };
        Ok(Some(deadline - now_ns))
    }

    pub fn needs_method_deadline(&self) -> bool {
        !self.deadline_failed && self.call_depth == 0 && self.max_method_duration_ns != 0
    }

    pub fn begin_work_unit_at(&mut self, now_ns: u64) -> Result<(), LoadError> {
        if self.deadline_failed {
            return Err(LoadError::DeadlineExceeded);
        }
        if self.operation_active || self.call_depth != 0 {
            return Err(LoadError::InvalidProfile);
        }
        let next_work_unit_generation = self
            .work_unit_generation
            .checked_add(1)
            .ok_or(LoadError::BudgetExceeded)?;
        let operation_deadline_ns = if self.max_operation_duration_ns == 0 {
            0
        } else {
            match now_ns.checked_add(self.max_operation_duration_ns) {
                Some(deadline) => deadline,
                None => {
                    self.deadline_failed = true;
                    return Err(LoadError::DeadlineExceeded);
                }
            }
        };
        self.operation_opcode_fuel_remaining = self.max_operation_opcode_fuel;
        self.operation_loop_iterations = 0;
        self.working_heap_bytes = 0;
        self.working_package_bytes = 0;
        self.working_buffer_bytes = 0;
        self.operation_deadline_ns = operation_deadline_ns;
        self.work_unit_generation = next_work_unit_generation;
        self.operation_active = true;
        self.deadline_check_countdown = 0;
        Ok(())
    }

    pub fn end_work_unit(&mut self) {
        self.operation_active = false;
        self.operation_deadline_ns = 0;
        self.deadline_check_countdown = 0;
        self.stale_working_heap_bytes = self
            .stale_working_heap_bytes
            .saturating_add(self.working_heap_bytes);
        self.working_heap_bytes = 0;
        self.stale_working_package_bytes = self
            .stale_working_package_bytes
            .saturating_add(self.working_package_bytes);
        self.working_package_bytes = 0;
        self.stale_working_buffer_bytes = self
            .stale_working_buffer_bytes
            .saturating_add(self.working_buffer_bytes);
        self.working_buffer_bytes = 0;
    }

    pub fn enter_call_at(&mut self, now_ns: u64) -> Result<(), LoadError> {
        if self.deadline_failed {
            return Err(LoadError::DeadlineExceeded);
        }
        if self.call_depth >= self.max_call_depth {
            return Err(LoadError::BudgetExceeded);
        }
        if self.call_depth == 0 {
            self.begin_top_level_operation();
            self.deadline_ns = if self.max_method_duration_ns == 0 {
                0
            } else {
                match now_ns.checked_add(self.max_method_duration_ns) {
                    Some(deadline) => deadline,
                    None => {
                        self.deadline_failed = true;
                        return Err(LoadError::DeadlineExceeded);
                    }
                }
            };
            self.deadline_check_countdown = 0;
        }
        self.call_depth += 1;
        Ok(())
    }

    /// Sample the clock on method entry and once per bounded opcode batch,
    /// not once per opcode/syscall.
    pub fn deadline_check_due(&mut self) -> bool {
        if self.deadline_failed || (self.deadline_ns == 0 && self.operation_deadline_ns == 0) {
            return false;
        }
        if self.deadline_check_countdown == 0 {
            self.deadline_check_countdown = DEADLINE_OPCODE_SAMPLE_INTERVAL - 1;
            return true;
        }
        self.deadline_check_countdown -= 1;
        false
    }

    pub fn deadline_failed(&self) -> bool {
        self.deadline_failed
    }

    pub fn charge_opcode(&mut self, amount: u64) -> Result<(), LoadError> {
        if amount > self.opcode_fuel_remaining
            || (self.operation_active && amount > self.operation_opcode_fuel_remaining)
        {
            return Err(LoadError::BudgetExceeded);
        }
        self.opcode_fuel_remaining -= amount;
        if self.operation_active {
            self.operation_opcode_fuel_remaining -= amount;
        }
        Ok(())
    }

    pub fn enter_call(&mut self) -> Result<(), LoadError> {
        if self.call_depth >= self.max_call_depth {
            return Err(LoadError::BudgetExceeded);
        }
        if self.call_depth == 0 {
            self.begin_top_level_operation();
        }
        self.call_depth += 1;
        Ok(())
    }

    fn begin_top_level_operation(&mut self) {
        self.opcode_fuel_remaining = self.max_opcode_fuel;
        self.loop_iterations = 0;
    }

    pub fn is_top_level(&self) -> bool {
        self.call_depth == 0
    }

    pub fn leave_call(&mut self) {
        self.call_depth = self.call_depth.saturating_sub(1);
        if self.call_depth == 0 {
            self.deadline_ns = 0;
            self.deadline_check_countdown = 0;
        }
    }

    pub fn loop_iteration(&mut self) -> Result<(), LoadError> {
        if self.loop_iterations >= self.max_loop_iterations
            || (self.operation_active
                && self.operation_loop_iterations >= self.max_operation_loop_iterations)
        {
            return Err(LoadError::BudgetExceeded);
        }
        self.loop_iterations += 1;
        if self.operation_active {
            self.operation_loop_iterations += 1;
        }
        Ok(())
    }

    pub fn add_namespace_objects(&mut self, count: u32) -> Result<(), LoadError> {
        let namespace_objects = self
            .namespace_objects
            .checked_add(count)
            .ok_or(LoadError::BudgetExceeded)?;
        if namespace_objects > self.max_namespace_objects {
            return Err(LoadError::BudgetExceeded);
        }
        self.namespace_objects = namespace_objects;
        Ok(())
    }

    fn checked_persistent_heap_total(&self, bytes: u64) -> Result<u64, LoadError> {
        let heap_bytes = self
            .heap_bytes
            .checked_add(bytes)
            .ok_or(LoadError::BudgetExceeded)?;
        let working_heap_bytes = self
            .working_heap_bytes
            .checked_add(self.stale_working_heap_bytes)
            .ok_or(LoadError::BudgetExceeded)?;
        let total_heap = heap_bytes
            .checked_add(working_heap_bytes)
            .ok_or(LoadError::BudgetExceeded)?;
        if total_heap > self.max_heap_bytes {
            return Err(LoadError::BudgetExceeded);
        }
        Ok(heap_bytes)
    }

    /// Reserve one actual AML namespace declaration together with its
    /// conservative node/name payload cost. This is charged at parser
    /// insertion time, including every `FieldUnit` in a `FieldList`.
    pub fn reserve_namespace_object(&mut self, heap_bytes: u64) -> Result<(), LoadError> {
        let object_count = self
            .namespace_objects
            .checked_add(1)
            .ok_or(LoadError::BudgetExceeded)?;
        if object_count > self.max_namespace_objects {
            return Err(LoadError::BudgetExceeded);
        }
        let heap_bytes_total = self.checked_persistent_heap_total(heap_bytes)?;
        self.namespace_objects = object_count;
        self.heap_bytes = heap_bytes_total;
        Ok(())
    }

    pub fn release_namespace_object(&mut self, heap_bytes: u64) -> Result<(), LoadError> {
        let object_count = self
            .namespace_objects
            .checked_sub(1)
            .ok_or(LoadError::InvalidProfile)?;
        let heap_bytes_total = self
            .heap_bytes
            .checked_sub(heap_bytes)
            .ok_or(LoadError::InvalidProfile)?;
        self.namespace_objects = object_count;
        self.heap_bytes = heap_bytes_total;
        Ok(())
    }

    pub fn reserve_package(&mut self, bytes: u64) -> Result<(), LoadError> {
        let package_bytes = self
            .package_bytes
            .checked_add(bytes)
            .ok_or(LoadError::BudgetExceeded)?;
        let total_package_bytes = package_bytes
            .checked_add(self.working_package_bytes)
            .and_then(|total| total.checked_add(self.stale_working_package_bytes))
            .ok_or(LoadError::BudgetExceeded)?;
        if total_package_bytes > self.max_package_bytes {
            return Err(LoadError::BudgetExceeded);
        }
        let heap_bytes = self.checked_persistent_heap_total(bytes)?;
        self.package_bytes = package_bytes;
        self.heap_bytes = heap_bytes;
        Ok(())
    }

    pub fn reserve_buffer(&mut self, bytes: u64) -> Result<(), LoadError> {
        let buffer_bytes = self
            .buffer_bytes
            .checked_add(bytes)
            .ok_or(LoadError::BudgetExceeded)?;
        let total_buffer_bytes = buffer_bytes
            .checked_add(self.working_buffer_bytes)
            .and_then(|total| total.checked_add(self.stale_working_buffer_bytes))
            .ok_or(LoadError::BudgetExceeded)?;
        if total_buffer_bytes > self.max_buffer_bytes {
            return Err(LoadError::BudgetExceeded);
        }
        let heap_bytes = self.checked_persistent_heap_total(bytes)?;
        self.buffer_bytes = buffer_bytes;
        self.heap_bytes = heap_bytes;
        Ok(())
    }

    pub fn rollback_buffer_reservation(&mut self, bytes: u64) {
        self.buffer_bytes = self.buffer_bytes.saturating_sub(bytes);
        self.heap_bytes = self.heap_bytes.saturating_sub(bytes);
    }

    pub fn reserve_heap(&mut self, bytes: u64) -> Result<(), LoadError> {
        self.heap_bytes = self.checked_persistent_heap_total(bytes)?;
        Ok(())
    }

    pub fn release_heap(&mut self, bytes: u64) -> Result<(), LoadError> {
        self.heap_bytes = self
            .heap_bytes
            .checked_sub(bytes)
            .ok_or(LoadError::InvalidProfile)?;
        Ok(())
    }

    pub fn release_persistent_allocation(
        &mut self,
        heap_bytes: u64,
        package_bytes: u64,
        buffer_bytes: u64,
    ) -> Result<(), LoadError> {
        let heap_total = self
            .heap_bytes
            .checked_sub(heap_bytes)
            .ok_or(LoadError::InvalidProfile)?;
        let package_total = self
            .package_bytes
            .checked_sub(package_bytes)
            .ok_or(LoadError::InvalidProfile)?;
        let buffer_total = self
            .buffer_bytes
            .checked_sub(buffer_bytes)
            .ok_or(LoadError::InvalidProfile)?;
        self.heap_bytes = heap_total;
        self.package_bytes = package_total;
        self.buffer_bytes = buffer_total;
        Ok(())
    }

    pub fn reserve_working_heap(&mut self, bytes: u64) -> Result<(), LoadError> {
        self.reserve_working_allocation(bytes, 0, 0)
    }

    pub fn reserve_working_allocation(
        &mut self,
        heap_bytes: u64,
        package_bytes: u64,
        buffer_bytes: u64,
    ) -> Result<(), LoadError> {
        if !self.operation_active {
            return Err(LoadError::InvalidProfile);
        }
        let working_heap_total = self
            .working_heap_bytes
            .checked_add(heap_bytes)
            .ok_or(LoadError::BudgetExceeded)?;
        let working_and_stale_heap = self
            .stale_working_heap_bytes
            .checked_add(working_heap_total)
            .ok_or(LoadError::BudgetExceeded)?;
        let total_heap = self
            .heap_bytes
            .checked_add(working_and_stale_heap)
            .ok_or(LoadError::BudgetExceeded)?;
        let working_package_total = self
            .working_package_bytes
            .checked_add(package_bytes)
            .ok_or(LoadError::BudgetExceeded)?;
        let total_package_bytes = self
            .package_bytes
            .checked_add(self.stale_working_package_bytes)
            .and_then(|bytes| bytes.checked_add(working_package_total))
            .ok_or(LoadError::BudgetExceeded)?;
        let working_buffer_total = self
            .working_buffer_bytes
            .checked_add(buffer_bytes)
            .ok_or(LoadError::BudgetExceeded)?;
        let total_buffer_bytes = self
            .buffer_bytes
            .checked_add(self.stale_working_buffer_bytes)
            .and_then(|bytes| bytes.checked_add(working_buffer_total))
            .ok_or(LoadError::BudgetExceeded)?;
        if total_heap > self.max_heap_bytes
            || total_package_bytes > self.max_package_bytes
            || total_buffer_bytes > self.max_buffer_bytes
        {
            return Err(LoadError::BudgetExceeded);
        }
        self.working_heap_bytes = working_heap_total;
        self.working_package_bytes = working_package_total;
        self.working_buffer_bytes = working_buffer_total;
        Ok(())
    }

    pub fn release_working_heap(&mut self, bytes: u64) {
        self.release_working_heap_for_generation(bytes, self.work_unit_generation);
    }

    fn release_working_heap_for_generation(&mut self, bytes: u64, generation: u64) {
        self.release_working_allocation_for_generation(bytes, 0, 0, generation);
    }

    pub fn release_working_allocation_for_generation(
        &mut self,
        heap_bytes: u64,
        package_bytes: u64,
        buffer_bytes: u64,
        generation: u64,
    ) {
        if self.operation_active && self.work_unit_generation == generation {
            self.working_heap_bytes = self.working_heap_bytes.saturating_sub(heap_bytes);
            self.working_package_bytes = self.working_package_bytes.saturating_sub(package_bytes);
            self.working_buffer_bytes = self.working_buffer_bytes.saturating_sub(buffer_bytes);
        } else {
            self.stale_working_heap_bytes =
                self.stale_working_heap_bytes.saturating_sub(heap_bytes);
            self.stale_working_package_bytes = self
                .stale_working_package_bytes
                .saturating_sub(package_bytes);
            self.stale_working_buffer_bytes =
                self.stale_working_buffer_bytes.saturating_sub(buffer_bytes);
        }
    }

    pub fn commit_working_heap(&mut self, bytes: u64) -> Result<(), LoadError> {
        self.commit_working_heap_for_generation(bytes, self.work_unit_generation)
    }

    fn commit_working_heap_for_generation(
        &mut self,
        bytes: u64,
        generation: u64,
    ) -> Result<(), LoadError> {
        self.commit_working_allocation_for_generation(bytes, 0, 0, generation)
    }

    pub fn commit_working_allocation_for_generation(
        &mut self,
        heap_bytes: u64,
        package_bytes: u64,
        buffer_bytes: u64,
        generation: u64,
    ) -> Result<(), LoadError> {
        if !self.operation_active || self.work_unit_generation != generation {
            return Err(LoadError::InvalidProfile);
        }
        let working_heap_total = self
            .working_heap_bytes
            .checked_sub(heap_bytes)
            .ok_or(LoadError::InvalidProfile)?;
        let heap_bytes_total = self
            .heap_bytes
            .checked_add(heap_bytes)
            .ok_or(LoadError::BudgetExceeded)?;
        let working_package_total = self
            .working_package_bytes
            .checked_sub(package_bytes)
            .ok_or(LoadError::InvalidProfile)?;
        let package_bytes_total = self
            .package_bytes
            .checked_add(package_bytes)
            .ok_or(LoadError::BudgetExceeded)?;
        let working_buffer_total = self
            .working_buffer_bytes
            .checked_sub(buffer_bytes)
            .ok_or(LoadError::InvalidProfile)?;
        let buffer_bytes_total = self
            .buffer_bytes
            .checked_add(buffer_bytes)
            .ok_or(LoadError::BudgetExceeded)?;
        if heap_bytes_total
            .checked_add(working_heap_total)
            .and_then(|total| total.checked_add(self.stale_working_heap_bytes))
            .ok_or(LoadError::BudgetExceeded)?
            > self.max_heap_bytes
            || package_bytes_total
                .checked_add(working_package_total)
                .and_then(|total| total.checked_add(self.stale_working_package_bytes))
                .ok_or(LoadError::BudgetExceeded)?
                > self.max_package_bytes
            || buffer_bytes_total
                .checked_add(working_buffer_total)
                .and_then(|total| total.checked_add(self.stale_working_buffer_bytes))
                .ok_or(LoadError::BudgetExceeded)?
                > self.max_buffer_bytes
        {
            return Err(LoadError::BudgetExceeded);
        }
        self.heap_bytes = heap_bytes_total;
        self.working_heap_bytes = working_heap_total;
        self.package_bytes = package_bytes_total;
        self.working_package_bytes = working_package_total;
        self.buffer_bytes = buffer_bytes_total;
        self.working_buffer_bytes = working_buffer_total;
        Ok(())
    }

    pub fn native_access(&mut self) -> Result<(), LoadError> {
        if self.native_access_remaining == 0 {
            return Err(LoadError::NativeAccessRejected);
        }
        self.native_access_remaining -= 1;
        Ok(())
    }

    pub fn opcode_fuel_remaining(&self) -> u64 {
        self.opcode_fuel_remaining
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum LoadError {
    MissingDsdt,
    InvalidTable,
    InvalidProfile,
    TableOrder,
    BudgetExceeded,
    DeadlineExceeded,
    AmlRejected,
    NativeAccessRejected,
    InvalidIdentity,
    GlobalLockUnsupported,
    GlobalLockTimeout,
    GlobalLockOutcomeUnknown,
    NativeRegionOutcomeUnknown,
    OscUnavailable,
    OscInvalidResponse,
    OscInvalidRequest,
    OscRejected { error_bits: u32 },
    CleanupFailed,
    GenerationExhausted,
    InvalidRootPath,
    InvalidPciRoot,
    InvalidGpeNumber,
    SleepStateUnavailable,
    InvalidSleepState,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct SleepTypes {
    pub type_a: u8,
    pub type_b: u8,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum GpeHandlerKind {
    Level,
    Edge,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum GpeDispatchResult {
    Handled(GpeHandlerKind),
    NoHandler,
}

/// PCI root-bridge `_OSC` UUID in the byte order used by the ACPI UUID
/// buffer argument (the first three UUID fields are little-endian).
pub const PCI_OSC_UUID: [u8; 16] = [
    0x5b, 0x4d, 0xdb, 0x33, 0xf7, 0x1f, 0x1c, 0x40, 0x96, 0x57, 0x74, 0x41, 0xc0, 0x3d, 0xd7, 0x66,
];
pub const PCI_OSC_REVISION: u64 = 1;
pub const PCI_OSC_ARGUMENT_COUNT: u64 = 3;
pub const OSC_QUERY_ENABLE: u32 = 0x0000_0001;
pub const OSC_REQUEST_ERROR: u32 = 0x0000_0002;
pub const OSC_INVALID_UUID_ERROR: u32 = 0x0000_0004;
pub const OSC_INVALID_REVISION_ERROR: u32 = 0x0000_0008;
pub const OSC_CAPABILITIES_MASK_ERROR: u32 = 0x0000_0010;
pub const OSC_ERROR_MASK: u32 = OSC_REQUEST_ERROR
    | OSC_INVALID_UUID_ERROR
    | OSC_INVALID_REVISION_ERROR
    | OSC_CAPABILITIES_MASK_ERROR;
pub const PCI_OSC_SUPPORT_MASK: u32 = 0x0000_019f;
pub const PCI_OSC_CONTROL_MASK: u32 = 0x0000_00bf;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum PciOscPhase {
    Query,
    Commit,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct PciOscResponse {
    pub error_bits: u32,
    pub support: u32,
    pub control: u32,
    pub denied_control: u32,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct PciOscNegotiation {
    pub firmware_version: String,
    pub aml_namespace_generation: u64,
    pub compatibility_profile_hash: u64,
    pub requested_support: u32,
    pub requested_control: u32,
    pub query_errors: u32,
    pub query_support: u32,
    pub query_granted_control: u32,
    pub masked_control: u32,
    pub commit_errors: u32,
    pub commit_support: u32,
    pub granted_control: u32,
    pub denied_control: u32,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
struct PciOscExchange {
    query_errors: u32,
    query_support: u32,
    query_granted_control: u32,
    masked_control: u32,
    commit_errors: u32,
    commit_support: u32,
    granted_control: u32,
    denied_control: u32,
}

/// Backend-private PCI root fact. `namespace_path` is retained only inside
/// platformd so the executor can address the root's `_OSC`; it is not a wire
/// field and must not be exposed to pcied or drivers.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct PciRootBridgeDescriptor {
    pub root_id: u64,
    /// Identity of the platform device node that owns this root bridge.
    pub device_id: u64,
    pub namespace_path: String,
    pub segment: u16,
    pub start_bus: u8,
    pub end_bus: u8,
    pub aml_namespace_generation: u64,
    pub resource_windows: Vec<PciRootResourceWindow>,
}

/// Normalized, address-bearing facts from a PCI root bridge's current `_CRS`.
/// These are policy inputs for `pcied`, not leases or mapping authority.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct PciRootResourceWindow {
    pub resource_type: u8,
    pub descriptor_revision: u32,
    pub general_flags: u32,
    pub type_specific_flags: u32,
    pub granularity: u64,
    pub minimum: u64,
    pub maximum: u64,
    pub translation_offset: u64,
    pub length: u64,
    pub type_specific_attributes: u64,
}

pub const MAX_PCI_ROOT_RESOURCE_WINDOWS: usize = 12;
pub const MAX_PCI_DEVICE_RESOURCE_WINDOWS: usize = 6;
pub const MAX_PCI_ROOTS: usize = 64;
const MAX_PCI_NAMESPACE_PATHS: usize = 16_384;
const MAX_SYSTEM_IO_DEVICES: usize = 256;
const MAX_SYSTEM_IO_DESCRIPTORS_PER_DEVICE: usize = 256;
const MAX_SYSTEM_IO_RESOURCES: usize = 4096;
const MAX_CACHED_CRS_DESCRIPTORS: usize = 16_384;

/// Derive the unique root bus interval from a bounded, normalized `_CRS` set.
/// The returned interval is firmware data only; kernel MCFG containment is a
/// separate admission check before any PCI capability is minted.
pub fn pci_root_bus_window(
    windows: &[PciRootResourceWindow],
) -> Result<Option<(u8, u8)>, LoadError> {
    pci_root_bus_window_with_budget(windows, || Ok(()))
}

fn pci_root_bus_window_with_budget(
    windows: &[PciRootResourceWindow],
    mut charge_iteration: impl FnMut() -> Result<(), LoadError>,
) -> Result<Option<(u8, u8)>, LoadError> {
    if windows.len() > MAX_PCI_ROOT_RESOURCE_WINDOWS {
        return Err(LoadError::BudgetExceeded);
    }
    let mut bus_window = None;
    for window in windows {
        charge_iteration()?;
        if window.resource_type != 2 {
            continue;
        }
        if window.length == 0 || window.minimum > window.maximum || window.type_specific_flags != 0
        {
            return Err(LoadError::InvalidPciRoot);
        }
        let end = window
            .minimum
            .checked_add(window.length - 1)
            .ok_or(LoadError::InvalidPciRoot)?;
        if end > window.maximum {
            return Err(LoadError::InvalidPciRoot);
        }
        let start = u8::try_from(window.minimum).map_err(|_| LoadError::InvalidPciRoot)?;
        let end = u8::try_from(end).map_err(|_| LoadError::InvalidPciRoot)?;
        if bus_window.replace((start, end)).is_some() {
            return Err(LoadError::InvalidPciRoot);
        }
    }
    Ok(bus_window)
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct PciDeviceResourceWindow {
    pub base: u64,
    pub length: u64,
    pub writable: bool,
}

fn reserve_bounded_output<T>(
    output: &mut Vec<T>,
    additional: usize,
    maximum: usize,
) -> Result<(), LoadError> {
    let new_len = output
        .len()
        .checked_add(additional)
        .filter(|length| *length <= maximum)
        .ok_or(LoadError::BudgetExceeded)?;
    let additional = new_len - output.len();
    output
        .try_reserve_exact(additional)
        .map_err(|_| LoadError::BudgetExceeded)
}

fn reserve_bounded_aml_output<T>(
    output: &mut AmlVec<T>,
    additional: usize,
    maximum: usize,
) -> Result<(), LoadError> {
    let new_len = output
        .len()
        .checked_add(additional)
        .filter(|length| *length <= maximum)
        .ok_or(LoadError::BudgetExceeded)?;
    output
        .try_reserve_exact(new_len - output.len())
        .map_err(|_| LoadError::BudgetExceeded)
}

fn reserve_snapshot_output<T>(output: &mut Vec<T>, additional: usize) -> Result<(), LoadError> {
    reserve_bounded_output(output, additional, usize::MAX)
}

fn copy_snapshot_slice<T: Copy>(values: &[T]) -> Result<Vec<T>, LoadError> {
    let mut copy = Vec::new();
    reserve_snapshot_output(&mut copy, values.len())?;
    copy.extend_from_slice(values);
    Ok(copy)
}

fn try_copy_bounded_string(value: &str, maximum: usize) -> Result<String, LoadError> {
    if value.len() > maximum {
        return Err(LoadError::BudgetExceeded);
    }
    let mut copy = String::new();
    reserve_string_output(&mut copy, value.len())?;
    copy.push_str(value);
    Ok(copy)
}

fn try_copy_aml_string(value: &AmlString, maximum: usize) -> Result<String, LoadError> {
    try_copy_bounded_string(value.as_str(), maximum)
}

fn try_copy_system_io_resource(
    resource: &crate::region::SystemIoResource,
) -> Result<crate::region::SystemIoResource, LoadError> {
    if resource.parent_device_path.len() > 255 {
        return Err(LoadError::BudgetExceeded);
    }
    Ok(crate::region::SystemIoResource {
        parent_device_path: try_copy_bounded_string(&resource.parent_device_path, 255)?,
        base: resource.base,
        length: resource.length,
    })
}

fn reserve_string_output(output: &mut String, additional: usize) -> Result<(), LoadError> {
    output
        .try_reserve_exact(additional)
        .map_err(|_| LoadError::BudgetExceeded)
}

fn try_copy_bounded_slice<T: Copy>(values: &[T], maximum: usize) -> Result<Vec<T>, LoadError> {
    let mut copy = Vec::new();
    reserve_bounded_output(&mut copy, values.len(), maximum)?;
    copy.extend_from_slice(values);
    Ok(copy)
}

/// Normalize only fixed, consumer-owned PCI device memory windows. Device
/// translations are rejected because applying a leaf offset without the
/// complete bridge translation chain could attest the wrong host address.
pub fn pci_device_memory_windows(
    descriptors: &[PciRootResourceWindow],
) -> Result<Vec<PciDeviceResourceWindow>, LoadError> {
    if descriptors.len() > 256 {
        return Err(LoadError::BudgetExceeded);
    }
    let mut result = Vec::new();
    reserve_bounded_output(
        &mut result,
        descriptors.len().min(MAX_PCI_DEVICE_RESOURCE_WINDOWS),
        MAX_PCI_DEVICE_RESOURCE_WINDOWS,
    )?;
    for descriptor in descriptors {
        match descriptor.resource_type {
            1 | 2 | 192..=255 => continue,
            0 => {}
            _ => return Err(LoadError::InvalidPciRoot),
        }
        if descriptor.descriptor_revision > 1
            || (descriptor.general_flags & 0xf0) != 0
            || descriptor.general_flags & 0x0d != 0x0d
            || descriptor.general_flags & 0x02 != 0
            || descriptor.type_specific_flags & 0x80 != 0
            || descriptor.type_specific_attributes != 0
            || descriptor.translation_offset != 0
        {
            return Err(LoadError::InvalidPciRoot);
        }
        if descriptor.granularity != u64::MAX
            && (descriptor.granularity == u64::MAX - 1
                || (descriptor.granularity + 1) & descriptor.granularity != 0)
        {
            return Err(LoadError::InvalidPciRoot);
        }
        if descriptor.length == 0 || descriptor.minimum > descriptor.maximum {
            return Err(LoadError::InvalidPciRoot);
        }
        let last = descriptor
            .minimum
            .checked_add(descriptor.length - 1)
            .ok_or(LoadError::InvalidPciRoot)?;
        let end = descriptor
            .minimum
            .checked_add(descriptor.length)
            .ok_or(LoadError::InvalidPciRoot)?;
        if last > descriptor.maximum {
            return Err(LoadError::InvalidPciRoot);
        }
        if result.iter().any(|window: &PciDeviceResourceWindow| {
            let other_end = window.base + window.length;
            descriptor.minimum < other_end && window.base < end
        }) {
            return Err(LoadError::InvalidPciRoot);
        }
        if result.len() == MAX_PCI_DEVICE_RESOURCE_WINDOWS {
            return Err(LoadError::BudgetExceeded);
        }
        result.push(PciDeviceResourceWindow {
            base: descriptor.minimum,
            length: descriptor.length,
            writable: descriptor.type_specific_flags & 1 != 0,
        });
    }
    Ok(result)
}

/// Kernel-facing selector for a PCI_Config OperationRegion's containing
/// function. The namespace path stays private to platformd; only the packed
/// root-relative `_ADR` chain is sent to the kernel authority.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct PciConfigRegionPath {
    pub parent_device_path: String,
    pub root_id: u64,
    pub segment: u16,
    pub root_start_bus: u8,
    pub root_end_bus: u8,
    pub device_function_path: Vec<u8>,
}

pub const MAX_PCI_DEVICE_FUNCTION_PATH: usize = 16;
const MAX_NAMESPACE_PATH_BYTES: usize = 1281;

/// Firmware-neutral identity of one ACPI device under a PCI root. The packed
/// address path is root-relative and contains no namespace path or authority.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct PciDeviceFunctionPath {
    pub device_id: u64,
    pub root_id: u64,
    pub device_function_path: Vec<u8>,
}

fn pack_pci_device_function_path(addresses: &[u64]) -> Result<Option<Vec<u8>>, LoadError> {
    pack_pci_device_function_path_with_budget(addresses, || Ok(()))
}

fn pack_pci_device_function_path_with_budget(
    addresses: &[u64],
    mut charge_iteration: impl FnMut() -> Result<(), LoadError>,
) -> Result<Option<Vec<u8>>, LoadError> {
    if addresses.is_empty() || addresses.len() > MAX_PCI_DEVICE_FUNCTION_PATH {
        return Ok(None);
    }
    let mut packed = Vec::new();
    reserve_bounded_output(&mut packed, addresses.len(), MAX_PCI_DEVICE_FUNCTION_PATH)?;
    for address in addresses {
        charge_iteration()?;
        if *address > u64::from(u32::MAX) {
            return Ok(None);
        }
        let device = (*address >> 16) as u32;
        let function = (*address & 0xffff) as u32;
        if device >= 32 || function >= 8 {
            return Ok(None);
        }
        packed.push((device as u8) | ((function as u8) << 5));
    }
    Ok(Some(packed))
}

fn collect_pci_device_path_ancestors<'a, S: AsRef<str>>(
    root_path: &str,
    target_path: &str,
    sorted_namespace_devices: &'a [S],
    output: &mut Vec<&'a str>,
) -> bool {
    output.clear();
    if target_path == root_path {
        return true;
    }
    let Some(suffix) = target_path.strip_prefix(root_path) else {
        return false;
    };
    let Some(suffix) = suffix.strip_prefix('.') else {
        return false;
    };

    let Some(mut prefix_end) = root_path.len().checked_add(1) else {
        return false;
    };
    for segment in suffix.split('.') {
        if segment.is_empty() {
            return false;
        }
        let Some(candidate_end) = prefix_end.checked_add(segment.len()) else {
            return false;
        };
        let Some(candidate) = target_path.get(..candidate_end) else {
            return false;
        };
        if let Ok(device_index) =
            sorted_namespace_devices.binary_search_by(|path| path.as_ref().cmp(candidate))
        {
            if output.len() == MAX_PCI_DEVICE_FUNCTION_PATH {
                return false;
            }
            output.push(sorted_namespace_devices[device_index].as_ref());
        }
        let Some(next_prefix_end) = candidate_end.checked_add(1) else {
            return false;
        };
        prefix_end = next_prefix_end;
    }
    true
}

fn collect_pci_device_path_ancestors_metered<'a, S: AsRef<str>>(
    root_path: &str,
    target_path: &str,
    sorted_namespace_devices: &'a [S],
    output: &mut Vec<&'a str>,
    mut charge_iteration: impl FnMut() -> Result<(), LoadError>,
) -> Result<bool, LoadError> {
    output.clear();
    if target_path == root_path {
        return Ok(true);
    }
    let Some(suffix) = target_path.strip_prefix(root_path) else {
        return Ok(false);
    };
    let Some(suffix) = suffix.strip_prefix('.') else {
        return Ok(false);
    };

    let Some(mut prefix_end) = root_path.len().checked_add(1) else {
        return Ok(false);
    };
    for segment in suffix.split('.') {
        charge_iteration()?;
        if segment.is_empty() {
            return Ok(false);
        }
        let Some(candidate_end) = prefix_end.checked_add(segment.len()) else {
            return Ok(false);
        };
        let Some(candidate) = target_path.get(..candidate_end) else {
            return Ok(false);
        };

        let mut low = 0;
        let mut high = sorted_namespace_devices.len();
        while low < high {
            charge_iteration()?;
            let middle = low + (high - low) / 2;
            match sorted_namespace_devices[middle].as_ref().cmp(candidate) {
                core::cmp::Ordering::Less => low = middle + 1,
                core::cmp::Ordering::Greater => high = middle,
                core::cmp::Ordering::Equal => {
                    if output.len() == MAX_PCI_DEVICE_FUNCTION_PATH {
                        return Ok(false);
                    }
                    output.push(sorted_namespace_devices[middle].as_ref());
                    break;
                }
            }
        }
        let Some(next_prefix_end) = candidate_end.checked_add(1) else {
            return Ok(false);
        };
        prefix_end = next_prefix_end;
    }
    Ok(true)
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct PciRootDiscovery {
    pub roots: Vec<PciRootBridgeDescriptor>,
    pub device_candidates: u32,
    pub hid_matches: u32,
    pub osc_candidates: u32,
    pub adr_zero_candidates: u32,
    pub crs_address_spaces: u32,
    pub bus_range_matches: u32,
}

/// Decode the fixed three-DWORD buffer returned by a PCI root `_OSC` method.
/// Only defined error flags are interpreted; reserved DWORD0 bits (including
/// firmware that echoes Query Enable) are not errors. The capabilities-mask
/// error is retained in the result but does not turn a partial negotiation
/// into a false all-or-nothing failure. Other defined error bits reject the
/// negotiation, and the caller must never mint authority for denied controls.
pub fn parse_pci_osc_response(
    _phase: PciOscPhase,
    requested_control: u32,
    response: &[u8],
) -> Result<PciOscResponse, LoadError> {
    if response.len() != 3 * core::mem::size_of::<u32>() {
        return Err(LoadError::OscInvalidResponse);
    }
    let error_bits = u32::from_le_bytes(
        response[0..4]
            .try_into()
            .map_err(|_| LoadError::OscInvalidResponse)?,
    );
    let raw_support = u32::from_le_bytes(
        response[4..8]
            .try_into()
            .map_err(|_| LoadError::OscInvalidResponse)?,
    );
    let raw_control = u32::from_le_bytes(
        response[8..12]
            .try_into()
            .map_err(|_| LoadError::OscInvalidResponse)?,
    );
    let defined_errors = error_bits & OSC_ERROR_MASK;
    if defined_errors & !OSC_CAPABILITIES_MASK_ERROR != 0 {
        return Err(LoadError::OscRejected { error_bits });
    }
    let support = raw_support & PCI_OSC_SUPPORT_MASK;
    let control = raw_control & PCI_OSC_CONTROL_MASK;
    Ok(PciOscResponse {
        error_bits: defined_errors,
        support,
        control,
        denied_control: requested_control & !control,
    })
}

fn validate_pci_osc_request(
    requested_support: u32,
    requested_control: u32,
) -> Result<(), LoadError> {
    if requested_support & !PCI_OSC_SUPPORT_MASK != 0
        || requested_control & !PCI_OSC_CONTROL_MASK != 0
    {
        return Err(LoadError::OscInvalidRequest);
    }
    Ok(())
}

fn negotiate_pci_osc_exchange(
    requested_support: u32,
    requested_control: u32,
    mut evaluate: impl FnMut(bool, u32, u32) -> Result<PciOscResponse, LoadError>,
) -> Result<PciOscExchange, LoadError> {
    validate_pci_osc_request(requested_support, requested_control)?;
    const MAX_QUERY_ATTEMPTS: usize = u32::BITS as usize + 1;

    let mut query_support = requested_support;
    let mut query_control = requested_control;
    let mut attempts = 0;
    let query = loop {
        let response = evaluate(true, query_support, query_control)?;
        attempts += 1;

        if response.error_bits & OSC_CAPABILITIES_MASK_ERROR == 0 {
            if response.support != query_support || response.control != query_control {
                return Err(LoadError::OscRejected {
                    error_bits: response.error_bits | OSC_CAPABILITIES_MASK_ERROR,
                });
            }
            break response;
        }

        let narrowed_support = query_support & response.support;
        let narrowed_control = query_control & response.control;
        if (narrowed_support == query_support && narrowed_control == query_control)
            || attempts >= MAX_QUERY_ATTEMPTS
        {
            return Err(LoadError::OscRejected {
                error_bits: response.error_bits,
            });
        }

        query_support = narrowed_support;
        query_control = narrowed_control;
    };

    // Query calls cannot change hardware state. Commit only the mask whose
    // final query completed without Capabilities Masked; repeating a wider
    // original request here could ask firmware to transfer control that the
    // query did not authorize.
    let commit = evaluate(false, query_support, query_control)?;
    let query_granted_control = query.control & query_control;
    let granted_control = query_granted_control & commit.control;
    Ok(PciOscExchange {
        query_errors: query.error_bits,
        query_support: query.support,
        query_granted_control,
        masked_control: requested_control & !query.control,
        commit_errors: commit.error_bits,
        commit_support: commit.support,
        granted_control,
        denied_control: requested_control & !granted_control,
    })
}

fn table_descriptor(bytes: &[u8], expected: &[u8; 4]) -> Result<TableDescriptor, LoadError> {
    if bytes.len() < 36 || &bytes[..4] != expected {
        return Err(LoadError::InvalidTable);
    }
    let length = u32::from_le_bytes(
        bytes[4..8]
            .try_into()
            .map_err(|_| LoadError::InvalidTable)?,
    );
    if length as usize != bytes.len() || length < 36 {
        return Err(LoadError::InvalidTable);
    }
    Ok(TableDescriptor {
        signature: *expected,
        revision: bytes[8],
        length,
    })
}

#[cfg(any(target_os = "naos", test))]
fn table_hash(mut hash: u64, bytes: &[u8]) -> u64 {
    for byte in bytes {
        hash ^= u64::from(*byte);
        hash = hash.wrapping_mul(0x100000001b3);
    }
    hash
}

#[cfg(any(target_os = "naos", test))]
fn namespace_device_id(generation: u64, namespace_path: &str) -> u64 {
    table_hash(generation, namespace_path.as_bytes()).max(1)
}

#[cfg(any(target_os = "naos", test))]
fn pci_root_identity(generation: u64, namespace_path: &str) -> (u64, u64) {
    let device_id = namespace_device_id(generation, namespace_path);
    let root_id = table_hash(
        table_hash(generation, b"PCI_ROOT\0"),
        namespace_path.as_bytes(),
    )
    .max(1);
    (root_id, device_id)
}

#[cfg(any(target_os = "naos", test))]
mod interpreter {
    use super::*;
    use crate::{
        native::{NativeAccessBudget, NativeRegionAdapter, NativeRegionBackend},
        region::{RegionAccessToken, RegionAccessor, RegionError},
    };
    use acpi::{
        Handle, Handler, RawPhysicalMapping,
        aml::{AmlError, ExecutionMeter, Interpreter},
        aml::{
            namespace::{AmlName, NameComponent, NameSeg, Namespace, NamespaceLevelKind},
            object::{Object, ObjectType, WrappedObject},
        },
    };
    use core::ptr::NonNull;
    use core::str::FromStr;
    use std::{
        collections::HashMap,
        sync::{
            Arc, Mutex,
            atomic::{AtomicBool, AtomicU8, AtomicU32, AtomicU64, Ordering},
        },
        vec::Vec,
    };

    pub(super) fn aml_error_contains_marker(error: &AmlError, marker: &str) -> bool {
        match error {
            AmlError::BudgetExceeded => marker == "AML_BUDGET_EXHAUSTED",
            AmlError::DeadlineExceeded => marker == "AML_DEADLINE_EXCEEDED",
            AmlError::NativeAccessBudgetExceeded => marker == "AML_NATIVE_ACCESS_BUDGET_EXHAUSTED",
            AmlError::HostError(message) | AmlError::InternalError(message) => {
                message.contains(marker)
            }
            _ => false,
        }
    }

    pub(super) fn pci_osc_working_heap_bytes(root_path_bytes: usize) -> Option<u64> {
        if root_path_bytes > 255 {
            return None;
        }

        let component_count = root_path_bytes.checked_add(2)?;
        let path_copy_bytes = core::mem::size_of::<Vec<acpi::aml::namespace::NameComponent>>()
            .checked_add(
                component_count
                    .checked_mul(core::mem::size_of::<acpi::aml::namespace::NameComponent>())?
                    .checked_mul(2)?,
            )?;
        // Cover the parsed root, resolved `_OSC`, normalization temporaries,
        // and one fallible path clone used by each sequential query/commit call.
        let path_bytes = path_copy_bytes.checked_mul(6)?;
        let argument_count = 4_usize;
        let argument_vector_bytes = core::mem::size_of::<Vec<WrappedObject>>()
            .checked_add(argument_count.checked_mul(core::mem::size_of::<WrappedObject>())?)?;
        let object_allocation_bytes = core::mem::size_of::<Object>()
            .checked_add(2_usize.checked_mul(core::mem::size_of::<usize>())?)?;
        let wrapped_object_bytes = argument_count.checked_mul(object_allocation_bytes)?;
        let buffers_bytes = PCI_OSC_UUID
            .len()
            .checked_add(3 * core::mem::size_of::<u32>())?;
        let total_bytes = path_bytes
            .checked_add(argument_vector_bytes)?
            .checked_add(wrapped_object_bytes)?
            .checked_add(buffers_bytes)?
            .checked_add(512)?;
        u64::try_from(total_bytes).ok()
    }

    pub(super) fn decode_pci_routing_table(
        value: &Object,
        namespace: &Namespace,
        generation: u64,
        mut charge_iteration: impl FnMut() -> Result<(), PciRoutingError>,
    ) -> Result<Vec<PciRoutingEntry>, PciRoutingError> {
        fn dereference(mut value: &Object) -> Result<&Object, PciRoutingError> {
            for _ in 0..=MAX_IDENTIFIER_REFERENCE_DEPTH {
                match value {
                    Object::Reference { inner, .. } => value = &**inner,
                    _ => return Ok(value),
                }
            }
            Err(PciRoutingError::InvalidEntry)
        }

        fn integer(value: &Object) -> Result<u64, PciRoutingError> {
            match dereference(value)? {
                Object::Integer(value) => Ok(*value),
                _ => Err(PciRoutingError::InvalidEntry),
            }
        }

        let Object::Package(entries) = value else {
            return Err(PciRoutingError::InvalidTable);
        };
        if entries.len() > MAX_PCI_ROUTING_ENTRIES {
            return Err(PciRoutingError::TooManyEntries);
        }

        let mut routes = Vec::new();
        routes
            .try_reserve_exact(entries.len())
            .map_err(|_| PciRoutingError::AllocationFailed)?;
        let mut seen_pins = [false; 32 * 4];
        for entry in entries {
            charge_iteration()?;
            let Object::Package(fields) = &**entry else {
                return Err(PciRoutingError::InvalidEntry);
            };
            if fields.len() != 4 {
                return Err(PciRoutingError::InvalidEntry);
            }

            let address = integer(&fields[0])?;
            let pin = integer(&fields[1])?;
            let source_index = integer(&fields[3])?;
            if address > u64::from(u32::MAX) || pin > 3 || source_index > u64::from(u32::MAX) {
                return Err(PciRoutingError::InvalidEntry);
            }
            let address = address as u32;
            let device = address >> 16;
            let function = address & 0xffff;
            if device > 31 || function != 0xffff {
                return Err(PciRoutingError::InvalidEntry);
            }

            let source = match dereference(&fields[2])? {
                Object::Integer(0) => PciRoutingSource::DirectGsi(source_index as u32),
                Object::NamePath { name, scope } => {
                    let link_path = namespace
                        .search_for_level(name, scope)
                        .map_err(|_| PciRoutingError::InvalidEntry)?;
                    let link_path = link_path
                        .try_to_string_bounded(MAX_NAMESPACE_PATH_BYTES)
                        .map_err(|_| PciRoutingError::AllocationFailed)?
                        .ok_or(PciRoutingError::InvalidEntry)?;
                    PciRoutingSource::LinkDevice {
                        device_id: super::namespace_device_id(generation, &link_path),
                        source_index: source_index as u32,
                    }
                }
                _ => return Err(PciRoutingError::InvalidEntry),
            };

            let pin_index = (device as usize)
                .checked_mul(4)
                .and_then(|index| index.checked_add(pin as usize))
                .ok_or(PciRoutingError::InvalidEntry)?;
            if seen_pins[pin_index] {
                return Err(PciRoutingError::DuplicatePin);
            }
            seen_pins[pin_index] = true;
            routes.push(PciRoutingEntry {
                address,
                pin: pin as u8,
                source,
            });
        }
        Ok(routes)
    }

    #[derive(Clone)]
    struct OfflineHandler {
        raw_handler_access: Arc<AtomicBool>,
        next_mutex: Arc<AtomicU32>,
    }

    struct DenyAllNativeRegionBackend {
        native_access: Arc<AtomicBool>,
    }

    struct NaosExecutionMeter {
        state: Mutex<ExecutionState>,
        clock: OfflineHandler,
        native_access_budget: Arc<NativeAccessBudget>,
        shared_work_unit_generation: AtomicU64,
        #[cfg(target_os = "naos")]
        heap_quota: Arc<naos_runtime::HeapQuota>,
    }

    struct WorkUnitExecutionScope<'a> {
        meter: &'a NaosExecutionMeter,
        owns_work_unit: bool,
        #[cfg(target_os = "naos")]
        _heap_quota_scope: naos_runtime::HeapQuotaScope,
    }

    pub struct SciWorkUnit {
        meter: AmlArc<NaosExecutionMeter>,
        generation: u64,
        active: bool,
    }

    impl SciWorkUnit {
        pub fn finish(mut self) -> Result<(), LoadError> {
            self.active = false;
            self.meter.end_shared_work_unit(self.generation)
        }
    }

    impl Drop for SciWorkUnit {
        fn drop(&mut self) {
            if self.active {
                let _ = self.meter.end_shared_work_unit(self.generation);
            }
        }
    }

    struct WorkingHeapReservation<'a> {
        meter: &'a NaosExecutionMeter,
        bytes: u64,
        generation: u64,
    }

    impl WorkingHeapReservation<'_> {
        fn commit_to_persistent(mut self) -> Result<(), LoadError> {
            self.meter
                .state
                .lock()
                .map_err(|_| LoadError::AmlRejected)?
                .commit_working_heap_for_generation(self.bytes, self.generation)?;
            self.bytes = 0;
            Ok(())
        }
    }

    struct HardwareSideEffectProfileScope {
        active_profile: Arc<AtomicU8>,
        previous_profile: u8,
    }

    impl HardwareSideEffectProfileScope {
        fn new(active_profile: Arc<AtomicU8>, profile: HardwareSideEffectProfile) -> Self {
            let previous_profile = active_profile.swap(profile.to_bits(), Ordering::AcqRel);
            Self {
                active_profile,
                previous_profile,
            }
        }
    }

    impl Drop for HardwareSideEffectProfileScope {
        fn drop(&mut self) {
            self.active_profile
                .store(self.previous_profile, Ordering::Release);
        }
    }

    pub(super) enum CachedCrsData {
        Missing,
        Invalid,
        Available {
            resources: Vec<ResourceSnapshot>,
            address_spaces: Vec<PciRootResourceWindow>,
            system_io: Vec<crate::region::SystemIoResource>,
        },
    }

    pub(super) type CachedCrs = Arc<CachedCrsData>;

    #[derive(Default)]
    struct CrsCache {
        entries: HashMap<String, CachedCrs>,
        descriptor_count: usize,
        persistent_heap_bytes: u64,
    }

    impl CrsCache {
        fn get(&self, path: &str) -> Option<CachedCrs> {
            self.entries.get(path).cloned()
        }

        fn reserve_descriptors(&self, additional: usize) -> Result<(), LoadError> {
            self.descriptor_count
                .checked_add(additional)
                .filter(|count| *count <= MAX_CACHED_CRS_DESCRIPTORS)
                .map(|_| ())
                .ok_or(LoadError::BudgetExceeded)
        }

        #[cfg(test)]
        fn insert(
            &mut self,
            path: String,
            value: CachedCrs,
        ) -> Result<Option<CachedCrs>, LoadError> {
            self.insert_metered(path, value, 0)
        }

        fn insert_metered(
            &mut self,
            path: String,
            value: CachedCrs,
            persistent_heap_bytes: u64,
        ) -> Result<Option<CachedCrs>, LoadError> {
            if let Some(existing) = self.entries.get(&path) {
                return Ok(Some(Arc::clone(existing)));
            }

            let additional_descriptors = match value.as_ref() {
                CachedCrsData::Available { resources, .. } => resources.len(),
                CachedCrsData::Missing | CachedCrsData::Invalid => 0,
            };
            let next_descriptor_count = self
                .descriptor_count
                .checked_add(additional_descriptors)
                .filter(|count| *count <= MAX_CACHED_CRS_DESCRIPTORS)
                .ok_or(LoadError::BudgetExceeded)?;
            let next_heap_bytes = self
                .persistent_heap_bytes
                .checked_add(persistent_heap_bytes)
                .ok_or(LoadError::BudgetExceeded)?;
            self.entries
                .try_reserve(1)
                .map_err(|_| LoadError::BudgetExceeded)?;
            self.entries.insert(path, value);
            self.descriptor_count = next_descriptor_count;
            self.persistent_heap_bytes = next_heap_bytes;
            Ok(None)
        }

        fn clear(&mut self) -> u64 {
            let persistent_heap_bytes = self.persistent_heap_bytes;
            *self = Self::default();
            persistent_heap_bytes
        }
    }

    fn crs_parser_working_heap_bytes(input_bytes: usize) -> Option<u64> {
        let payload_bytes = input_bytes.checked_mul(2)?;
        let descriptor_slots = MAX_SYSTEM_IO_DESCRIPTORS_PER_DEVICE
            .checked_mul(core::mem::size_of::<acpi::aml::resource::Resource>())?;
        let bytes = payload_bytes
            .checked_add(descriptor_slots)?
            .checked_add(512)?;
        u64::try_from(bytes).ok()
    }

    fn normalized_crs_cache_heap_bytes(
        descriptor_count: usize,
        parent_path_bytes: usize,
    ) -> Option<u64> {
        let per_descriptor = core::mem::size_of::<ResourceSnapshot>()
            .checked_add(2_usize.checked_mul(core::mem::size_of::<PciRootResourceWindow>())?)?
            .checked_add(
                2_usize.checked_mul(
                    core::mem::size_of::<crate::region::SystemIoResource>()
                        .checked_add(parent_path_bytes)?
                        .checked_add(64)?,
                )?,
            )?
            .checked_add(128)?;
        let bytes = descriptor_count
            .checked_mul(per_descriptor)?
            .checked_add(core::mem::size_of::<CachedCrsData>())?
            .checked_add(512)?;
        u64::try_from(bytes).ok()
    }

    fn try_crs_output_vectors(
        descriptor_count: usize,
    ) -> Result<
        (
            Vec<ResourceSnapshot>,
            Vec<PciRootResourceWindow>,
            Vec<crate::region::SystemIoResource>,
        ),
        LoadError,
    > {
        fn reserve<T>(capacity: usize) -> Result<Vec<T>, LoadError> {
            let mut values = Vec::new();
            values
                .try_reserve_exact(capacity)
                .map_err(|_| LoadError::BudgetExceeded)?;
            Ok(values)
        }

        Ok((
            reserve(descriptor_count)?,
            reserve(descriptor_count)?,
            reserve(descriptor_count)?,
        ))
    }

    fn crs_cache_entry_heap_bytes(parent_path_bytes: usize) -> Option<u64> {
        let bytes = core::mem::size_of::<String>()
            .checked_add(parent_path_bytes)?
            .checked_add(512)?;
        u64::try_from(bytes).ok()
    }

    #[cfg(test)]
    mod cached_crs_tests {
        use super::super::LoadError;
        use super::{
            CachedCrs, CachedCrsData, CrsCache, MAX_CACHED_CRS_DESCRIPTORS,
            crs_cache_entry_heap_bytes, crs_parser_working_heap_bytes,
            normalized_crs_cache_heap_bytes, try_crs_output_vectors,
        };
        use std::sync::Arc;

        #[test]
        fn cached_crs_clone_shares_normalized_storage() {
            let cached: CachedCrs = Arc::new(CachedCrsData::Available {
                resources: vec![super::super::ResourceSnapshot {
                    kind: 1,
                    base: 9,
                    length: 1,
                    flags: 0,
                }],
                address_spaces: Vec::new(),
                system_io: Vec::new(),
            });

            let clone = cached.clone();

            assert!(Arc::ptr_eq(&cached, &clone));
        }

        #[test]
        fn cache_descriptor_count_is_updated_only_for_new_entries() {
            let mut cache = CrsCache::default();
            let missing: CachedCrs = Arc::new(CachedCrsData::Missing);
            assert!(
                cache
                    .insert("\\DEV0".to_string(), missing.clone())
                    .unwrap()
                    .is_none()
            );
            assert_eq!(cache.descriptor_count, 0);

            let available: CachedCrs = Arc::new(CachedCrsData::Available {
                resources: vec![super::super::ResourceSnapshot {
                    kind: 1,
                    base: 5,
                    length: 1,
                    flags: 0,
                }],
                address_spaces: Vec::new(),
                system_io: Vec::new(),
            });
            assert!(
                cache
                    .insert("\\DEV1".to_string(), available.clone())
                    .unwrap()
                    .is_none()
            );
            assert_eq!(cache.descriptor_count, 1);

            let duplicate = cache
                .insert("\\DEV0".to_string(), available)
                .unwrap()
                .unwrap();
            assert!(Arc::ptr_eq(&duplicate, &missing));
            assert_eq!(cache.descriptor_count, 1);
        }

        #[test]
        fn cache_descriptor_counter_rejects_capacity_overflow_before_insert() {
            let mut cache = CrsCache::default();
            cache.descriptor_count = MAX_CACHED_CRS_DESCRIPTORS;
            let available: CachedCrs = Arc::new(CachedCrsData::Available {
                resources: vec![super::super::ResourceSnapshot {
                    kind: 1,
                    base: 5,
                    length: 1,
                    flags: 0,
                }],
                address_spaces: Vec::new(),
                system_io: Vec::new(),
            });

            assert!(matches!(
                cache.insert("\\DEV0".to_string(), available),
                Err(LoadError::BudgetExceeded)
            ));
            assert!(!cache.entries.contains_key("\\DEV0"));
        }

        #[test]
        fn crs_heap_estimates_cover_payload_growth_and_overflow() {
            let small_parser = crs_parser_working_heap_bytes(0).unwrap();
            let large_parser = crs_parser_working_heap_bytes(4096).unwrap();
            assert!(large_parser > small_parser);
            assert_eq!(crs_parser_working_heap_bytes(usize::MAX), None);

            let small_cache = normalized_crs_cache_heap_bytes(1, 8).unwrap();
            let large_cache = normalized_crs_cache_heap_bytes(8, 64).unwrap();
            assert!(large_cache > small_cache);
            assert_eq!(
                normalized_crs_cache_heap_bytes(usize::MAX, usize::MAX),
                None
            );

            assert!(
                crs_cache_entry_heap_bytes(64).unwrap() > crs_cache_entry_heap_bytes(8).unwrap()
            );
            assert_eq!(crs_cache_entry_heap_bytes(usize::MAX), None);
        }

        #[test]
        fn normalized_crs_vectors_report_fallible_capacity_exhaustion() {
            assert!(matches!(
                try_crs_output_vectors(usize::MAX),
                Err(LoadError::BudgetExceeded)
            ));
        }
    }

    impl Drop for WorkUnitExecutionScope<'_> {
        fn drop(&mut self) {
            if self.owns_work_unit {
                if let Ok(mut state) = self.meter.state.lock() {
                    state.end_work_unit();
                }
                self.meter.native_access_budget.end_work_unit();
            }
        }
    }

    impl Drop for WorkingHeapReservation<'_> {
        fn drop(&mut self) {
            if let Ok(mut state) = self.meter.state.lock() {
                state.release_working_heap_for_generation(self.bytes, self.generation);
            }
        }
    }

    /// Private, single-interpreter ACPI execution domain.  The interpreter is
    /// retained after table loading so policy code can perform bounded,
    /// ordered entry evaluation instead of reducing initialization to a log
    /// message and dropping the namespace.
    pub struct AcpiExecutor {
        interpreter: Interpreter<OfflineHandler>,
        summary: NamespaceSummary,
        identity: FirmwareIdentity,
        compatibility_profile_hash: u64,
        hardware_side_effect_profile: HardwareSideEffectProfile,
        active_hardware_side_effect_profile: Arc<AtomicU8>,
        meter: AmlArc<NaosExecutionMeter>,
        native_access: Arc<AtomicBool>,
        native_access_authorized: Arc<AtomicBool>,
        raw_handler_access: Arc<AtomicBool>,
        pic_reentry: Arc<AtomicBool>,
        pic_attempted: AtomicBool,
        pic_evaluated: AtomicBool,
        pic_degraded: AtomicBool,
        region_activation_attempted: AtomicBool,
        system_io_preflight_attempted: AtomicBool,
        native_regions_active: AtomicBool,
        pic_mode: u8,
        region_registration_degraded: Arc<AtomicBool>,
        crs_cache: Mutex<CrsCache>,
    }

    impl NaosExecutionMeter {
        fn new(
            state: ExecutionState,
            clock: OfflineHandler,
            native_access_budget: Arc<NativeAccessBudget>,
            allocator_heap_bytes: u64,
        ) -> Result<Self, LoadError> {
            #[cfg(target_os = "naos")]
            let heap_quota = naos_runtime::HeapQuota::new(
                usize::try_from(allocator_heap_bytes).map_err(|_| LoadError::BudgetExceeded)?,
            );
            #[cfg(not(target_os = "naos"))]
            let _ = allocator_heap_bytes;

            Ok(Self {
                state: Mutex::new(state),
                clock,
                native_access_budget,
                shared_work_unit_generation: AtomicU64::new(0),
                #[cfg(target_os = "naos")]
                heap_quota,
            })
        }

        fn map_error(error: LoadError) -> AmlError {
            match error {
                LoadError::BudgetExceeded => AmlError::BudgetExceeded,
                LoadError::DeadlineExceeded => AmlError::DeadlineExceeded,
                other => AmlError::HostError(format!("AML_BUDGET_EXHAUSTED: {other:?}")),
            }
        }

        fn opcode_fuel_remaining(&self) -> u64 {
            self.state
                .lock()
                .map(|state| state.opcode_fuel_remaining())
                .unwrap_or(0)
        }

        fn deadline_failed(&self) -> bool {
            self.state
                .lock()
                .map(|state| state.deadline_failed())
                .unwrap_or(true)
        }

        fn begin_shared_work_unit(meter: &AmlArc<Self>) -> Result<SciWorkUnit, LoadError> {
            if meter.shared_work_unit_generation.load(Ordering::Acquire) != 0 {
                return Err(LoadError::InvalidProfile);
            }
            let now_ns = meter.clock.nanos_since_boot();
            let generation = {
                let mut state = meter.state.lock().map_err(|_| LoadError::AmlRejected)?;
                state.begin_work_unit_at(now_ns)?;
                state.work_unit_generation
            };
            if meter.native_access_budget.begin_work_unit().is_err() {
                if let Ok(mut state) = meter.state.lock() {
                    state.end_work_unit();
                }
                return Err(LoadError::BudgetExceeded);
            }
            if meter
                .shared_work_unit_generation
                .compare_exchange(0, generation, Ordering::AcqRel, Ordering::Acquire)
                .is_err()
            {
                if let Ok(mut state) = meter.state.lock() {
                    state.end_work_unit();
                }
                meter.native_access_budget.end_work_unit();
                return Err(LoadError::InvalidProfile);
            }
            Ok(SciWorkUnit {
                meter: AmlArc::clone(meter),
                generation,
                active: true,
            })
        }

        fn end_shared_work_unit(&self, generation: u64) -> Result<(), LoadError> {
            let now_ns = self.clock.nanos_since_boot();
            let mut state = match self.state.lock() {
                Ok(state) => state,
                Err(_) => {
                    let _ = self.shared_work_unit_generation.compare_exchange(
                        generation,
                        0,
                        Ordering::AcqRel,
                        Ordering::Acquire,
                    );
                    self.native_access_budget.end_work_unit();
                    return Err(LoadError::AmlRejected);
                }
            };
            if !state.operation_active || state.work_unit_generation != generation {
                return Err(LoadError::InvalidProfile);
            }
            if self
                .shared_work_unit_generation
                .compare_exchange(generation, 0, Ordering::AcqRel, Ordering::Acquire)
                .is_err()
            {
                return Err(LoadError::InvalidProfile);
            }
            let deadline_result = state.check_deadline(now_ns);
            state.end_work_unit();
            drop(state);
            self.native_access_budget.end_work_unit();
            deadline_result
        }

        fn begin_work_unit(&self) -> Result<WorkUnitExecutionScope<'_>, LoadError> {
            let now_ns = self.clock.nanos_since_boot();
            let shared_generation = self.shared_work_unit_generation.load(Ordering::Acquire);
            let owns_work_unit = {
                let mut state = self.state.lock().map_err(|_| LoadError::AmlRejected)?;
                if shared_generation != 0 {
                    if !state.operation_active || state.work_unit_generation != shared_generation {
                        return Err(LoadError::InvalidProfile);
                    }
                    state.check_deadline(now_ns)?;
                    false
                } else {
                    state.begin_work_unit_at(now_ns)?;
                    true
                }
            };
            if owns_work_unit && self.native_access_budget.begin_work_unit().is_err() {
                if let Ok(mut state) = self.state.lock() {
                    state.end_work_unit();
                }
                return Err(LoadError::BudgetExceeded);
            }
            #[cfg(target_os = "naos")]
            let heap_quota_scope = match naos_runtime::HeapQuotaScope::enter(&self.heap_quota) {
                Ok(scope) => scope,
                Err(_) => {
                    if owns_work_unit {
                        if let Ok(mut state) = self.state.lock() {
                            state.end_work_unit();
                        }
                        self.native_access_budget.end_work_unit();
                    }
                    return Err(LoadError::AmlRejected);
                }
            };
            Ok(WorkUnitExecutionScope {
                meter: self,
                owns_work_unit,
                #[cfg(target_os = "naos")]
                _heap_quota_scope: heap_quota_scope,
            })
        }

        fn reserve_working_heap(
            &self,
            bytes: u64,
        ) -> Result<WorkingHeapReservation<'_>, LoadError> {
            let mut state = self.state.lock().map_err(|_| LoadError::AmlRejected)?;
            state.reserve_working_heap(bytes)?;
            Ok(WorkingHeapReservation {
                meter: self,
                bytes,
                generation: state.work_unit_generation,
            })
        }

        fn check_work_unit_deadline(&self) -> Result<(), LoadError> {
            let now_ns = self.clock.nanos_since_boot();
            self.state
                .lock()
                .map_err(|_| LoadError::AmlRejected)?
                .check_deadline(now_ns)
        }
    }

    impl ExecutionMeter for NaosExecutionMeter {
        fn charge_opcode(&self) -> Result<(), AmlError> {
            let deadline_due = {
                let mut state = self
                    .state
                    .lock()
                    .map_err(|_| AmlError::InternalError("execution meter poisoned".to_string()))?;
                if state.deadline_failed() {
                    return Err(Self::map_error(LoadError::DeadlineExceeded));
                }
                state.charge_opcode(1).map_err(Self::map_error)?;
                state.deadline_check_due()
            };
            if !deadline_due {
                return Ok(());
            }
            let now_ns = self.clock.nanos_since_boot();
            self.state
                .lock()
                .map_err(|_| AmlError::InternalError("execution meter poisoned".to_string()))?
                .check_deadline(now_ns)
                .map_err(Self::map_error)
        }

        fn enter_method(&self) -> Result<(), AmlError> {
            let (needs_deadline, is_top_level) = {
                let state = self
                    .state
                    .lock()
                    .map_err(|_| AmlError::InternalError("execution meter poisoned".to_string()))?;
                (state.needs_method_deadline(), state.is_top_level())
            };
            if is_top_level {
                self.native_access_budget.reset();
            }
            let now_ns = if needs_deadline {
                self.clock.nanos_since_boot()
            } else {
                0
            };
            self.state
                .lock()
                .map_err(|_| AmlError::InternalError("execution meter poisoned".to_string()))?
                .enter_call_at(now_ns)
                .map_err(Self::map_error)
        }

        fn leave_method(&self) {
            if let Ok(mut state) = self.state.lock() {
                state.leave_call();
            }
        }

        fn loop_iteration(&self) -> Result<(), AmlError> {
            let deadline_due = {
                let mut state = self
                    .state
                    .lock()
                    .map_err(|_| AmlError::InternalError("execution meter poisoned".to_string()))?;
                if state.deadline_failed() {
                    return Err(Self::map_error(LoadError::DeadlineExceeded));
                }
                state.loop_iteration().map_err(Self::map_error)?;
                state.deadline_check_due()
            };
            if !deadline_due {
                return Ok(());
            }
            let now_ns = self.clock.nanos_since_boot();
            self.state
                .lock()
                .map_err(|_| AmlError::InternalError("execution meter poisoned".to_string()))?
                .check_deadline(now_ns)
                .map_err(Self::map_error)
        }

        fn check_deadline(&self) -> Result<(), AmlError> {
            self.check_work_unit_deadline().map_err(Self::map_error)
        }

        fn remaining_deadline_ns(&self) -> Result<Option<u64>, AmlError> {
            let now_ns = self.clock.nanos_since_boot();
            self.state
                .lock()
                .map_err(|_| AmlError::InternalError("execution meter poisoned".to_string()))?
                .remaining_deadline_ns(now_ns)
                .map_err(Self::map_error)
        }

        fn reserve_package(&self, bytes: u64) -> Result<(), AmlError> {
            self.state
                .lock()
                .map_err(|_| AmlError::InternalError("execution meter poisoned".to_string()))?
                .reserve_package(bytes)
                .map_err(Self::map_error)
        }

        fn reserve_buffer(&self, bytes: u64) -> Result<(), AmlError> {
            self.state
                .lock()
                .map_err(|_| AmlError::InternalError("execution meter poisoned".to_string()))?
                .reserve_buffer(bytes)
                .map_err(Self::map_error)
        }

        fn rollback_buffer_reservation(&self, bytes: u64) {
            if let Ok(mut state) = self.state.lock() {
                state.rollback_buffer_reservation(bytes);
            }
        }

        fn reserve_heap(&self, bytes: u64) -> Result<(), AmlError> {
            self.state
                .lock()
                .map_err(|_| AmlError::InternalError("execution meter poisoned".to_string()))?
                .reserve_heap(bytes)
                .map_err(Self::map_error)
        }

        fn reserve_working_heap(&self, bytes: u64) -> Result<(), AmlError> {
            self.state
                .lock()
                .map_err(|_| AmlError::InternalError("execution meter poisoned".to_string()))?
                .reserve_working_heap(bytes)
                .map_err(Self::map_error)
        }

        fn reserve_working_heap_scoped(&self, bytes: u64) -> Result<u64, AmlError> {
            let mut state = self
                .state
                .lock()
                .map_err(|_| AmlError::InternalError("execution meter poisoned".to_string()))?;
            state.reserve_working_heap(bytes).map_err(Self::map_error)?;
            Ok(state.work_unit_generation)
        }

        fn release_working_heap(&self, bytes: u64) {
            if let Ok(mut state) = self.state.lock() {
                state.release_working_heap(bytes);
            }
        }

        fn release_working_heap_scoped(&self, bytes: u64, generation: u64) {
            if let Ok(mut state) = self.state.lock() {
                state.release_working_heap_for_generation(bytes, generation);
            }
        }

        fn commit_working_heap(&self, bytes: u64) -> Result<(), AmlError> {
            self.state
                .lock()
                .map_err(|_| AmlError::InternalError("execution meter poisoned".to_string()))?
                .commit_working_heap(bytes)
                .map_err(Self::map_error)
        }

        fn commit_working_heap_scoped(&self, bytes: u64, generation: u64) -> Result<(), AmlError> {
            self.state
                .lock()
                .map_err(|_| AmlError::InternalError("execution meter poisoned".to_string()))?
                .commit_working_heap_for_generation(bytes, generation)
                .map_err(Self::map_error)
        }

        fn reserve_working_allocation_scoped(
            &self,
            heap_bytes: u64,
            package_bytes: u64,
            buffer_bytes: u64,
        ) -> Result<u64, AmlError> {
            let mut state = self
                .state
                .lock()
                .map_err(|_| AmlError::InternalError("execution meter poisoned".to_string()))?;
            state
                .reserve_working_allocation(heap_bytes, package_bytes, buffer_bytes)
                .map_err(Self::map_error)?;
            Ok(state.work_unit_generation)
        }

        fn release_working_allocation_scoped(
            &self,
            heap_bytes: u64,
            package_bytes: u64,
            buffer_bytes: u64,
            generation: u64,
        ) {
            if let Ok(mut state) = self.state.lock() {
                state.release_working_allocation_for_generation(
                    heap_bytes,
                    package_bytes,
                    buffer_bytes,
                    generation,
                );
            }
        }

        fn commit_working_allocation_scoped(
            &self,
            heap_bytes: u64,
            package_bytes: u64,
            buffer_bytes: u64,
            generation: u64,
        ) -> Result<(), AmlError> {
            self.state
                .lock()
                .map_err(|_| AmlError::InternalError("execution meter poisoned".to_string()))?
                .commit_working_allocation_for_generation(
                    heap_bytes,
                    package_bytes,
                    buffer_bytes,
                    generation,
                )
                .map_err(Self::map_error)
        }

        fn release_persistent_allocation(
            &self,
            heap_bytes: u64,
            package_bytes: u64,
            buffer_bytes: u64,
        ) -> Result<(), AmlError> {
            self.state
                .lock()
                .map_err(|_| AmlError::InternalError("execution meter poisoned".to_string()))?
                .release_persistent_allocation(heap_bytes, package_bytes, buffer_bytes)
                .map_err(Self::map_error)
        }

        fn release_heap(&self, bytes: u64) -> Result<(), AmlError> {
            self.state
                .lock()
                .map_err(|_| AmlError::InternalError("execution meter poisoned".to_string()))?
                .release_heap(bytes)
                .map_err(Self::map_error)
        }

        fn reserve_namespace_object(&self, heap_bytes: u64) -> Result<(), AmlError> {
            self.state
                .lock()
                .map_err(|_| AmlError::InternalError("execution meter poisoned".to_string()))?
                .reserve_namespace_object(heap_bytes)
                .map_err(Self::map_error)
        }

        fn release_namespace_object(&self, heap_bytes: u64) {
            if let Ok(mut state) = self.state.lock() {
                let _ = state.release_namespace_object(heap_bytes);
            }
        }
    }

    impl AcpiExecutor {
        fn charge_loop_iteration(&self) -> Result<(), LoadError> {
            self.meter
                .loop_iteration()
                .map_err(|error| Self::map_osc_error(&error))
        }

        pub(super) fn sort_namespace_paths_metered<S: AsRef<str>>(
            &self,
            paths: &mut [S],
        ) -> Result<(), LoadError> {
            fn compare_paths(
                executor: &AcpiExecutor,
                left: &str,
                right: &str,
                comparisons: &mut usize,
            ) -> Result<core::cmp::Ordering, LoadError> {
                executor.charge_loop_iteration()?;
                *comparisons = comparisons
                    .checked_add(1)
                    .ok_or(LoadError::BudgetExceeded)?;
                if *comparisons & 0x3f == 0 {
                    executor.meter.check_work_unit_deadline()?;
                }
                Ok(left.cmp(right))
            }

            fn sift_down<S: AsRef<str>>(
                paths: &mut [S],
                mut root: usize,
                end: usize,
                executor: &AcpiExecutor,
                comparisons: &mut usize,
            ) -> Result<(), LoadError> {
                loop {
                    let Some(left) = root.checked_mul(2).and_then(|value| value.checked_add(1))
                    else {
                        return Err(LoadError::BudgetExceeded);
                    };
                    if left >= end {
                        return Ok(());
                    }
                    let right = left + 1;
                    let mut larger_child = left;
                    if right < end
                        && compare_paths(
                            executor,
                            paths[left].as_ref(),
                            paths[right].as_ref(),
                            comparisons,
                        )? == core::cmp::Ordering::Less
                    {
                        larger_child = right;
                    }
                    if compare_paths(
                        executor,
                        paths[root].as_ref(),
                        paths[larger_child].as_ref(),
                        comparisons,
                    )? != core::cmp::Ordering::Less
                    {
                        return Ok(());
                    }
                    paths.swap(root, larger_child);
                    root = larger_child;
                }
            }

            let mut comparisons = 0;
            let path_count = paths.len();
            for root in (0..path_count / 2).rev() {
                sift_down(paths, root, path_count, self, &mut comparisons)?;
            }
            for end in (1..path_count).rev() {
                paths.swap(0, end);
                sift_down(paths, 0, end, self, &mut comparisons)?;
            }
            self.meter.check_work_unit_deadline()
        }

        fn ensure_aml_execution_ready(&self) -> Result<(), LoadError> {
            if !self.native_regions_active.load(Ordering::Acquire)
                || !self.pic_attempted.load(Ordering::Acquire)
                || self.pic_degraded.load(Ordering::Acquire)
                || self.pic_reentry.load(Ordering::Acquire)
            {
                return Err(LoadError::InvalidProfile);
            }
            Ok(())
        }

        fn is_budget_error(error: &AmlError) -> bool {
            aml_error_contains_marker(error, "AML_BUDGET_EXHAUSTED")
                || aml_error_contains_marker(error, "AML_SNAPSHOT_SIZE_LIMIT")
                || aml_error_contains_marker(error, "AML_SNAPSHOT_SIZE_OVERFLOW")
        }

        fn is_deadline_error(error: &AmlError) -> bool {
            aml_error_contains_marker(error, "AML_DEADLINE_EXCEEDED")
        }

        fn is_global_lock_error(error: &AmlError) -> bool {
            matches!(
                error,
                AmlError::MutexAcquireTimeout | AmlError::GlobalLockOutcomeUnknown
            ) || aml_error_contains_marker(error, "NOT_SUPPORTED_GLOBAL_LOCK")
        }

        fn is_global_lock_outcome_unknown(error: &AmlError) -> bool {
            matches!(error, AmlError::GlobalLockOutcomeUnknown)
        }

        fn is_native_region_outcome_unknown(error: &AmlError) -> bool {
            matches!(error, AmlError::NativeRegionOutcomeUnknown)
        }

        fn map_osc_error(error: &AmlError) -> LoadError {
            if Self::is_budget_error(error) {
                LoadError::BudgetExceeded
            } else if Self::is_deadline_error(error) {
                LoadError::DeadlineExceeded
            } else if Self::is_global_lock_outcome_unknown(error) {
                LoadError::GlobalLockOutcomeUnknown
            } else if Self::is_native_region_outcome_unknown(error) {
                LoadError::NativeRegionOutcomeUnknown
            } else if matches!(error, AmlError::MutexAcquireTimeout) {
                LoadError::GlobalLockTimeout
            } else if Self::is_global_lock_error(error) {
                LoadError::GlobalLockUnsupported
            } else if aml_error_contains_marker(error, "rollback/token invalidation failed") {
                LoadError::CleanupFailed
            } else {
                LoadError::AmlRejected
            }
        }

        fn evaluate_pci_osc(
            &self,
            path: AmlName,
            query: bool,
            support: u32,
            control: u32,
        ) -> Result<PciOscResponse, LoadError> {
            let query_enable = if query { OSC_QUERY_ENABLE } else { 0 };
            let mut uuid = aml_allocator::vec_new();
            uuid.try_reserve_exact(PCI_OSC_UUID.len())
                .map_err(|_| LoadError::BudgetExceeded)?;
            uuid.extend_from_slice(&PCI_OSC_UUID);
            let mut capabilities = aml_allocator::vec_new();
            capabilities
                .try_reserve_exact(3 * core::mem::size_of::<u32>())
                .map_err(|_| LoadError::BudgetExceeded)?;
            capabilities.extend_from_slice(&query_enable.to_le_bytes());
            capabilities.extend_from_slice(&support.to_le_bytes());
            capabilities.extend_from_slice(&control.to_le_bytes());
            let mut args = aml_allocator::vec_with_capacity(4);
            args.push(Object::Buffer(uuid).wrap());
            args.push(Object::Integer(PCI_OSC_REVISION).wrap());
            args.push(Object::Integer(PCI_OSC_ARGUMENT_COUNT).wrap());
            args.push(Object::Buffer(capabilities).wrap());
            let value = self
                .interpreter
                .evaluate_if_present(path, args)
                .map_err(|error| Self::map_osc_error(&error))?
                .ok_or(LoadError::OscUnavailable)?;
            let Object::Buffer(response) = &*value else {
                return Err(LoadError::OscInvalidResponse);
            };
            parse_pci_osc_response(
                if query {
                    PciOscPhase::Query
                } else {
                    PciOscPhase::Commit
                },
                control,
                response,
            )
        }

        fn gpe_method_present(namespace: &Namespace, path: &AmlName) -> Result<bool, LoadError> {
            match namespace.get_absolute_if_present(path) {
                Ok(Some(object)) if object.typ() == ObjectType::Method => Ok(true),
                Ok(Some(_)) => Err(LoadError::AmlRejected),
                Ok(None) => Ok(false),
                Err(error) => Err(Self::map_osc_error(&error)),
            }
        }

        pub(super) fn gpe_path_working_heap_bytes() -> Option<u64> {
            let component_count = 3_usize;
            let path_bytes = core::mem::size_of::<Vec<NameComponent>>()
                .checked_add(component_count.checked_mul(core::mem::size_of::<NameComponent>())?)?;
            let bytes = path_bytes.checked_mul(6)?.checked_add(512)?;
            u64::try_from(bytes).ok()
        }

        fn gpe_method_paths(gpe_number: u32) -> Result<(AmlName, AmlName), LoadError> {
            const HEX: &[u8; 16] = b"0123456789ABCDEF";
            let high = HEX[((gpe_number >> 4) & 0xf) as usize];
            let low = HEX[(gpe_number & 0xf) as usize];
            let gpe = NameSeg::from_bytes(*b"_GPE").map_err(|_| LoadError::InvalidGpeNumber)?;
            let level = NameSeg::from_bytes([b'_', b'L', high, low])
                .map_err(|_| LoadError::InvalidGpeNumber)?;
            let edge = NameSeg::from_bytes([b'_', b'E', high, low])
                .map_err(|_| LoadError::InvalidGpeNumber)?;
            let level_path = AmlName::try_from_absolute_segments(&[gpe, level])
                .map_err(|error| Self::map_osc_error(&error))?;
            let edge_path = AmlName::try_from_absolute_segments(&[gpe, edge])
                .map_err(|error| Self::map_osc_error(&error))?;
            Ok((level_path, edge_path))
        }

        fn find_gpe_method(
            &self,
            gpe_number: u32,
        ) -> Result<Option<(AmlName, GpeHandlerKind)>, LoadError> {
            let (level_path, edge_path) = Self::gpe_method_paths(gpe_number)?;
            let namespace = self.interpreter.namespace.lock();
            let level_present = Self::gpe_method_present(&namespace, &level_path)?;
            let edge_present = Self::gpe_method_present(&namespace, &edge_path)?;

            match (level_present, edge_present) {
                (false, false) => Ok(None),
                (true, false) => Ok(Some((level_path, GpeHandlerKind::Level))),
                (false, true) => Ok(Some((edge_path, GpeHandlerKind::Edge))),
                (true, true) => Err(LoadError::AmlRejected),
            }
        }

        /// Start one aggregate SCI work unit. Executor calls made until the
        /// returned guard is finished join this budget instead of replenishing
        /// per-work-unit fuel, loop, native-access, or deadline limits.
        pub fn begin_sci_work_unit(&self) -> Result<SciWorkUnit, LoadError> {
            self.ensure_aml_execution_ready()?;
            NaosExecutionMeter::begin_shared_work_unit(&self.meter)
        }

        /// Resolve only the standard `\\_GPE._Lxx` or `\\_GPE._Exx` handler
        /// for one bounded GPE number without executing AML. Both candidate
        /// names are resolved before returning a kind; ambiguity is rejected.
        pub fn resolve_gpe_handler(&self, gpe_number: u32) -> Result<GpeDispatchResult, LoadError> {
            if gpe_number > u32::from(u8::MAX) {
                return Err(LoadError::InvalidGpeNumber);
            }
            self.ensure_aml_execution_ready()?;
            let _budget_scope = self.meter.begin_work_unit()?;
            let scratch_bytes =
                Self::gpe_path_working_heap_bytes().ok_or(LoadError::BudgetExceeded)?;
            let _path_scratch = self.meter.reserve_working_heap(scratch_bytes)?;
            let result = match self.find_gpe_method(gpe_number)? {
                Some((_, kind)) => GpeDispatchResult::Handled(kind),
                None => GpeDispatchResult::NoHandler,
            };
            self.meter.check_work_unit_deadline()?;
            Ok(result)
        }

        /// Execute a previously resolved GPE handler only if the namespace
        /// still contains exactly that handler kind. Separating resolution
        /// lets the platform clear edge-triggered status before AML runs and
        /// clear level-triggered status after AML completes.
        pub fn execute_gpe_handler(
            &self,
            gpe_number: u32,
            expected_kind: GpeHandlerKind,
        ) -> Result<(), LoadError> {
            if gpe_number > u32::from(u8::MAX) {
                return Err(LoadError::InvalidGpeNumber);
            }
            self.ensure_aml_execution_ready()?;
            let _budget_scope = self.meter.begin_work_unit()?;
            let scratch_bytes =
                Self::gpe_path_working_heap_bytes().ok_or(LoadError::BudgetExceeded)?;
            let _path_scratch = self.meter.reserve_working_heap(scratch_bytes)?;
            let (path, kind) = self
                .find_gpe_method(gpe_number)?
                .ok_or(LoadError::AmlRejected)?;
            if kind != expected_kind {
                return Err(LoadError::AmlRejected);
            }

            self.interpreter
                .evaluate(path, aml_allocator::vec_new())
                .map_err(|error| Self::map_osc_error(&error))?;
            self.meter.check_work_unit_deadline()?;
            Ok(())
        }

        /// Resolve and execute a GPE handler in one executor call. Platforms
        /// that need type-specific EOI ordering use the split methods above.
        pub fn dispatch_gpe(&self, gpe_number: u32) -> Result<GpeDispatchResult, LoadError> {
            if gpe_number > u32::from(u8::MAX) {
                return Err(LoadError::InvalidGpeNumber);
            }
            self.ensure_aml_execution_ready()?;
            let _budget_scope = self.meter.begin_work_unit()?;
            let scratch_bytes =
                Self::gpe_path_working_heap_bytes().ok_or(LoadError::BudgetExceeded)?;
            let _path_scratch = self.meter.reserve_working_heap(scratch_bytes)?;
            let Some((path, kind)) = self.find_gpe_method(gpe_number)? else {
                self.meter.check_work_unit_deadline()?;
                return Ok(GpeDispatchResult::NoHandler);
            };
            self.interpreter
                .evaluate(path, aml_allocator::vec_new())
                .map_err(|error| Self::map_osc_error(&error))?;
            self.meter.check_work_unit_deadline()?;
            Ok(GpeDispatchResult::Handled(kind))
        }

        /// Negotiate the PCI root's ownership through its typed `_OSC`
        /// method. This is deliberately narrower than arbitrary AML
        /// evaluation: it accepts only an absolute root path, performs a
        /// bounded query sequence that only narrows masked capabilities, then
        /// commits exactly the final query mask in this executor's ordering
        /// domain. Denied control bits remain denied in the returned feature
        /// set.
        pub fn negotiate_pci_features(
            &self,
            root_path: &str,
            requested_support: u32,
            requested_control: u32,
        ) -> Result<PciOscNegotiation, LoadError> {
            self.ensure_aml_execution_ready()?;
            validate_pci_osc_request(requested_support, requested_control)?;
            if root_path.as_bytes().len() > 255
                || root_path.as_bytes().contains(&0)
                || !root_path.starts_with('\\')
            {
                return Err(LoadError::InvalidRootPath);
            }
            let _budget_scope = self.meter.begin_work_unit()?;
            let scratch_heap_bytes =
                pci_osc_working_heap_bytes(root_path.len()).ok_or(LoadError::BudgetExceeded)?;
            let _osc_scratch_heap = self.meter.reserve_working_heap(scratch_heap_bytes)?;
            let root = AmlName::from_str(root_path).map_err(|_| LoadError::InvalidRootPath)?;
            let osc = AmlName::from_str("_OSC")
                .map_err(|_| LoadError::InvalidRootPath)?
                .resolve(&root)
                .map_err(|_| LoadError::InvalidRootPath)?;

            let exchange = negotiate_pci_osc_exchange(
                requested_support,
                requested_control,
                |query, support, control| {
                    let path = osc
                        .try_clone()
                        .map_err(|error| Self::map_osc_error(&error))?;
                    self.evaluate_pci_osc(path, query, support, control)
                },
            )?;
            self.meter.check_work_unit_deadline()?;
            Ok(PciOscNegotiation {
                firmware_version: self.identity.firmware_version.clone(),
                aml_namespace_generation: self.summary.generation,
                compatibility_profile_hash: self.compatibility_profile_hash,
                requested_support,
                requested_control,
                query_errors: exchange.query_errors,
                query_support: exchange.query_support,
                query_granted_control: exchange.query_granted_control,
                masked_control: exchange.masked_control,
                commit_errors: exchange.commit_errors,
                commit_support: exchange.commit_support,
                granted_control: exchange.granted_control,
                denied_control: exchange.denied_control,
            })
        }

        fn evaluate_root_object(
            &self,
            scope: &AmlName,
            name: &str,
        ) -> Result<Option<WrappedObject>, LoadError> {
            let path = AmlName::from_str(name)
                .map_err(|_| LoadError::InvalidPciRoot)?
                .resolve(scope)
                .map_err(|_| LoadError::InvalidPciRoot)?;
            self.interpreter
                .evaluate_if_present(path, acpi::aml_vec![])
                .map_err(|error| Self::map_osc_error(&error))
        }

        pub(super) fn normalize_crs_with_budget(
            parent_device_path: &str,
            descriptors: AmlVec<acpi::aml::resource::Resource>,
            mut charge_iteration: impl FnMut() -> Result<(), LoadError>,
            mut check_deadline: impl FnMut() -> Result<(), LoadError>,
        ) -> Result<CachedCrs, LoadError> {
            let (mut resources, mut address_spaces, mut system_io) =
                try_crs_output_vectors(descriptors.len())?;
            let mut normalized_descriptors = 0_usize;
            for descriptor in descriptors {
                charge_iteration()?;
                normalized_descriptors = normalized_descriptors
                    .checked_add(1)
                    .ok_or(LoadError::BudgetExceeded)?;
                if normalized_descriptors & 0x3f == 0 {
                    check_deadline()?;
                }
                let snapshot = match descriptor {
                    acpi::aml::resource::Resource::Irq(value) => ResourceSnapshot {
                        kind: 1,
                        base: value.irqs.first().copied().unwrap_or(0) as u64,
                        length: value.irqs.len() as u64,
                        flags: 0,
                    },
                    acpi::aml::resource::Resource::AddressSpace(value) => {
                        let resource_type = match value.resource_type {
                            acpi::aml::resource::AddressSpaceResourceType::MemoryRange => 0,
                            acpi::aml::resource::AddressSpaceResourceType::IORange => 1,
                            acpi::aml::resource::AddressSpaceResourceType::BusNumberRange => 2,
                            acpi::aml::resource::AddressSpaceResourceType::VendorDefined(value) => {
                                value
                            }
                        };
                        address_spaces.push(PciRootResourceWindow {
                            resource_type,
                            descriptor_revision: u32::from(value.revision_id.unwrap_or(0)),
                            general_flags: u32::from(value.general_flags),
                            type_specific_flags: u32::from(value.type_specific_flags),
                            granularity: value.granularity,
                            minimum: value.address_range.0,
                            maximum: value.address_range.1,
                            translation_offset: value.translation_offset,
                            length: value.length,
                            type_specific_attributes: value.type_specific_attributes.unwrap_or(0),
                        });
                        if value.resource_type
                            == acpi::aml::resource::AddressSpaceResourceType::IORange
                            && let Some(resource) =
                                crate::region::try_system_io_address_space_resource_from_crs(
                                    parent_device_path,
                                    value.address_range.0,
                                    value.address_range.1,
                                    value.translation_offset,
                                    value.length,
                                    value.revision_id == Some(1) && value.general_flags & 1 != 0,
                                    value.is_minimum_address_fixed
                                        && value.is_maximum_address_fixed,
                                    value.type_specific_flags,
                                )
                                .map_err(|error| match error {
                                    crate::region::RegionError::HeapBudgetExceeded
                                    | crate::region::RegionError::BudgetExceeded => {
                                        LoadError::BudgetExceeded
                                    }
                                    _ => LoadError::InvalidPciRoot,
                                })?
                        {
                            system_io.push(resource);
                        }
                        ResourceSnapshot {
                            kind: 2,
                            base: value.address_range.0,
                            length: value.length,
                            flags: 0,
                        }
                    }
                    acpi::aml::resource::Resource::MemoryRange(value) => match value {
                        acpi::aml::resource::MemoryRangeDescriptor::FixedLocation {
                            is_writable,
                            base_address,
                            range_length,
                        } => ResourceSnapshot {
                            kind: 3,
                            base: base_address as u64,
                            length: range_length as u64,
                            flags: is_writable as u64,
                        },
                    },
                    acpi::aml::resource::Resource::IOPort(value) => {
                        if let Some(resource) = crate::region::try_system_io_resource_from_crs(
                            parent_device_path,
                            u64::from(value.memory_range.0),
                            u64::from(value.memory_range.1),
                            0,
                            u64::from(value.range_length),
                            true,
                        )
                        .map_err(|error| match error {
                            crate::region::RegionError::HeapBudgetExceeded
                            | crate::region::RegionError::BudgetExceeded => {
                                LoadError::BudgetExceeded
                            }
                            _ => LoadError::InvalidPciRoot,
                        })? {
                            system_io.push(resource);
                        }
                        ResourceSnapshot {
                            kind: 4,
                            base: value.memory_range.0 as u64,
                            length: value.range_length as u64,
                            flags: value.decodes_full_address as u64,
                        }
                    }
                    acpi::aml::resource::Resource::Dma(value) => ResourceSnapshot {
                        kind: 5,
                        base: value.channel_mask as u64,
                        length: 0,
                        flags: value.is_bus_master as u64,
                    },
                    acpi::aml::resource::Resource::SerialBus(_) => ResourceSnapshot {
                        kind: 6,
                        base: 0,
                        length: 0,
                        flags: 0,
                    },
                    acpi::aml::resource::Resource::GPIO(_) => ResourceSnapshot {
                        kind: 7,
                        base: 0,
                        length: 0,
                        flags: 0,
                    },
                };
                resources.push(snapshot);
            }
            check_deadline()?;
            Ok(Arc::new(CachedCrsData::Available {
                resources,
                address_spaces,
                system_io,
            }))
        }

        fn cached_crs(&self, path: &str) -> Result<Option<CachedCrs>, LoadError> {
            self.crs_cache
                .lock()
                .map(|cache| cache.get(path))
                .map_err(|_| LoadError::AmlRejected)
        }

        fn invalidate_crs_cache(&self) -> Result<(), LoadError> {
            let persistent_heap_bytes = self
                .crs_cache
                .lock()
                .map_err(|_| LoadError::AmlRejected)?
                .clear();
            self.meter
                .release_heap(persistent_heap_bytes)
                .map_err(|error| Self::map_osc_error(&error))
        }

        fn reserve_cached_crs_descriptors(&self, additional: usize) -> Result<(), LoadError> {
            let cache = self.crs_cache.lock().map_err(|_| LoadError::AmlRejected)?;
            cache.reserve_descriptors(additional)
        }

        fn evaluate_crs(&self, parent_device_path: &str) -> Result<CachedCrs, LoadError> {
            if let Some(cached) = self.cached_crs(parent_device_path)? {
                return Ok(cached);
            }
            let cache_entry_heap_bytes = crs_cache_entry_heap_bytes(parent_device_path.len())
                .ok_or(LoadError::BudgetExceeded)?;
            let cache_entry_heap_reservation =
                self.meter.reserve_working_heap(cache_entry_heap_bytes)?;
            let mut parser_heap_reservation = None;
            let mut normalized_heap_reservation = None;
            let mut normalized_heap_bytes = 0_u64;
            let cached = match AmlName::from_str(parent_device_path) {
                Ok(parent) => {
                    match AmlName::from_str("_CRS").and_then(|name| name.resolve(&parent)) {
                        Ok(path) => {
                            match self.interpreter.evaluate_if_present(path, acpi::aml_vec![]) {
                                Ok(None) => Arc::new(CachedCrsData::Missing),
                                Ok(Some(value)) => {
                                    let input_bytes = match &*value {
                                        Object::Buffer(bytes) => bytes.len(),
                                        _ => 0,
                                    };
                                    let parser_heap_bytes =
                                        crs_parser_working_heap_bytes(input_bytes)
                                            .ok_or(LoadError::BudgetExceeded)?;
                                    parser_heap_reservation =
                                        Some(self.meter.reserve_working_heap(parser_heap_bytes)?);
                                    match acpi::aml::resource::resource_descriptor_list_metered(
                                        value,
                                        self.meter.as_ref(),
                                    ) {
                                        Ok(descriptors)
                                            if descriptors.len()
                                                <= MAX_SYSTEM_IO_DESCRIPTORS_PER_DEVICE =>
                                        {
                                            self.reserve_cached_crs_descriptors(descriptors.len())?;
                                            let normalized_cache_bytes =
                                                normalized_crs_cache_heap_bytes(
                                                    descriptors.len(),
                                                    parent_device_path.len(),
                                                )
                                                .ok_or(LoadError::BudgetExceeded)?;
                                            let reservation = self
                                                .meter
                                                .reserve_working_heap(normalized_cache_bytes)?;
                                            normalized_heap_bytes = normalized_cache_bytes;
                                            let normalized = Self::normalize_crs_with_budget(
                                                parent_device_path,
                                                descriptors,
                                                || self.charge_loop_iteration(),
                                                || self.meter.check_work_unit_deadline(),
                                            )?;
                                            normalized_heap_reservation = Some(reservation);
                                            normalized
                                        }
                                        Err(error) if Self::is_budget_error(&error) => {
                                            return Err(LoadError::BudgetExceeded);
                                        }
                                        Err(error) if Self::is_deadline_error(&error) => {
                                            return Err(LoadError::DeadlineExceeded);
                                        }
                                        _ => Arc::new(CachedCrsData::Invalid),
                                    }
                                }
                                Err(error) if Self::is_budget_error(&error) => {
                                    return Err(LoadError::BudgetExceeded);
                                }
                                Err(error) if Self::is_deadline_error(&error) => {
                                    return Err(LoadError::DeadlineExceeded);
                                }
                                Err(error) if Self::is_global_lock_outcome_unknown(&error) => {
                                    return Err(LoadError::GlobalLockOutcomeUnknown);
                                }
                                Err(error) if Self::is_native_region_outcome_unknown(&error) => {
                                    return Err(LoadError::NativeRegionOutcomeUnknown);
                                }
                                Err(error) if matches!(&error, AmlError::MutexAcquireTimeout) => {
                                    return Err(LoadError::GlobalLockTimeout);
                                }
                                Err(error) if Self::is_global_lock_error(&error) => {
                                    return Err(LoadError::GlobalLockUnsupported);
                                }
                                Err(_) => Arc::new(CachedCrsData::Invalid),
                            }
                        }
                        Err(_) => Arc::new(CachedCrsData::Invalid),
                    }
                }
                Err(_) => Arc::new(CachedCrsData::Invalid),
            };
            let cache_key = try_copy_bounded_string(parent_device_path, MAX_NAMESPACE_PATH_BYTES)?;
            let persistent_heap_bytes = cache_entry_heap_bytes
                .checked_add(normalized_heap_bytes)
                .ok_or(LoadError::BudgetExceeded)?;
            let insertion = self
                .crs_cache
                .lock()
                .map_err(|_| LoadError::AmlRejected)?
                .insert_metered(cache_key, Arc::clone(&cached), persistent_heap_bytes)?;
            if let Some(existing) = insertion {
                return Ok(existing);
            }
            drop(parser_heap_reservation);
            cache_entry_heap_reservation.commit_to_persistent()?;
            if let Some(reservation) = normalized_heap_reservation {
                reservation.commit_to_persistent()?;
            }
            Ok(cached)
        }

        fn evaluate_root_integer(
            &self,
            scope: &AmlName,
            name: &str,
        ) -> Result<Option<u64>, LoadError> {
            let Some(value) = self.evaluate_root_object(scope, name)? else {
                return Ok(None);
            };
            match &*value {
                Object::Integer(value) => Ok(Some(*value)),
                _ => Err(LoadError::InvalidPciRoot),
            }
        }

        fn root_bus_window(
            &self,
            scope: &AmlName,
        ) -> Result<(Option<(u8, u8)>, u32, Vec<PciRootResourceWindow>), LoadError> {
            let path = scope
                .try_to_string_bounded(255)
                .map_err(|error| Self::map_osc_error(&error))?
                .ok_or(LoadError::InvalidPciRoot)?;
            let cached = self.evaluate_crs(&path)?;
            let windows = match cached.as_ref() {
                CachedCrsData::Missing => return Ok((None, 0, Vec::new())),
                CachedCrsData::Invalid => return Err(LoadError::InvalidPciRoot),
                CachedCrsData::Available { address_spaces, .. } => {
                    try_copy_bounded_slice(address_spaces, MAX_PCI_ROOT_RESOURCE_WINDOWS)?
                }
            };
            if windows.len() > MAX_PCI_ROOT_RESOURCE_WINDOWS {
                return Err(LoadError::BudgetExceeded);
            }
            let mut checked_windows = 0_usize;
            let bus_window = pci_root_bus_window_with_budget(&windows, || {
                self.charge_loop_iteration()?;
                checked_windows = checked_windows
                    .checked_add(1)
                    .ok_or(LoadError::BudgetExceeded)?;
                if checked_windows & 0x3f == 0 {
                    self.meter.check_work_unit_deadline()?;
                }
                Ok(())
            })?;
            Ok((bus_window, windows.len() as u32, windows))
        }

        fn is_pci_root_hid(value: &Object) -> bool {
            match value {
                Object::String(value) => {
                    let value = value.trim_matches('\0');
                    value == "PNP0A03" || value == "PNP0A08"
                }
                Object::Integer(value) => {
                    let Ok(value) = u32::try_from(*value) else {
                        return false;
                    };
                    // ACPI integer EISA IDs use the little-endian packed form
                    // consumed by AML. Decode the compressed ID in host order.
                    let value = value.swap_bytes();
                    let first = ((value >> 26) & 0x1f) as u8;
                    let second = ((value >> 21) & 0x1f) as u8;
                    let third = ((value >> 16) & 0x1f) as u8;
                    first == b'P' - b'A' + 1
                        && second == b'N' - b'A' + 1
                        && third == b'P' - b'A' + 1
                        && matches!(value & 0xffff, 0x0a03 | 0x0a08)
                }
                Object::Reference { inner, .. } => Self::is_pci_root_hid(&*inner),
                _ => false,
            }
        }

        fn is_pci_root_cid(value: &Object) -> bool {
            match value {
                Object::Package(identifiers) => identifiers
                    .iter()
                    .any(|identifier| Self::is_pci_root_hid(&**identifier)),
                Object::Reference { inner, .. } => Self::is_pci_root_cid(&**inner),
                _ => Self::is_pci_root_hid(value),
            }
        }

        /// Collect namespace-static PCI root hints without evaluating `_CRS`
        /// or other AML methods. platformd uses these only to bind the root
        /// bus before final root discovery, so `_CRS` can safely perform its
        /// own read-only PCI_Config accesses.
        pub fn get_pci_config_root_candidates(
            &self,
        ) -> Result<Vec<PciRootBridgeDescriptor>, LoadError> {
            let _budget_scope = self.meter.begin_work_unit()?;
            fn integer(value: &Object) -> Option<u64> {
                match value {
                    Object::Integer(value) => Some(*value),
                    Object::Reference { inner, .. } => integer(inner),
                    _ => None,
                }
            }

            let hid_name = NameSeg::from_str("_HID").map_err(|_| LoadError::InvalidPciRoot)?;
            let cid_name = NameSeg::from_str("_CID").map_err(|_| LoadError::InvalidPciRoot)?;
            let segment_name = NameSeg::from_str("_SEG").map_err(|_| LoadError::InvalidPciRoot)?;
            let base_bus_name = NameSeg::from_str("_BBN").map_err(|_| LoadError::InvalidPciRoot)?;
            let root_output_bytes = core::mem::size_of::<PciRootBridgeDescriptor>()
                .checked_add(255 + 24)
                .and_then(|bytes| MAX_PCI_ROOTS.checked_mul(bytes))
                .and_then(|bytes| u64::try_from(bytes).ok())
                .ok_or(LoadError::BudgetExceeded)?;
            let _root_output_heap = self.meter.reserve_working_heap(root_output_bytes)?;
            let namespace = self.interpreter.namespace.lock();
            let mut roots = Vec::new();
            reserve_bounded_output(&mut roots, MAX_PCI_ROOTS, MAX_PCI_ROOTS)?;
            namespace
                .traverse_metered(self.meter.as_ref(), |path, level| {
                    if level.kind != NamespaceLevelKind::Device {
                        return Ok(true);
                    }
                    let get = |name: &NameSeg| level.values.get(name).map(|(_, object)| &**object);
                    let hid_match = get(&hid_name).is_some_and(Self::is_pci_root_hid);
                    let cid_match = get(&cid_name).is_some_and(Self::is_pci_root_cid);
                    if !hid_match && !cid_match {
                        return Ok(true);
                    }

                    let segment = get(&segment_name).and_then(integer).unwrap_or(0);
                    let base_bus = get(&base_bus_name).and_then(integer).unwrap_or(0);
                    let segment =
                        u16::try_from(segment).map_err(|_| AmlError::InvalidName(None))?;
                    let base_bus =
                        u8::try_from(base_bus).map_err(|_| AmlError::InvalidName(None))?;
                    let namespace_path = path
                        .try_to_string_bounded(255)?
                        .ok_or(AmlError::InvalidName(None))?;
                    if roots.len() == MAX_PCI_ROOTS {
                        return Err(AmlError::InvalidName(None));
                    }
                    let (root_id, device_id) =
                        pci_root_identity(self.summary.generation, &namespace_path);
                    roots.push(PciRootBridgeDescriptor {
                        root_id,
                        device_id,
                        namespace_path: try_copy_aml_string(&namespace_path, 255)
                            .map_err(|_| AmlError::BudgetExceeded)?,
                        segment,
                        start_bus: base_bus,
                        end_bus: base_bus,
                        aml_namespace_generation: self.summary.generation,
                        resource_windows: Vec::new(),
                    });
                    Ok(true)
                })
                .map_err(|error| Self::map_osc_error(&error))?;
            self.meter.check_work_unit_deadline()?;
            Ok(roots)
        }

        /// Evaluate bounded device `_CRS` objects before native regions are
        /// activated, and return only fixed consumer SystemIO ranges. AML
        /// that cannot produce a valid resource template remains unauthorized.
        pub fn preflight_system_io_resources(
            &self,
        ) -> Result<Vec<crate::region::SystemIoResource>, LoadError> {
            if self.region_activation_attempted.load(Ordering::Acquire)
                || self
                    .system_io_preflight_attempted
                    .swap(true, Ordering::AcqRel)
            {
                return Err(LoadError::InvalidProfile);
            }
            let _budget_scope = self.meter.begin_work_unit()?;
            let scratch_bytes = core::mem::size_of::<String>()
                .checked_add(255 + 24)
                .and_then(|per_path| MAX_SYSTEM_IO_DEVICES.checked_mul(per_path))
                .and_then(|paths| {
                    let path_bytes = 255_usize.checked_add(24)?;
                    let per_resource = core::mem::size_of::<crate::region::SystemIoResource>()
                        .checked_add(path_bytes)?;
                    MAX_SYSTEM_IO_RESOURCES
                        .checked_mul(per_resource)
                        .and_then(|resources| paths.checked_add(resources))
                })
                .and_then(|bytes| u64::try_from(bytes).ok())
                .ok_or(LoadError::BudgetExceeded)?;
            let _preflight_heap = self.meter.reserve_working_heap(scratch_bytes)?;
            let namespace = self.interpreter.namespace.lock();
            let mut device_paths: AmlVec<AmlString> = aml_allocator::vec_new();
            reserve_bounded_aml_output(
                &mut device_paths,
                MAX_SYSTEM_IO_DEVICES,
                MAX_SYSTEM_IO_DEVICES,
            )?;
            let mut exceeded_device_limit = false;
            namespace
                .traverse_metered(self.meter.as_ref(), |path, level| {
                    if !matches!(
                        level.kind,
                        NamespaceLevelKind::Device
                            | NamespaceLevelKind::Processor
                            | NamespaceLevelKind::ThermalZone
                            | NamespaceLevelKind::PowerResource
                    ) {
                        return Ok(true);
                    }
                    if device_paths.len() == MAX_SYSTEM_IO_DEVICES {
                        exceeded_device_limit = true;
                        return Ok(false);
                    }
                    if let Some(path) = path.try_to_string_bounded(255)? {
                        device_paths.push(path);
                    }
                    Ok(true)
                })
                .map_err(|error| Self::map_osc_error(&error))?;
            drop(namespace);
            if exceeded_device_limit {
                return Err(LoadError::BudgetExceeded);
            }

            let mut authorized = Vec::new();
            reserve_bounded_output(
                &mut authorized,
                MAX_SYSTEM_IO_RESOURCES,
                MAX_SYSTEM_IO_RESOURCES,
            )?;
            for parent_device_path in device_paths {
                self.charge_loop_iteration()?;
                let cached = self.evaluate_crs(&parent_device_path)?;
                if let CachedCrsData::Available { system_io, .. } = cached.as_ref() {
                    if authorized.len().saturating_add(system_io.len()) > MAX_SYSTEM_IO_RESOURCES {
                        return Err(LoadError::BudgetExceeded);
                    }
                    for resource in system_io {
                        self.charge_loop_iteration()?;
                        authorized.push(try_copy_system_io_resource(resource)?);
                        if authorized.len() & 0x3f == 0 {
                            self.meter.check_work_unit_deadline()?;
                        }
                    }
                }
                self.meter.check_work_unit_deadline()?;
            }
            self.meter.check_work_unit_deadline()?;
            Ok(authorized)
        }

        /// Discover only PCI root bridge facts needed for a later platform
        /// topology binding. This evaluates the typed `_HID`, `_SEG`, `_BBN`
        /// and `_CRS` entry set; it does not expose AML objects or physical
        /// addresses and it never invents a bus range when firmware omits one.
        pub fn get_pci_roots(&self) -> Result<PciRootDiscovery, LoadError> {
            self.ensure_aml_execution_ready()?;
            let _budget_scope = self.meter.begin_work_unit()?;
            let namespace = self.interpreter.namespace.lock();
            let (candidate_capacity, candidate_heap_bytes) = namespace
                .device_candidate_heap_upper_bound_metered(self.meter.as_ref())
                .map_err(|error| Self::map_osc_error(&error))?;
            let _candidate_heap = self.meter.reserve_working_heap(candidate_heap_bytes)?;
            let mut candidates = Vec::new();
            candidates
                .try_reserve_exact(candidate_capacity)
                .map_err(|_| LoadError::BudgetExceeded)?;
            let osc_name = NameSeg::from_str("_OSC").map_err(|_| LoadError::InvalidPciRoot)?;
            namespace
                .traverse_metered(self.meter.as_ref(), |path, level| {
                    if level.kind == NamespaceLevelKind::Device {
                        let has_osc = level.values.contains_key(&osc_name);
                        candidates.push((path.try_clone()?, has_osc));
                    }
                    Ok(true)
                })
                .map_err(|error| Self::map_osc_error(&error))?;
            drop(namespace);

            let mut roots = Vec::new();
            let root_output_bytes = core::mem::size_of::<PciRootBridgeDescriptor>()
                .checked_add(255 + 24)
                .and_then(|bytes| MAX_PCI_ROOTS.checked_mul(bytes))
                .and_then(|bytes| u64::try_from(bytes).ok())
                .ok_or(LoadError::BudgetExceeded)?;
            let _root_output_heap = self.meter.reserve_working_heap(root_output_bytes)?;
            reserve_bounded_output(&mut roots, MAX_PCI_ROOTS, MAX_PCI_ROOTS)?;
            let device_candidates = candidates.len() as u32;
            let mut hid_matches = 0_u32;
            let mut osc_candidates = 0_u32;
            let mut adr_zero_candidates = 0_u32;
            let mut crs_address_spaces = 0_u32;
            let mut bus_range_matches = 0_u32;
            for (path, has_osc) in candidates {
                self.charge_loop_iteration()?;
                if has_osc {
                    osc_candidates = osc_candidates.saturating_add(1);
                }
                let hid = self.evaluate_root_object(&path, "_HID")?;
                let cid = self.evaluate_root_object(&path, "_CID")?;
                let hid_matches_root = hid
                    .as_ref()
                    .is_some_and(|value| Self::is_pci_root_hid(&**value));
                let cid_matches_root = cid
                    .as_ref()
                    .is_some_and(|value| Self::is_pci_root_cid(&**value));
                if hid_matches_root {
                    hid_matches = hid_matches.saturating_add(1);
                }
                if !hid_matches_root && !cid_matches_root {
                    continue;
                }
                let adr_zero = self
                    .evaluate_root_object(&path, "_ADR")?
                    .is_some_and(|value| matches!(&*value, Object::Integer(0)));
                if adr_zero {
                    adr_zero_candidates = adr_zero_candidates.saturating_add(1);
                }
                let (bus_window, address_spaces, resource_windows) =
                    match self.root_bus_window(&path) {
                        Ok(result) => result,
                        Err(
                            error @ (LoadError::BudgetExceeded
                            | LoadError::DeadlineExceeded
                            | LoadError::CleanupFailed
                            | LoadError::GlobalLockOutcomeUnknown
                            | LoadError::NativeRegionOutcomeUnknown),
                        ) => return Err(error),
                        Err(_) => continue,
                    };
                crs_address_spaces = crs_address_spaces.saturating_add(address_spaces);
                let Some((start_bus, end_bus)) = bus_window else {
                    continue;
                };
                bus_range_matches = bus_range_matches.saturating_add(1);
                let segment = self.evaluate_root_integer(&path, "_SEG")?.unwrap_or(0);
                let base_bus = self
                    .evaluate_root_integer(&path, "_BBN")?
                    .unwrap_or(u64::from(start_bus));
                if u8::try_from(base_bus).map_err(|_| LoadError::InvalidPciRoot)? != start_bus {
                    return Err(LoadError::InvalidPciRoot);
                }
                let segment = u16::try_from(segment).map_err(|_| LoadError::InvalidPciRoot)?;
                let namespace_path = path
                    .try_to_string_bounded(255)
                    .map_err(|error| Self::map_osc_error(&error))?
                    .ok_or(LoadError::InvalidPciRoot)?;
                if roots.len() == MAX_PCI_ROOTS {
                    return Err(LoadError::BudgetExceeded);
                }
                let (root_id, device_id) =
                    pci_root_identity(self.summary.generation, &namespace_path);
                roots.push(PciRootBridgeDescriptor {
                    root_id,
                    device_id,
                    namespace_path: try_copy_aml_string(&namespace_path, 255)?,
                    segment,
                    start_bus,
                    end_bus,
                    aml_namespace_generation: self.summary.generation,
                    resource_windows,
                });
            }
            self.meter.check_work_unit_deadline()?;
            Ok(PciRootDiscovery {
                roots,
                device_candidates,
                hid_matches,
                osc_candidates,
                adr_zero_candidates,
                crs_address_spaces,
                bus_range_matches,
            })
        }

        /// Resolve every native PCI_Config region to its containing PCI
        /// function using the `_ADR` values already present in the loaded
        /// namespace. A missing, malformed, or over-deep path is omitted so
        /// the kernel cannot accidentally interpret an incomplete path as a
        /// different function.
        pub fn pci_config_region_paths(
            &self,
            roots: &[PciRootBridgeDescriptor],
        ) -> Result<Vec<PciConfigRegionPath>, LoadError> {
            if roots.len() > MAX_PCI_ROOTS {
                return Err(LoadError::BudgetExceeded);
            }
            let _budget_scope = self.meter.begin_work_unit()?;
            fn is_path_prefix(prefix: &str, path: &str) -> bool {
                path == prefix
                    || path
                        .strip_prefix(prefix)
                        .is_some_and(|suffix| suffix.starts_with('.'))
            }

            let namespace = self.interpreter.namespace.lock();
            let (device_path_count, region_parent_count, path_heap_bytes) = namespace
                .pci_config_path_heap_upper_bound_metered(self.meter.as_ref())
                .map_err(|error| Self::map_osc_error(&error))?;
            if device_path_count > MAX_PCI_NAMESPACE_PATHS
                || region_parent_count > MAX_PCI_NAMESPACE_PATHS
            {
                return Err(LoadError::BudgetExceeded);
            }
            let per_region_result_bytes = core::mem::size_of::<PciConfigRegionPath>()
                .checked_add(MAX_PCI_DEVICE_FUNCTION_PATH + 24)
                .ok_or(LoadError::BudgetExceeded)?;
            let result_heap_bytes = region_parent_count
                .checked_mul(per_region_result_bytes)
                .and_then(|bytes| {
                    MAX_PCI_DEVICE_FUNCTION_PATH
                        .checked_mul(core::mem::size_of::<&str>())
                        .and_then(|scratch| bytes.checked_add(scratch + 24))
                })
                .and_then(|bytes| u64::try_from(bytes).ok())
                .ok_or(LoadError::BudgetExceeded)?;
            let _path_heap = self.meter.reserve_working_heap(
                path_heap_bytes
                    .checked_add(result_heap_bytes)
                    .ok_or(LoadError::BudgetExceeded)?,
            )?;
            let mut device_paths: AmlVec<AmlString> = aml_allocator::vec_new();
            reserve_bounded_aml_output(
                &mut device_paths,
                device_path_count,
                MAX_PCI_NAMESPACE_PATHS,
            )?;
            let mut region_parents: AmlVec<AmlString> = aml_allocator::vec_new();
            reserve_bounded_aml_output(
                &mut region_parents,
                region_parent_count,
                MAX_PCI_NAMESPACE_PATHS,
            )?;
            namespace
                .traverse_metered(self.meter.as_ref(), |path, level| {
                    if level.kind == NamespaceLevelKind::Device {
                        let namespace_path = path
                            .try_to_string_bounded(MAX_NAMESPACE_PATH_BYTES)?
                            .ok_or_else(|| AmlError::BudgetExceeded)?;
                        device_paths.push(namespace_path);
                    }
                    for (_, value) in level.values.values() {
                        if let Object::OpRegion(region) = &**value {
                            if region.space == acpi::aml::op_region::RegionSpace::PciConfig {
                                let parent_path = region
                                    .parent_device_path
                                    .try_to_string_bounded(MAX_NAMESPACE_PATH_BYTES)?
                                    .ok_or_else(|| AmlError::BudgetExceeded)?;
                                region_parents.push(parent_path);
                            }
                        }
                    }
                    Ok(true)
                })
                .map_err(|error| Self::map_osc_error(&error))?;
            drop(namespace);

            self.sort_namespace_paths_metered(&mut device_paths)?;
            self.sort_namespace_paths_metered(&mut region_parents)?;
            let mut unique_count = 0;
            for index in 0..region_parents.len() {
                self.charge_loop_iteration()?;
                if index & 0x3f == 0 {
                    self.meter.check_work_unit_deadline()?;
                }
                let is_duplicate =
                    unique_count != 0 && region_parents[index] == region_parents[unique_count - 1];
                if !is_duplicate {
                    if unique_count != index {
                        region_parents.swap(unique_count, index);
                    }
                    unique_count += 1;
                }
            }
            region_parents.truncate(unique_count);
            let mut result = Vec::new();
            reserve_bounded_output(&mut result, region_parents.len(), MAX_PCI_NAMESPACE_PATHS)?;
            let mut path_devices = Vec::<&str>::new();
            reserve_bounded_output(
                &mut path_devices,
                MAX_PCI_DEVICE_FUNCTION_PATH,
                MAX_PCI_DEVICE_FUNCTION_PATH,
            )?;
            for (index, parent_device_path) in region_parents.into_iter().enumerate() {
                self.charge_loop_iteration()?;
                if index & 0x3f == 0 {
                    self.meter.check_work_unit_deadline()?;
                }
                let mut selected_root = None;
                for candidate in roots {
                    self.charge_loop_iteration()?;
                    if is_path_prefix(&candidate.namespace_path, &parent_device_path)
                        && selected_root.is_none_or(|current: &PciRootBridgeDescriptor| {
                            candidate.namespace_path.len() > current.namespace_path.len()
                        })
                    {
                        selected_root = Some(candidate);
                    }
                }
                let Some(root) = selected_root else {
                    continue;
                };

                let mut ancestor_scan_steps = 0_usize;
                let has_complete_path = collect_pci_device_path_ancestors_metered(
                    &root.namespace_path,
                    &parent_device_path,
                    &device_paths,
                    &mut path_devices,
                    || {
                        self.charge_loop_iteration()?;
                        ancestor_scan_steps = ancestor_scan_steps
                            .checked_add(1)
                            .ok_or(LoadError::BudgetExceeded)?;
                        if ancestor_scan_steps & 0x3f == 0 {
                            self.meter.check_work_unit_deadline()?;
                        }
                        Ok(())
                    },
                )?;
                if !has_complete_path {
                    continue;
                }

                let mut device_function_path = Vec::new();
                reserve_bounded_output(
                    &mut device_function_path,
                    path_devices.len(),
                    MAX_PCI_DEVICE_FUNCTION_PATH,
                )?;
                let mut valid = true;
                for path in &path_devices {
                    self.charge_loop_iteration()?;
                    let scope = AmlName::from_str(path).map_err(|_| LoadError::InvalidPciRoot)?;
                    let Some(address) = self.evaluate_root_integer(&scope, "_ADR")? else {
                        valid = false;
                        break;
                    };
                    if address > u64::from(u32::MAX) {
                        valid = false;
                        break;
                    }
                    let device = (address >> 16) as u32;
                    let function = (address & 0xffff) as u32;
                    if device >= 32 || function >= 8 {
                        valid = false;
                        break;
                    }
                    device_function_path.push((device as u8) | ((function as u8) << 5));
                }
                if valid {
                    result.push(PciConfigRegionPath {
                        parent_device_path: try_copy_aml_string(
                            &parent_device_path,
                            MAX_NAMESPACE_PATH_BYTES,
                        )?,
                        root_id: root.root_id,
                        segment: root.segment,
                        root_start_bus: root.start_bus,
                        root_end_bus: root.end_bus,
                        device_function_path,
                    });
                }
            }
            self.meter.check_work_unit_deadline()?;
            Ok(result)
        }

        /// Associate bounded platform device snapshots with their nearest PCI
        /// root using the `_ADR` chain already present in the namespace. Invalid
        /// or incomplete paths are omitted and cannot be used as a BDF claim.
        pub fn pci_device_function_paths(
            &self,
            devices: &[DeviceSnapshot],
            roots: &[PciRootBridgeDescriptor],
        ) -> Result<Vec<PciDeviceFunctionPath>, LoadError> {
            if devices.len() > 256 || roots.len() > MAX_PCI_ROOTS {
                return Err(LoadError::BudgetExceeded);
            }
            if devices.iter().any(|device| {
                device.device_id == 0
                    || device.namespace_generation != self.summary.generation
                    || device.path.len() > 255
            }) {
                return Err(LoadError::InvalidPciRoot);
            }

            let _budget_scope = self.meter.begin_work_unit()?;
            fn is_path_prefix(prefix: &str, path: &str) -> bool {
                path == prefix
                    || path
                        .strip_prefix(prefix)
                        .is_some_and(|suffix| suffix.starts_with('.'))
            }

            let namespace = self.interpreter.namespace.lock();
            let (path_count, path_heap_bytes) = namespace
                .device_path_string_heap_upper_bound_metered(self.meter.as_ref())
                .map_err(|error| Self::map_osc_error(&error))?;
            if path_count > MAX_PCI_NAMESPACE_PATHS {
                return Err(LoadError::BudgetExceeded);
            }
            let per_result_bytes = core::mem::size_of::<PciDeviceFunctionPath>()
                .checked_add(MAX_PCI_DEVICE_FUNCTION_PATH + 24)
                .ok_or(LoadError::BudgetExceeded)?;
            let result_heap_bytes = devices
                .len()
                .checked_mul(per_result_bytes)
                .and_then(|bytes| {
                    bytes.checked_add(
                        MAX_PCI_DEVICE_FUNCTION_PATH
                            .checked_mul(core::mem::size_of::<&str>())?
                            .checked_add(24)?,
                    )
                })
                .and_then(|bytes| {
                    bytes.checked_add(
                        MAX_PCI_DEVICE_FUNCTION_PATH
                            .checked_mul(core::mem::size_of::<u64>())?
                            .checked_add(24)?,
                    )
                })
                .and_then(|bytes| u64::try_from(bytes).ok())
                .ok_or(LoadError::BudgetExceeded)?;
            let _path_snapshot_heap = self.meter.reserve_working_heap(
                path_heap_bytes
                    .checked_add(result_heap_bytes)
                    .ok_or(LoadError::BudgetExceeded)?,
            )?;
            let mut namespace_device_paths: AmlVec<AmlString> = aml_allocator::vec_new();
            reserve_bounded_aml_output(
                &mut namespace_device_paths,
                path_count,
                MAX_PCI_NAMESPACE_PATHS,
            )?;
            namespace
                .traverse_metered(self.meter.as_ref(), |path, level| {
                    if level.kind == NamespaceLevelKind::Device {
                        let namespace_path = path
                            .try_to_string_bounded(MAX_NAMESPACE_PATH_BYTES)?
                            .ok_or_else(|| {
                                AmlError::HostError(
                                    "AML_BUDGET_EXHAUSTED: PCI namespace path limit".to_string(),
                                )
                            })?;
                        namespace_device_paths.push(namespace_path);
                    }
                    Ok(true)
                })
                .map_err(|error| Self::map_osc_error(&error))?;
            drop(namespace);
            self.sort_namespace_paths_metered(&mut namespace_device_paths)?;

            let mut result = Vec::new();
            reserve_bounded_output(&mut result, devices.len(), devices.len())?;
            let mut path_devices = Vec::<&str>::new();
            reserve_bounded_output(
                &mut path_devices,
                MAX_PCI_DEVICE_FUNCTION_PATH,
                MAX_PCI_DEVICE_FUNCTION_PATH,
            )?;
            let mut addresses = Vec::<u64>::new();
            reserve_bounded_output(
                &mut addresses,
                MAX_PCI_DEVICE_FUNCTION_PATH,
                MAX_PCI_DEVICE_FUNCTION_PATH,
            )?;
            for device in devices {
                let mut selected_root = None;
                for candidate in roots {
                    self.charge_loop_iteration()?;
                    if candidate.namespace_path != device.path
                        && is_path_prefix(&candidate.namespace_path, &device.path)
                        && selected_root.is_none_or(|current: &PciRootBridgeDescriptor| {
                            candidate.namespace_path.len() > current.namespace_path.len()
                        })
                    {
                        selected_root = Some(candidate);
                    }
                }
                let Some(root) = selected_root else {
                    continue;
                };

                let mut ancestor_scan_steps = 0_usize;
                let has_complete_path = collect_pci_device_path_ancestors_metered(
                    &root.namespace_path,
                    &device.path,
                    &namespace_device_paths,
                    &mut path_devices,
                    || {
                        self.charge_loop_iteration()?;
                        ancestor_scan_steps = ancestor_scan_steps
                            .checked_add(1)
                            .ok_or(LoadError::BudgetExceeded)?;
                        if ancestor_scan_steps & 0x3f == 0 {
                            self.meter.check_work_unit_deadline()?;
                        }
                        Ok(())
                    },
                )?;
                if !has_complete_path {
                    continue;
                }
                if path_devices.last().copied() != Some(device.path.as_str()) {
                    continue;
                }
                addresses.clear();
                let mut valid = true;
                for path in &path_devices {
                    self.charge_loop_iteration()?;
                    let scope = AmlName::from_str(path).map_err(|_| LoadError::InvalidPciRoot)?;
                    match self.evaluate_root_integer(&scope, "_ADR") {
                        Ok(Some(address)) => addresses.push(address),
                        Ok(None) => {
                            valid = false;
                            break;
                        }
                        Err(error @ (LoadError::BudgetExceeded | LoadError::DeadlineExceeded)) => {
                            return Err(error);
                        }
                        Err(_) => {
                            valid = false;
                            break;
                        }
                    }
                }
                if !valid {
                    continue;
                }
                let Some(device_function_path) =
                    pack_pci_device_function_path_with_budget(&addresses, || {
                        self.charge_loop_iteration()
                    })?
                else {
                    continue;
                };
                result.push(PciDeviceFunctionPath {
                    device_id: device.device_id,
                    root_id: root.root_id,
                    device_function_path,
                });
            }
            self.meter.check_work_unit_deadline()?;
            Ok(result)
        }

        pub fn summary(&self) -> NamespaceSummary {
            let mut summary = self.summary;
            summary.opcode_fuel_remaining = self.meter.opcode_fuel_remaining();
            summary.global_lock_dependency |= self.interpreter.has_global_lock_dependency();
            summary.native_access_attempted = self.native_access.load(Ordering::Acquire)
                || self.raw_handler_access.load(Ordering::Acquire);
            summary.native_access_authorized =
                self.native_access_authorized.load(Ordering::Acquire);
            summary.pic_evaluated = self.pic_evaluated.load(Ordering::Acquire);
            summary.pic_degraded |= self.pic_degraded.load(Ordering::Acquire)
                || self.pic_reentry.load(Ordering::Acquire);
            summary.region_registration_degraded =
                self.region_registration_degraded.load(Ordering::Acquire);
            summary.deadline_exceeded = self.meter.deadline_failed();
            summary
        }

        #[cfg(test)]
        pub fn evaluate_method_for_test(&self, path: &str) -> Result<u64, String> {
            let _work_unit = self
                .meter
                .begin_work_unit()
                .map_err(|error| format!("{error:?}"))?;
            let name = AmlName::from_str(path).map_err(|error| format!("{error:?}"))?;
            let value = self
                .interpreter
                .evaluate(name, aml_allocator::vec_new())
                .map_err(|error| format!("{error:?}"))?;
            value.as_integer().map_err(|error| format!("{error:?}"))
        }

        pub fn compatibility_profile_hash(&self) -> u64 {
            self.compatibility_profile_hash
        }

        /// Read the fixed S5 sleep types from the static AML namespace.
        /// This deliberately does not evaluate an AML method or accept a
        /// caller-selected namespace path.
        pub fn get_s5_sleep_types(&self) -> Result<SleepTypes, LoadError> {
            let _budget_scope = self.meter.begin_work_unit()?;

            let path_component_count = 2_usize;
            let path_heap_bytes = core::mem::size_of::<Vec<acpi::aml::namespace::NameComponent>>()
                .checked_add(
                    path_component_count
                        .checked_mul(core::mem::size_of::<acpi::aml::namespace::NameComponent>())
                        .ok_or(LoadError::BudgetExceeded)?,
                )
                .and_then(|bytes| bytes.checked_add(24))
                .and_then(|bytes| u64::try_from(bytes).ok())
                .ok_or(LoadError::BudgetExceeded)?;
            let _path_scratch = self.meter.reserve_working_heap(path_heap_bytes)?;
            let path = AmlName::from_str("\\_S5").map_err(|_| LoadError::InvalidRootPath)?;
            let namespace = self.interpreter.namespace.lock();
            let object = namespace
                .get_absolute_if_present(&path)
                .map_err(|error| Self::map_osc_error(&error))?
                .ok_or(LoadError::SleepStateUnavailable)?;
            let Object::Package(entries) = &*object else {
                return Err(LoadError::InvalidSleepState);
            };
            if entries.len() != 2 {
                return Err(LoadError::InvalidSleepState);
            }

            fn sleep_type(value: &WrappedObject) -> Result<u8, LoadError> {
                let mut object = &**value;
                for depth in 0..=MAX_IDENTIFIER_REFERENCE_DEPTH {
                    match object {
                        Object::Integer(value) if *value <= 7 => return Ok(*value as u8),
                        Object::Integer(_) => return Err(LoadError::InvalidSleepState),
                        Object::Reference { inner, .. }
                            if depth < MAX_IDENTIFIER_REFERENCE_DEPTH =>
                        {
                            object = &**inner;
                        }
                        _ => return Err(LoadError::InvalidSleepState),
                    }
                }
                Err(LoadError::InvalidSleepState)
            }

            let sleep_types = SleepTypes {
                type_a: sleep_type(&entries[0])?,
                type_b: sleep_type(&entries[1])?,
            };
            drop(namespace);
            self.meter.check_work_unit_deadline()?;
            Ok(sleep_types)
        }

        #[cfg(test)]
        pub(super) fn set_heap_budget_to_persistent_usage_for_test(&self) {
            let mut state = self.meter.state.lock().unwrap();
            state.max_heap_bytes = state.heap_bytes;
        }

        #[cfg(test)]
        pub(super) fn set_loop_budget_to_current_usage_for_test(&self) {
            let mut state = self.meter.state.lock().unwrap();
            state.max_loop_iterations = state.loop_iterations;
        }

        /// Activate native OperationRegion handlers through the interpreter's
        /// `_REG` lifecycle hook, then perform the one-shot `_PIC` handshake.
        /// This order ensures `_PIC` can use connected regions and that every
        /// later `_PRT` evaluation sees the selected interrupt model.
        pub fn activate_native_regions(&self) -> Result<(), LoadError> {
            if self
                .region_activation_attempted
                .swap(true, Ordering::AcqRel)
            {
                return Err(LoadError::InvalidProfile);
            }
            let _budget_scope = self.meter.begin_work_unit()?;
            let activation = self.interpreter.initialize_native_regions();
            self.invalidate_crs_cache()?;
            activation.map_err(|error| {
                self.region_registration_degraded
                    .store(true, Ordering::Release);
                Self::map_osc_error(&error)
            })?;
            self.native_regions_active.store(true, Ordering::Release);
            if let Err(pic_error) = self.initialize_interrupt_model() {
                let cleanup = self.interpreter.deinitialize_native_regions();
                self.native_regions_active.store(false, Ordering::Release);
                if cleanup.is_err() {
                    self.region_registration_degraded
                        .store(true, Ordering::Release);
                    return Err(LoadError::CleanupFailed);
                }
                return Err(pic_error);
            }
            self.meter.check_work_unit_deadline()
        }

        fn initialize_interrupt_model(&self) -> Result<(), LoadError> {
            if !self.native_regions_active.load(Ordering::Acquire)
                || self.pic_attempted.swap(true, Ordering::AcqRel)
            {
                return Err(LoadError::InvalidProfile);
            }
            match self.interpreter.evaluate_pic_once(acpi::aml_vec![
                Object::Integer(u64::from(self.pic_mode)).wrap()
            ]) {
                Ok(Some(_)) => {
                    self.pic_evaluated.store(true, Ordering::Release);
                    self.interpreter.enable_routing_methods();
                    Ok(())
                }
                Ok(None) => {
                    self.interpreter.enable_routing_methods();
                    Ok(())
                }
                Err(error) => {
                    self.pic_degraded.store(true, Ordering::Release);
                    Err(Self::map_osc_error(&error))
                }
            }
        }

        /// Explicitly deactivate native OperationRegion registrations. This
        /// is a recovery/teardown action, never an implicit retry path.
        pub fn deactivate_native_regions(&self) -> Result<(), LoadError> {
            let _budget_scope = self.meter.begin_work_unit()?;
            let result = self.interpreter.deinitialize_native_regions();
            self.native_regions_active.store(false, Ordering::Release);
            let cache_result = self.invalidate_crs_cache();
            result.map_err(|error| {
                self.region_registration_degraded
                    .store(true, Ordering::Release);
                Self::map_osc_error(&error)
            })?;
            cache_result?;
            self.meter.check_work_unit_deadline()
        }

        /// Evaluate the policy-approved device initialization subset in the
        /// interpreter's one ordering domain.  A failed `_STA`/`_INI` is
        /// reported on that device as degraded; only a budget exhaustion or
        /// traversal failure aborts the bounded snapshot.  The entry policy
        /// is explicit so this operation cannot silently grow into a generic
        /// AML evaluator.
        pub fn snapshot_devices(
            &self,
            limit: usize,
            policy: DeviceSnapshotPolicy,
        ) -> Result<Vec<DeviceSnapshot>, LoadError> {
            if limit > 256 {
                return Err(LoadError::BudgetExceeded);
            }
            self.ensure_aml_execution_ready()?;
            policy.validate()?;
            if !policy
                .hardware
                .is_subset_of(self.hardware_side_effect_profile)
            {
                return Err(LoadError::InvalidProfile);
            }
            let _budget_scope = self.meter.begin_work_unit()?;
            let _hardware_profile_scope = HardwareSideEffectProfileScope::new(
                self.active_hardware_side_effect_profile.clone(),
                policy.hardware,
            );
            let snapshot_heap_bytes = snapshot_working_heap_bytes(limit, policy.max_snapshot_bytes)
                .ok_or(LoadError::BudgetExceeded)?;
            let _snapshot_heap = self.meter.reserve_working_heap(snapshot_heap_bytes)?;
            let namespace_guard = self.interpreter.namespace.lock();
            let (candidate_capacity, candidate_heap_bytes) = namespace_guard
                .device_like_candidate_heap_upper_bound_metered(self.meter.as_ref())
                .map_err(|error| Self::map_osc_error(&error))?;
            let _candidate_heap = self.meter.reserve_working_heap(candidate_heap_bytes)?;
            let mut candidates = Vec::new();
            candidates
                .try_reserve_exact(candidate_capacity)
                .map_err(|_| LoadError::BudgetExceeded)?;
            namespace_guard
                .traverse_metered(self.meter.as_ref(), |path, level| {
                    if matches!(
                        level.kind,
                        NamespaceLevelKind::Device
                            | NamespaceLevelKind::Processor
                            | NamespaceLevelKind::ThermalZone
                            | NamespaceLevelKind::PowerResource
                    ) {
                        candidates.push((path.try_clone()?, level.kind));
                    }
                    Ok(true)
                })
                .map_err(|error| Self::map_osc_error(&error))?;
            drop(namespace_guard);
            let mut devices = Vec::new();
            devices
                .try_reserve_exact(limit)
                .map_err(|_| LoadError::BudgetExceeded)?;
            let mut snapshot_bytes = 0_usize;
            let mut skipped_subtree: Option<&AmlName> = None;
            for (path, _) in &candidates {
                self.charge_loop_iteration()?;
                if let Some(parent) = skipped_subtree {
                    if path.is_descendant_of(parent) {
                        continue;
                    }
                    skipped_subtree = None;
                }
                if devices.len() >= limit {
                    return Err(LoadError::BudgetExceeded);
                }
                let should_descend = (|| -> Result<bool, AmlError> {
                    let path_string = path.try_to_string_bounded(255)?.ok_or_else(|| {
                        AmlError::HostError("AML_SNAPSHOT_PATH_TOO_LONG".to_string())
                    })?;
                    let device_requires_global_lock = self
                        .interpreter
                        .namespace
                        .lock()
                        .path_requires_global_lock(path);

                    let mut status = 0x0f_u64;
                    let mut status_evaluated = false;
                    let mut degraded = false;
                    let mut global_lock_rejected = false;
                    if policy.evaluate_sta {
                        status_evaluated = true;
                        match self.interpreter.evaluate_if_present_at_scope(
                            "_STA",
                            path,
                            acpi::aml_vec![],
                        ) {
                            Ok(Some(value)) => match value.as_integer() {
                                Ok(value) => status = value,
                                Err(_) => {
                                    // An invalid _STA value is not evidence that
                                    // the device is present or functional.
                                    status = 0;
                                    degraded = true;
                                }
                            },
                            Ok(None) => {}
                            Err(error) => {
                                if Self::is_budget_error(&error)
                                    || Self::is_deadline_error(&error)
                                    || Self::is_global_lock_outcome_unknown(&error)
                                    || Self::is_native_region_outcome_unknown(&error)
                                {
                                    return Err(error);
                                }
                                if device_requires_global_lock {
                                    #[cfg(target_os = "naos")]
                                    log::warn!(
                                        "ACPI _GLK device _STA failed path={} error={error:?}",
                                        path_string
                                    );
                                }
                                global_lock_rejected |= Self::is_global_lock_error(&error);
                                degraded = true;
                                status = 0;
                            }
                        }
                    }

                    let present_and_functional =
                        status_evaluated && (status & 1) != 0 && (status & (1 << 3)) != 0;
                    let mut initialized = false;
                    let mut initialization_evaluated = false;
                    if present_and_functional && policy.evaluate_ini {
                        initialization_evaluated = true;
                        match self.interpreter.evaluate_if_present_at_scope(
                            "_INI",
                            path,
                            acpi::aml_vec![],
                        ) {
                            Ok(_) => initialized = true,
                            Err(error) => {
                                if Self::is_budget_error(&error)
                                    || Self::is_deadline_error(&error)
                                    || Self::is_global_lock_outcome_unknown(&error)
                                    || Self::is_native_region_outcome_unknown(&error)
                                {
                                    return Err(error);
                                }
                                if device_requires_global_lock {
                                    #[cfg(target_os = "naos")]
                                    log::warn!(
                                        "ACPI _GLK device _INI failed path={} error={error:?}",
                                        path_string
                                    );
                                }
                                global_lock_rejected |= Self::is_global_lock_error(&error);
                                degraded = true;
                            }
                        }
                    }

                    let mut identifiers = Vec::new();
                    let identifiers_evaluated = policy.evaluate_identity;
                    if policy.evaluate_identity && !global_lock_rejected {
                        let evaluate_identifier = |name: &str| {
                            self.interpreter.evaluate_if_present_at_scope(
                                name,
                                path,
                                acpi::aml_vec![],
                            )
                        };
                        let hid = match evaluate_identifier("_HID") {
                            Ok(value) => value,
                            Err(error) => {
                                if Self::is_budget_error(&error)
                                    || Self::is_deadline_error(&error)
                                    || Self::is_global_lock_outcome_unknown(&error)
                                    || Self::is_native_region_outcome_unknown(&error)
                                {
                                    return Err(error);
                                }
                                global_lock_rejected |= Self::is_global_lock_error(&error);
                                degraded = true;
                                None
                            }
                        };
                        let cid = if global_lock_rejected {
                            None
                        } else {
                            match evaluate_identifier("_CID") {
                                Ok(value) => value,
                                Err(error) => {
                                    if Self::is_budget_error(&error)
                                        || Self::is_deadline_error(&error)
                                        || Self::is_global_lock_outcome_unknown(&error)
                                        || Self::is_native_region_outcome_unknown(&error)
                                    {
                                        return Err(error);
                                    }
                                    global_lock_rejected |= Self::is_global_lock_error(&error);
                                    degraded = true;
                                    None
                                }
                            }
                        };
                        let uid = if global_lock_rejected {
                            None
                        } else {
                            match evaluate_identifier("_UID") {
                                Ok(value) => value,
                                Err(error) => {
                                    if Self::is_budget_error(&error)
                                        || Self::is_deadline_error(&error)
                                        || Self::is_global_lock_outcome_unknown(&error)
                                        || Self::is_native_region_outcome_unknown(&error)
                                    {
                                        return Err(error);
                                    }
                                    global_lock_rejected |= Self::is_global_lock_error(&error);
                                    degraded = true;
                                    None
                                }
                            }
                        };
                        match decode_device_identifiers(
                            hid.as_ref().map(|value| &**value),
                            cid.as_ref().map(|value| &**value),
                            uid.as_ref().map(|value| &**value),
                        ) {
                            Ok(decoded) => identifiers = decoded,
                            Err(DeviceIdentifierError::AllocationFailed) => {
                                return Err(AmlError::BudgetExceeded);
                            }
                            Err(_) => degraded = true,
                        }
                    }

                    let mut resources = Vec::new();
                    let mut address_spaces = Vec::new();
                    let mut resources_evaluated = false;
                    if present_and_functional && policy.evaluate_crs && !global_lock_rejected {
                        resources_evaluated = true;
                        match self.evaluate_crs(&path_string) {
                            Ok(cached) => match cached.as_ref() {
                                CachedCrsData::Missing => {}
                                CachedCrsData::Invalid => degraded = true,
                                CachedCrsData::Available {
                                    resources: cached_resources,
                                    address_spaces: cached_address_spaces,
                                    ..
                                } if cached_resources.len() <= policy.max_resources_per_device => {
                                    resources = copy_snapshot_slice(cached_resources)
                                        .map_err(|_| AmlError::BudgetExceeded)?;
                                    address_spaces = copy_snapshot_slice(cached_address_spaces)
                                        .map_err(|_| AmlError::BudgetExceeded)?;
                                }
                                CachedCrsData::Available { .. } => degraded = true,
                            },
                            Err(LoadError::BudgetExceeded) => {
                                return Err(AmlError::BudgetExceeded);
                            }
                            Err(LoadError::DeadlineExceeded) => {
                                return Err(AmlError::DeadlineExceeded);
                            }
                            Err(LoadError::GlobalLockOutcomeUnknown) => {
                                return Err(AmlError::GlobalLockOutcomeUnknown);
                            }
                            Err(LoadError::NativeRegionOutcomeUnknown) => {
                                return Err(AmlError::NativeRegionOutcomeUnknown);
                            }
                            Err(LoadError::GlobalLockTimeout) => {
                                global_lock_rejected = true;
                                degraded = true;
                            }
                            Err(LoadError::GlobalLockUnsupported) => {
                                global_lock_rejected = true;
                                degraded = true;
                            }
                            Err(_) => degraded = true,
                        }
                    }

                    let mut routing_entries = 0_u32;
                    let mut routing = Vec::new();
                    let mut routing_evaluated = false;
                    let mut routing_complete = false;
                    let pic_allows_routing =
                        !self.summary().pic_degraded && !self.pic_reentry.load(Ordering::Acquire);
                    if present_and_functional
                        && policy.evaluate_prt
                        && pic_allows_routing
                        && !global_lock_rejected
                    {
                        routing_evaluated = true;
                        match self.interpreter.evaluate_if_present_at_scope(
                            "_PRT",
                            path,
                            acpi::aml_vec![],
                        ) {
                            Ok(Some(value)) => {
                                let namespace = self.interpreter.namespace.lock();
                                let mut route_iterations = 0_usize;
                                match decode_pci_routing_table(
                                    &value,
                                    &namespace,
                                    self.summary.generation,
                                    || {
                                        self.charge_loop_iteration().map_err(
                                            |error| match error {
                                                LoadError::BudgetExceeded => {
                                                    PciRoutingError::BudgetExceeded
                                                }
                                                LoadError::DeadlineExceeded => {
                                                    PciRoutingError::DeadlineExceeded
                                                }
                                                _ => PciRoutingError::MeterFailure,
                                            },
                                        )?;
                                        route_iterations = route_iterations
                                            .checked_add(1)
                                            .ok_or(PciRoutingError::BudgetExceeded)?;
                                        if route_iterations & 0x1f == 0 {
                                            self.meter.check_work_unit_deadline().map_err(
                                                |error| match error {
                                                    LoadError::BudgetExceeded => {
                                                        PciRoutingError::BudgetExceeded
                                                    }
                                                    LoadError::DeadlineExceeded => {
                                                        PciRoutingError::DeadlineExceeded
                                                    }
                                                    _ => PciRoutingError::MeterFailure,
                                                },
                                            )?;
                                        }
                                        Ok(())
                                    },
                                ) {
                                    Ok(entries) => {
                                        routing_entries = entries.len() as u32;
                                        routing = entries;
                                        routing_complete = true;
                                    }
                                    Err(PciRoutingError::AllocationFailed) => {
                                        return Err(AmlError::BudgetExceeded);
                                    }
                                    Err(PciRoutingError::BudgetExceeded) => {
                                        return Err(AmlError::BudgetExceeded);
                                    }
                                    Err(PciRoutingError::DeadlineExceeded) => {
                                        return Err(AmlError::DeadlineExceeded);
                                    }
                                    Err(PciRoutingError::MeterFailure) => {
                                        return Err(AmlError::InternalError(
                                            "PCI routing work meter failed".to_string(),
                                        ));
                                    }
                                    Err(_) => degraded = true,
                                }
                            }
                            Ok(None) => routing_complete = true,
                            Err(error) => {
                                if Self::is_budget_error(&error)
                                    || Self::is_deadline_error(&error)
                                    || Self::is_global_lock_outcome_unknown(&error)
                                    || Self::is_native_region_outcome_unknown(&error)
                                {
                                    return Err(error);
                                }
                                degraded = true;
                            }
                        }
                    }

                    let entry_bytes = snapshot_entry_size_bytes(
                        &path_string,
                        &identifiers,
                        &resources,
                        &address_spaces,
                        &routing,
                    )
                    .ok_or_else(|| AmlError::HostError("AML_SNAPSHOT_SIZE_OVERFLOW".to_string()))?;
                    snapshot_bytes = snapshot_bytes.checked_add(entry_bytes).ok_or_else(|| {
                        AmlError::HostError("AML_SNAPSHOT_SIZE_OVERFLOW".to_string())
                    })?;
                    if snapshot_bytes > policy.max_snapshot_bytes {
                        return Err(AmlError::HostError("AML_SNAPSHOT_SIZE_LIMIT".to_string()));
                    }
                    let device_id = namespace_device_id(self.summary.generation, &path_string);
                    devices.push(DeviceSnapshot {
                        device_id,
                        namespace_generation: self.summary.generation,
                        path: try_copy_aml_string(&path_string, 255)
                            .map_err(|_| AmlError::BudgetExceeded)?,
                        identifiers,
                        identifiers_evaluated,
                        status,
                        status_evaluated,
                        initialized,
                        initialization_evaluated,
                        degraded,
                        resources,
                        address_spaces,
                        resources_evaluated,
                        routing_entries,
                        routing,
                        routing_evaluated,
                        routing_complete,
                    });
                    Ok(present_and_functional)
                })()
                .map_err(|error| {
                    if Self::is_budget_error(&error) {
                        LoadError::BudgetExceeded
                    } else if Self::is_deadline_error(&error) {
                        LoadError::DeadlineExceeded
                    } else if Self::is_global_lock_outcome_unknown(&error) {
                        LoadError::GlobalLockOutcomeUnknown
                    } else if Self::is_native_region_outcome_unknown(&error) {
                        LoadError::NativeRegionOutcomeUnknown
                    } else if matches!(&error, AmlError::MutexAcquireTimeout) {
                        LoadError::GlobalLockTimeout
                    } else if Self::is_global_lock_error(&error) {
                        LoadError::GlobalLockUnsupported
                    } else {
                        LoadError::AmlRejected
                    }
                })?;
                if !should_descend {
                    skipped_subtree = Some(path);
                }
            }
            self.meter.check_work_unit_deadline()?;
            Ok(devices)
        }
    }

    impl RegionAccessor for DenyAllNativeRegionBackend {
        fn lease_generation(&self, _lease_id: u64) -> Result<u64, RegionError> {
            Err(RegionError::NoMatchingLease)
        }

        fn read(&self, _lease_id: u64, _address: u64, _width: u8) -> Result<u64, RegionError> {
            Err(RegionError::NoMatchingLease)
        }

        fn write(
            &self,
            _lease_id: u64,
            _address: u64,
            _width: u8,
            _value: u64,
        ) -> Result<(), RegionError> {
            Err(RegionError::NoMatchingLease)
        }
    }

    impl NativeRegionBackend for DenyAllNativeRegionBackend {
        fn acquire_global_lock(&self, _timeout_us: u64) -> Result<(), RegionError> {
            Ok(())
        }

        fn release_global_lock(&self) -> Result<(), RegionError> {
            Ok(())
        }

        fn execution_thread_id(&self) -> Option<u64> {
            Some(1)
        }

        fn resource_generation(
            &self,
            _region: &acpi::aml::op_region::OpRegion,
            _parent_device_path: &str,
            _namespace_generation: u64,
            _mode: crate::region::AccessMode,
        ) -> Result<u64, RegionError> {
            Err(RegionError::NoMatchingLease)
        }

        fn authorize(
            &self,
            _region: &acpi::aml::op_region::OpRegion,
            _parent_device_path: &str,
            _namespace_generation: u64,
            _mode: crate::region::AccessMode,
        ) -> Result<RegionAccessToken, RegionError> {
            self.native_access.store(true, Ordering::Release);
            Err(RegionError::NoMatchingLease)
        }
    }

    impl Handler for OfflineHandler {
        unsafe fn map_physical_region<T>(
            &self,
            physical_address: usize,
            size: usize,
        ) -> RawPhysicalMapping<T> {
            self.raw_handler_access.store(true, Ordering::Release);
            unsafe { RawPhysicalMapping::new(physical_address, NonNull::dangling(), size, size) }
        }

        unsafe fn unmap_physical_region<T>(&self, _region: RawPhysicalMapping<T>) {}

        fn read_u8(&self, _address: usize) -> u8 {
            self.raw_handler_access.store(true, Ordering::Release);
            0
        }
        fn read_u16(&self, _address: usize) -> u16 {
            self.raw_handler_access.store(true, Ordering::Release);
            0
        }
        fn read_u32(&self, _address: usize) -> u32 {
            self.raw_handler_access.store(true, Ordering::Release);
            0
        }
        fn read_u64(&self, _address: usize) -> u64 {
            self.raw_handler_access.store(true, Ordering::Release);
            0
        }
        fn write_u8(&self, _address: usize, _value: u8) {
            self.raw_handler_access.store(true, Ordering::Release);
        }
        fn write_u16(&self, _address: usize, _value: u16) {
            self.raw_handler_access.store(true, Ordering::Release);
        }
        fn write_u32(&self, _address: usize, _value: u32) {
            self.raw_handler_access.store(true, Ordering::Release);
        }
        fn write_u64(&self, _address: usize, _value: u64) {
            self.raw_handler_access.store(true, Ordering::Release);
        }
        fn read_io_u8(&self, _port: u16) -> u8 {
            self.raw_handler_access.store(true, Ordering::Release);
            0
        }
        fn read_io_u16(&self, _port: u16) -> u16 {
            self.raw_handler_access.store(true, Ordering::Release);
            0
        }
        fn read_io_u32(&self, _port: u16) -> u32 {
            self.raw_handler_access.store(true, Ordering::Release);
            0
        }
        fn write_io_u8(&self, _port: u16, _value: u8) {
            self.raw_handler_access.store(true, Ordering::Release);
        }
        fn write_io_u16(&self, _port: u16, _value: u16) {
            self.raw_handler_access.store(true, Ordering::Release);
        }
        fn write_io_u32(&self, _port: u16, _value: u32) {
            self.raw_handler_access.store(true, Ordering::Release);
        }
        fn read_pci_u8(&self, _address: acpi::PciAddress, _offset: u16) -> u8 {
            self.raw_handler_access.store(true, Ordering::Release);
            0
        }
        fn read_pci_u16(&self, _address: acpi::PciAddress, _offset: u16) -> u16 {
            self.raw_handler_access.store(true, Ordering::Release);
            0
        }
        fn read_pci_u32(&self, _address: acpi::PciAddress, _offset: u16) -> u32 {
            self.raw_handler_access.store(true, Ordering::Release);
            0
        }
        fn write_pci_u8(&self, _address: acpi::PciAddress, _offset: u16, _value: u8) {
            self.raw_handler_access.store(true, Ordering::Release);
        }
        fn write_pci_u16(&self, _address: acpi::PciAddress, _offset: u16, _value: u16) {
            self.raw_handler_access.store(true, Ordering::Release);
        }
        fn write_pci_u32(&self, _address: acpi::PciAddress, _offset: u16, _value: u32) {
            self.raw_handler_access.store(true, Ordering::Release);
        }
        fn nanos_since_boot(&self) -> u64 {
            #[cfg(target_os = "naos")]
            {
                let mut clock = naos_sys::TimeClock::default();
                let status = unsafe { naos_sys::_s_clock(1, &mut clock) };
                if status != 0 || clock.tv_sec < 0 || clock.tv_nsec < 0 {
                    return u64::MAX;
                }
                let seconds = match u64::try_from(clock.tv_sec) {
                    Ok(value) => value,
                    Err(_) => return u64::MAX,
                };
                seconds
                    .checked_mul(1_000_000_000)
                    .and_then(|value| value.checked_add(u64::try_from(clock.tv_nsec).ok()?))
                    .unwrap_or(u64::MAX)
            }
            #[cfg(not(target_os = "naos"))]
            {
                static START: std::sync::OnceLock<std::time::Instant> = std::sync::OnceLock::new();
                u64::try_from(
                    START
                        .get_or_init(std::time::Instant::now)
                        .elapsed()
                        .as_nanos(),
                )
                .unwrap_or(u64::MAX)
            }
        }
        fn stall(&self, _microseconds: u64) {}
        fn sleep(&self, _milliseconds: u64) {}
        fn create_mutex(&self) -> Handle {
            Handle(self.next_mutex.fetch_add(1, Ordering::Relaxed))
        }
        fn acquire(&self, _mutex: Handle, _timeout: u16) -> Result<(), AmlError> {
            Err(AmlError::MutexAcquireTimeout)
        }
        fn release(&self, _mutex: Handle) {}
    }

    fn install_compatibility_profile(
        interpreter: &Interpreter<OfflineHandler>,
        profile: CompatibilityProfile,
    ) -> Result<(), LoadError> {
        let os_name = aml_allocator::try_string_from_str(profile.os_name)
            .map_err(|_| LoadError::BudgetExceeded)?;
        let mut namespace = interpreter.namespace.lock();
        namespace
            .insert_immutable(
                AmlName::from_str("\\_OS").map_err(|_| LoadError::InvalidProfile)?,
                Object::String(os_name).wrap(),
            )
            .map_err(|_| LoadError::InvalidProfile)?;
        namespace
            .insert_immutable(
                AmlName::from_str("\\_REV").map_err(|_| LoadError::InvalidProfile)?,
                Object::Integer(profile.os_revision).wrap(),
            )
            .map_err(|_| LoadError::InvalidProfile)?;
        let osi_windows = profile.osi_windows;
        let osi_linux = profile.osi_linux;
        namespace
            .insert_immutable(
                AmlName::from_str("\\_OSI").map_err(|_| LoadError::InvalidProfile)?,
                Object::native_method_metered(1, move |args, meter| {
                    if args.len() != 1 {
                        return Err(AmlError::MethodArgCountIncorrect);
                    }
                    let Object::String(feature) = &*args[0] else {
                        return Err(AmlError::ObjectNotOfExpectedType {
                            expected: ObjectType::String,
                            got: args[0].typ(),
                        });
                    };
                    let supported = (osi_windows && feature.starts_with("Windows "))
                        || (osi_linux && feature.as_str() == "Linux");
                    acpi::aml::wrap_metered_object(
                        Object::Integer(if supported { u64::MAX } else { 0 }),
                        meter,
                    )
                })
                .wrap(),
            )
            .map_err(|_| LoadError::InvalidProfile)?;
        Ok(())
    }

    fn fixed_namespace_heap_bytes(os_name_len: usize) -> Result<u64, LoadError> {
        const FIXED_NAMESPACE_OBJECTS: u64 = 10;
        const NAMESPACE_OBJECT_OVERHEAD: u64 = 128;
        // Five predefined scopes and five predefined values (`_GL`, `_OS`,
        // `_OSI`, `_REV`, and the sealed `_PIC` guard).
        const ENCODED_NAME_BYTES: u64 = 48;
        let name_component_bytes = core::mem::size_of::<Vec<acpi::aml::namespace::NameComponent>>()
            .checked_add(
                2_usize
                    .checked_mul(core::mem::size_of::<acpi::aml::namespace::NameComponent>())
                    .ok_or(LoadError::BudgetExceeded)?,
            )
            .and_then(|bytes| bytes.checked_add(24))
            .and_then(|bytes| u64::try_from(bytes).ok())
            .ok_or(LoadError::BudgetExceeded)?;
        FIXED_NAMESPACE_OBJECTS
            .checked_mul(NAMESPACE_OBJECT_OVERHEAD)
            .and_then(|bytes| bytes.checked_add(ENCODED_NAME_BYTES))
            .and_then(|bytes| {
                FIXED_NAMESPACE_OBJECTS
                    .checked_mul(name_component_bytes)
                    .and_then(|component_bytes| bytes.checked_add(component_bytes))
            })
            .and_then(|bytes| {
                u64::try_from(os_name_len)
                    .ok()
                    .and_then(|name_bytes| bytes.checked_add(name_bytes))
            })
            .ok_or(LoadError::BudgetExceeded)
    }

    fn seal_pic_method(
        interpreter: &Interpreter<OfflineHandler>,
        reentry: Arc<AtomicBool>,
    ) -> Result<(), LoadError> {
        let guard = Object::native_method(1, move |_args| {
            reentry.store(true, Ordering::Release);
            Err(AmlError::HostError("NOT_SUPPORTED_PIC_REENTRY".to_string()))
        })
        .wrap();
        interpreter
            .seal_pic_method(guard)
            .map_err(|_| LoadError::InvalidProfile)
    }

    pub fn load_executor(
        dsdt: &[u8],
        ssdts: &[&[u8]],
        profile: CompatibilityProfile,
        budget: ExecutionBudget,
        identity: FirmwareIdentity,
    ) -> Result<AcpiExecutor, LoadError> {
        if budget.native_access_fuel != 0 {
            return Err(LoadError::NativeAccessRejected);
        }
        let native_access = Arc::new(AtomicBool::new(false));
        load_executor_with_backend(
            dsdt,
            ssdts,
            profile,
            budget,
            identity,
            HardwareSideEffectProfile::deny_all(),
            Arc::new(DenyAllNativeRegionBackend { native_access }),
        )
    }

    /// Load the same bounded executor with a caller-supplied kernel-backed
    /// region authority. The authority remains process-local to platformd;
    /// this function does not expose an AML path or a raw Handler boundary.
    pub fn load_executor_with_backend(
        dsdt: &[u8],
        ssdts: &[&[u8]],
        profile: CompatibilityProfile,
        budget: ExecutionBudget,
        identity: FirmwareIdentity,
        hardware_side_effect_profile: HardwareSideEffectProfile,
        backend: Arc<dyn NativeRegionBackend>,
    ) -> Result<AcpiExecutor, LoadError> {
        load_executor_with_allocator_quota(
            dsdt,
            ssdts,
            profile,
            budget,
            identity,
            hardware_side_effect_profile,
            backend,
            budget.max_heap_bytes,
        )
    }

    /// Load a metered executor directly from the owned AML table inventory.
    /// This avoids building an unmetered `Vec<&[u8]>` on the platformd worker
    /// before the table-load work unit begins.
    pub fn load_executor_with_owned_backend(
        dsdt: &[u8],
        ssdts: &[Vec<u8>],
        profile: CompatibilityProfile,
        budget: ExecutionBudget,
        identity: FirmwareIdentity,
        hardware_side_effect_profile: HardwareSideEffectProfile,
        backend: Arc<dyn NativeRegionBackend>,
    ) -> Result<AcpiExecutor, LoadError> {
        load_executor_with_table_iter(
            dsdt,
            ssdts.iter().map(|table| table.as_slice()),
            profile,
            budget,
            identity,
            hardware_side_effect_profile,
            backend,
            budget.max_heap_bytes,
        )
    }

    fn load_executor_with_allocator_quota(
        dsdt: &[u8],
        ssdts: &[&[u8]],
        profile: CompatibilityProfile,
        budget: ExecutionBudget,
        identity: FirmwareIdentity,
        hardware_side_effect_profile: HardwareSideEffectProfile,
        backend: Arc<dyn NativeRegionBackend>,
        allocator_heap_bytes: u64,
    ) -> Result<AcpiExecutor, LoadError> {
        load_executor_with_table_iter(
            dsdt,
            ssdts.iter().copied(),
            profile,
            budget,
            identity,
            hardware_side_effect_profile,
            backend,
            allocator_heap_bytes,
        )
    }

    fn load_executor_with_table_iter<'a>(
        dsdt: &[u8],
        ssdts: impl Iterator<Item = &'a [u8]> + Clone,
        profile: CompatibilityProfile,
        budget: ExecutionBudget,
        identity: FirmwareIdentity,
        hardware_side_effect_profile: HardwareSideEffectProfile,
        backend: Arc<dyn NativeRegionBackend>,
        allocator_heap_bytes: u64,
    ) -> Result<AcpiExecutor, LoadError> {
        if profile.pic_mode > 1 || profile.os_name.is_empty() {
            return Err(LoadError::InvalidProfile);
        }
        if !identity.valid() {
            return Err(LoadError::InvalidIdentity);
        }
        let dsdt_descriptor = table_descriptor(dsdt, b"DSDT")?;
        if u64::from(dsdt_descriptor.length) > MAX_AML_TABLE_BYTES {
            return Err(LoadError::BudgetExceeded);
        }
        let mut all_bytes = dsdt.len() as u64;
        if all_bytes > MAX_AML_CATALOG_BYTES {
            return Err(LoadError::BudgetExceeded);
        }
        let mut table_count = 1_u32;
        let mut global_lock = false;
        for ssdt in ssdts.clone() {
            let descriptor = table_descriptor(ssdt, b"SSDT")?;
            if u64::from(descriptor.length) > MAX_AML_TABLE_BYTES {
                return Err(LoadError::BudgetExceeded);
            }
            all_bytes = all_bytes
                .checked_add(ssdt.len() as u64)
                .filter(|total| *total <= MAX_AML_CATALOG_BYTES)
                .ok_or(LoadError::BudgetExceeded)?;
            table_count = table_count
                .checked_add(1)
                .ok_or(LoadError::BudgetExceeded)?;
            if table_count as usize > MAX_AML_TABLE_COUNT {
                return Err(LoadError::BudgetExceeded);
            }
        }
        let mut execution = ExecutionState::new(budget)?;
        // Account for all fixed namespace entries, including the predefined
        // scopes and values. Firmware declarations are charged at each actual
        // namespace insertion by the pinned AML parser.
        execution.add_namespace_objects(10)?;
        execution.reserve_heap(fixed_namespace_heap_bytes(profile.os_name.len())?)?;
        execution.reserve_package(all_bytes)?;
        let generation = next_instance_generation(identity.process_id)?;

        let native_access = Arc::new(AtomicBool::new(false));
        let native_access_authorized = Arc::new(AtomicBool::new(false));
        let raw_handler_access = Arc::new(AtomicBool::new(false));
        let pic_reentry = Arc::new(AtomicBool::new(false));
        let region_registration_degraded = Arc::new(AtomicBool::new(false));
        let next_mutex = Arc::new(AtomicU32::new(1));
        let handler = OfflineHandler {
            raw_handler_access: raw_handler_access.clone(),
            next_mutex: next_mutex.clone(),
        };
        let clock = handler.clone();
        let native_access_budget = Arc::new(NativeAccessBudget::new(
            budget.native_access_fuel,
            budget.max_operation_native_access_fuel,
        ));
        let active_hardware_side_effect_profile =
            Arc::new(AtomicU8::new(hardware_side_effect_profile.to_bits()));
        let meter = aml_allocator::arc_new_unmetered(NaosExecutionMeter::new(
            execution,
            clock,
            native_access_budget.clone(),
            allocator_heap_bytes,
        )?);
        // Include interpreter construction and every fixed/profile namespace
        // insertion in the same allocator quota, deadline, and work budget as
        // AML table parsing and namespace validation.
        let _load_work_unit = meter.begin_work_unit()?;
        let interpreter = Interpreter::new(handler, dsdt_descriptor.revision);
        interpreter.install_execution_meter(meter.clone());
        interpreter.install_native_region_handler(aml_allocator::box_new(
            NativeRegionAdapter::new_with_profile_cell_and_meter(
                backend,
                generation,
                native_access_budget,
                native_access.clone(),
                native_access_authorized.clone(),
                active_hardware_side_effect_profile.clone(),
                Some(meter.clone()),
            ),
        ));
        // Install the profile before parsing so the pinned parser's immutable
        // namespace entries cannot be replaced by firmware declarations.
        install_compatibility_profile(&interpreter, profile)?;
        // Keep the DSDT and every SSDT under one aggregate fuel/deadline so a
        // catalog cannot multiply the per-table work allowance.
        interpreter.load_table(&dsdt[36..]).map_err(|error| {
            if AcpiExecutor::is_budget_error(&error) {
                LoadError::BudgetExceeded
            } else if AcpiExecutor::is_deadline_error(&error) {
                LoadError::DeadlineExceeded
            } else {
                LoadError::AmlRejected
            }
        })?;
        for ssdt in ssdts {
            interpreter.load_table(&ssdt[36..]).map_err(|error| {
                if AcpiExecutor::is_budget_error(&error) {
                    LoadError::BudgetExceeded
                } else if AcpiExecutor::is_deadline_error(&error) {
                    LoadError::DeadlineExceeded
                } else {
                    LoadError::AmlRejected
                }
            })?;
        }
        seal_pic_method(&interpreter, pic_reentry.clone())?;
        global_lock |= interpreter
            .try_detect_global_lock_namespace_dependency()
            .map_err(|error| {
                if AcpiExecutor::is_budget_error(&error) {
                    LoadError::BudgetExceeded
                } else if AcpiExecutor::is_deadline_error(&error) {
                    LoadError::DeadlineExceeded
                } else {
                    LoadError::AmlRejected
                }
            })?;
        if raw_handler_access.load(Ordering::Acquire) {
            return Err(LoadError::NativeAccessRejected);
        }
        drop(_load_work_unit);
        Ok(AcpiExecutor {
            interpreter,
            summary: NamespaceSummary {
                generation,
                tables_loaded: table_count,
                opcode_fuel_remaining: meter.opcode_fuel_remaining(),
                global_lock_dependency: global_lock,
                native_access_attempted: false,
                native_access_authorized: native_access_authorized.load(Ordering::Acquire),
                pic_evaluated: false,
                pic_degraded: false,
                region_registration_degraded: false,
                deadline_exceeded: meter.deadline_failed(),
            },
            identity,
            compatibility_profile_hash: profile.stable_hash(),
            hardware_side_effect_profile,
            active_hardware_side_effect_profile,
            meter,
            native_access,
            native_access_authorized,
            raw_handler_access,
            pic_reentry,
            pic_attempted: AtomicBool::new(false),
            pic_evaluated: AtomicBool::new(false),
            pic_degraded: AtomicBool::new(false),
            region_activation_attempted: AtomicBool::new(false),
            system_io_preflight_attempted: AtomicBool::new(false),
            native_regions_active: AtomicBool::new(false),
            pic_mode: profile.pic_mode,
            region_registration_degraded,
            crs_cache: Mutex::new(CrsCache::default()),
        })
    }

    #[cfg(all(target_os = "naos", feature = "naos-allocator-quota-smoke"))]
    pub(super) fn run_allocator_quota_smoke() -> Result<(), LoadError> {
        // The AML meter admits this 4 MiB BufferField result under its 8 MiB
        // buffer and 32 MiB heap budgets. The test-only 1 MiB allocator quota
        // must reject the real allocation inside HeapQuotaScope.
        const BUFFER_RESULT_BYTES: u64 = 4 << 20;
        const ALLOCATOR_QUOTA_BYTES: u64 = 1 << 20;
        const AML: &[u8] = &[
            // Name (BUF0, Buffer (0x04) { 1, 2, 3, 4 })
            0x08, b'B', b'U', b'F', b'0', 0x11, 0x07, 0x0a, 0x04, 1, 2, 3, 4,
            // Method (MAIN, 0) { Return (BufferField (BUF0, 0, 0x02000000)) }
            0x14, 0x1b, b'M', b'A', b'I', b'N', 0x00, 0x5b, 0x13, b'B', b'U', b'F', b'0', 0x00,
            0x0c, 0x00, 0x00, 0x00, 0x02, b'F', b'L', b'D', b'0', 0xa4, b'F', b'L', b'D', b'0',
        ];

        let table_length = 36_usize
            .checked_add(AML.len())
            .ok_or(LoadError::BudgetExceeded)?;
        let table_length = u32::try_from(table_length).map_err(|_| LoadError::BudgetExceeded)?;
        let mut dsdt = Vec::new();
        dsdt.try_reserve_exact(table_length as usize)
            .map_err(|_| LoadError::BudgetExceeded)?;
        dsdt.resize(36, 0);
        dsdt[..4].copy_from_slice(b"DSDT");
        dsdt[4..8].copy_from_slice(&table_length.to_le_bytes());
        dsdt[8] = 2;
        dsdt.extend_from_slice(AML);

        let mut budget = ExecutionBudget::offline_default();
        budget.max_buffer_bytes = 8 << 20;
        let native_access = Arc::new(AtomicBool::new(false));
        let executor = load_executor_with_allocator_quota(
            &dsdt,
            &[],
            CompatibilityProfile::default(),
            budget,
            FirmwareIdentity::new("01000000000000000200000000000000", 1),
            HardwareSideEffectProfile::deny_all(),
            Arc::new(DenyAllNativeRegionBackend { native_access }),
            1 << 20,
        )?;
        let method = AmlName::from_str("\\MAIN").map_err(|_| LoadError::AmlRejected)?;
        let _work_unit = executor.meter.begin_work_unit()?;
        let logical_heap_bytes = executor
            .meter
            .state
            .lock()
            .map_err(|_| LoadError::AmlRejected)?
            .heap_bytes;
        if BUFFER_RESULT_BYTES > budget.max_buffer_bytes
            || logical_heap_bytes
                .checked_add(BUFFER_RESULT_BYTES)
                .map_or(true, |total| total > budget.max_heap_bytes)
            || BUFFER_RESULT_BYTES <= ALLOCATOR_QUOTA_BYTES
        {
            return Err(LoadError::InvalidProfile);
        }
        match executor
            .interpreter
            .evaluate(method, aml_allocator::vec_new())
        {
            Err(error) if AcpiExecutor::is_budget_error(&error) => Ok(()),
            _ => Err(LoadError::AmlRejected),
        }
    }

    #[cfg(test)]
    mod compatibility_profile_tests {
        use super::*;

        #[test]
        fn allocator_exhaustion_maps_to_payload_free_budget_error() {
            let error = NaosExecutionMeter::map_error(LoadError::BudgetExceeded);

            assert_eq!(error, AmlError::BudgetExceeded);
            assert!(AcpiExecutor::is_budget_error(&error));
        }

        #[test]
        fn fork_scratch_reservation_cannot_release_or_commit_into_a_later_work_unit() {
            let budget = ExecutionBudget::offline_default();
            let clock = OfflineHandler {
                raw_handler_access: Arc::new(AtomicBool::new(false)),
                next_mutex: Arc::new(AtomicU32::new(1)),
            };
            let meter = NaosExecutionMeter::new(
                ExecutionState::new(budget).unwrap(),
                clock,
                Arc::new(NativeAccessBudget::new(
                    budget.native_access_fuel,
                    budget.max_operation_native_access_fuel,
                )),
                budget.max_heap_bytes,
            )
            .unwrap();

            let first_work_unit = meter.begin_work_unit().unwrap();
            let stale_generation = meter.reserve_working_heap_scoped(40).unwrap();
            drop(first_work_unit);
            assert_eq!(meter.state.lock().unwrap().stale_working_heap_bytes, 40);

            let second_work_unit = meter.begin_work_unit().unwrap();
            let active_generation = meter.reserve_working_heap_scoped(40).unwrap();
            assert_ne!(stale_generation, active_generation);

            meter.release_working_heap_scoped(40, stale_generation);
            {
                let state = meter.state.lock().unwrap();
                assert_eq!(state.working_heap_bytes, 40);
                assert_eq!(state.stale_working_heap_bytes, 0);
            }
            assert!(
                meter
                    .commit_working_heap_scoped(40, stale_generation)
                    .is_err()
            );
            assert_eq!(meter.state.lock().unwrap().working_heap_bytes, 40);

            meter.release_working_heap_scoped(40, active_generation);
            assert_eq!(meter.state.lock().unwrap().working_heap_bytes, 0);
            drop(second_work_unit);
        }

        #[test]
        fn shared_sci_work_unit_does_not_replenish_fuel_between_gpe_calls() {
            let mut budget = ExecutionBudget::offline_default();
            budget.max_operation_opcode_fuel = 4;
            budget.max_operation_loop_iterations = 4;
            let clock = OfflineHandler {
                raw_handler_access: Arc::new(AtomicBool::new(false)),
                next_mutex: Arc::new(AtomicU32::new(1)),
            };
            let meter = Arc::new(
                NaosExecutionMeter::new(
                    ExecutionState::new(budget).unwrap(),
                    clock,
                    Arc::new(NativeAccessBudget::new(
                        budget.native_access_fuel,
                        budget.max_operation_native_access_fuel,
                    )),
                    budget.max_heap_bytes,
                )
                .unwrap(),
            );
            let _sci_work_unit = NaosExecutionMeter::begin_shared_work_unit(&meter).unwrap();

            {
                let _first_gpe = meter.begin_work_unit().unwrap();
                meter.state.lock().unwrap().charge_opcode(3).unwrap();
                for _ in 0..3 {
                    meter.state.lock().unwrap().loop_iteration().unwrap();
                }
            }

            {
                let _second_gpe = meter.begin_work_unit().unwrap();
                let mut state = meter.state.lock().unwrap();
                assert_eq!(state.operation_opcode_fuel_remaining, 1);
                assert_eq!(state.charge_opcode(2), Err(LoadError::BudgetExceeded));
                assert_eq!(state.loop_iteration(), Ok(()));
                assert_eq!(state.loop_iteration(), Err(LoadError::BudgetExceeded));
            }

            _sci_work_unit.finish().unwrap();
            let _next_work_unit = meter.begin_work_unit().unwrap();
            assert_eq!(
                meter.state.lock().unwrap().operation_opcode_fuel_remaining,
                4
            );
        }

        fn dsdt_with_profile_replacement_and_store() -> Vec<u8> {
            let mut aml = vec![0x08, 0x5c, b'_', b'R', b'E', b'V', 0x0a, 0x63];
            let body = [0x70, 0x0a, 0x63, 0x5c, b'_', b'R', b'E', b'V', 0xa4, 0x00];
            let package_contents = 4 + 1 + body.len();
            aml.extend_from_slice(&[0x14, (package_contents + 1) as u8]);
            aml.extend_from_slice(b"_INI");
            aml.push(0);
            aml.extend_from_slice(&body);

            let total_length = 36 + aml.len();
            let mut table = vec![0; 36];
            table[..4].copy_from_slice(b"DSDT");
            table[4..8].copy_from_slice(&(total_length as u32).to_le_bytes());
            table[8] = 2;
            table.extend_from_slice(&aml);
            table
        }

        #[test]
        fn firmware_cannot_replace_or_store_to_the_frozen_compatibility_revision() {
            let dsdt = dsdt_with_profile_replacement_and_store();
            let executor = crate::load_executor(
                &dsdt,
                &[],
                CompatibilityProfile::default(),
                ExecutionBudget::offline_default(),
                FirmwareIdentity::new("01000000000000000200000000000000", 251),
            )
            .unwrap();
            let revision_path = AmlName::from_str("\\_REV").unwrap();
            let revision = executor
                .interpreter
                .namespace
                .lock()
                .get(revision_path)
                .unwrap();
            assert!(matches!(*revision, Object::Integer(2)));

            let _budget_scope = executor.meter.begin_work_unit().unwrap();
            let method = AmlName::from_str("\\_INI").unwrap();
            assert!(matches!(
                executor.interpreter.evaluate(method, acpi::aml_vec![]),
                Err(AmlError::StoreToInvalidReferenceType)
            ));
            let revision_path = AmlName::from_str("\\_REV").unwrap();
            let revision = executor
                .interpreter
                .namespace
                .lock()
                .get(revision_path)
                .unwrap();
            assert!(matches!(*revision, Object::Integer(2)));
        }
    }
}

#[cfg(any(target_os = "naos", test))]
pub use interpreter::{
    AcpiExecutor, SciWorkUnit, load_executor, load_executor_with_backend,
    load_executor_with_owned_backend,
};

#[cfg(all(target_os = "naos", feature = "naos-allocator-quota-smoke"))]
pub fn run_allocator_quota_smoke() -> Result<(), LoadError> {
    interpreter::run_allocator_quota_smoke()
}

#[cfg(target_os = "naos")]
pub fn load_namespace(
    dsdt: &[u8],
    ssdts: &[&[u8]],
    profile: CompatibilityProfile,
    budget: ExecutionBudget,
    identity: FirmwareIdentity,
) -> Result<NamespaceSummary, LoadError> {
    Ok(load_executor(dsdt, ssdts, profile, budget, identity)?.summary())
}

#[cfg(not(target_os = "naos"))]
pub fn load_namespace(
    dsdt: &[u8],
    _ssdts: &[&[u8]],
    profile: CompatibilityProfile,
    _budget: ExecutionBudget,
    identity: FirmwareIdentity,
) -> Result<NamespaceSummary, LoadError> {
    if profile.pic_mode > 1 || profile.os_name.is_empty() {
        return Err(LoadError::InvalidProfile);
    }
    if !identity.valid() {
        return Err(LoadError::InvalidIdentity);
    }
    table_descriptor(dsdt, b"DSDT")?;
    Err(LoadError::AmlRejected)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::native::NativeRegionBackend;
    use crate::region::{AccessMode, RegionAccessToken, RegionAccessor, RegionError};
    use acpi::aml::AmlError;
    use acpi::aml::object::Object;
    use acpi::aml::op_region::OpRegion;
    use std::sync::Arc;

    #[test]
    fn aml_error_marker_classification_reads_message_without_debug_rendering() {
        let budget = AmlError::BudgetExceeded;
        let deadline = AmlError::DeadlineExceeded;
        let native_access_budget = AmlError::NativeAccessBudgetExceeded;
        let cleanup = AmlError::InternalError("rollback/token invalidation failed".to_string());
        let unrelated = AmlError::HostError("unrelated failure".to_string());
        let structured = AmlError::InvalidName(None);

        assert!(super::interpreter::aml_error_contains_marker(
            &budget,
            "AML_BUDGET_EXHAUSTED"
        ));
        assert!(super::interpreter::aml_error_contains_marker(
            &deadline,
            "AML_DEADLINE_EXCEEDED"
        ));
        assert!(!super::interpreter::aml_error_contains_marker(
            &deadline,
            "AML_BUDGET_EXHAUSTED"
        ));
        assert!(super::interpreter::aml_error_contains_marker(
            &native_access_budget,
            "AML_NATIVE_ACCESS_BUDGET_EXHAUSTED"
        ));
        assert!(!super::interpreter::aml_error_contains_marker(
            &native_access_budget,
            "AML_BUDGET_EXHAUSTED"
        ));
        assert!(super::interpreter::aml_error_contains_marker(
            &cleanup,
            "rollback/token invalidation failed"
        ));
        assert!(!super::interpreter::aml_error_contains_marker(
            &unrelated,
            "AML_BUDGET_EXHAUSTED"
        ));
        assert!(!super::interpreter::aml_error_contains_marker(
            &structured,
            "AML_BUDGET_EXHAUSTED"
        ));
    }

    #[test]
    fn bounded_output_reservation_rejects_limits_and_capacity_overflow() {
        let mut values = Vec::<u8>::new();
        assert_eq!(
            reserve_bounded_output(&mut values, 2, 1),
            Err(LoadError::BudgetExceeded)
        );
        assert_eq!(
            reserve_bounded_output(&mut values, usize::MAX, usize::MAX),
            Err(LoadError::BudgetExceeded)
        );
        assert!(values.is_empty());
    }

    #[test]
    fn bounded_string_copy_reserves_before_copying_and_rejects_oversize() {
        assert_eq!(try_copy_bounded_string("PCI0", 4), Ok("PCI0".to_string()));
        assert_eq!(
            try_copy_bounded_string("PCI0", 3),
            Err(LoadError::BudgetExceeded)
        );
        let mut output = String::from("kept");
        assert_eq!(
            reserve_string_output(&mut output, usize::MAX),
            Err(LoadError::BudgetExceeded)
        );
        assert_eq!(output, "kept");
    }

    #[test]
    fn snapshot_output_copy_reservation_is_fallible_and_preserves_values() {
        let source = [3_u64, 5, 8];
        assert_eq!(copy_snapshot_slice(&source), Ok(source.to_vec()));

        let mut output = Vec::<u8>::new();
        assert_eq!(
            reserve_snapshot_output(&mut output, usize::MAX),
            Err(LoadError::BudgetExceeded)
        );
        assert!(output.is_empty());
    }

    #[test]
    fn pci_device_function_paths_pack_only_valid_bounded_adr_values() {
        assert_eq!(
            pack_pci_device_function_path(&[0x0001_0000, 0x0003_0001, 0x0004_0002]),
            Ok(Some(vec![1, 0x23, 0x44]))
        );
        assert_eq!(pack_pci_device_function_path(&[]), Ok(None));
        assert_eq!(pack_pci_device_function_path(&[0x0020_0000]), Ok(None));
        assert_eq!(pack_pci_device_function_path(&[0x0001_0008]), Ok(None));
        assert_eq!(pack_pci_device_function_path(&[0x1_0000_0000]), Ok(None));
        assert_eq!(pack_pci_device_function_path(&[0; 17]), Ok(None));
    }

    #[test]
    fn metered_pci_device_path_pack_charges_each_address() {
        let mut charged = 0;
        assert_eq!(
            pack_pci_device_function_path_with_budget(&[0x0001_0000, 0x0003_0001], || {
                charged += 1;
                Ok(())
            },),
            Ok(Some(vec![1, 0x23]))
        );
        assert_eq!(charged, 2);

        let mut remaining = 1;
        assert_eq!(
            pack_pci_device_function_path_with_budget(&[0x0001_0000, 0x0003_0001], || {
                if remaining == 0 {
                    return Err(LoadError::BudgetExceeded);
                }
                remaining -= 1;
                Ok(())
            },),
            Err(LoadError::BudgetExceeded)
        );
    }

    #[test]
    fn pci_path_ancestor_selection_reuses_bounded_sorted_device_paths() {
        let namespace_devices = vec![
            "\\_SB_.PCI0".to_string(),
            "\\_SB_.PCI0.DEV0".to_string(),
            "\\_SB_.PCI0.DEV0.SLOT".to_string(),
            "\\_SB_.PCI0.OTHR".to_string(),
        ];
        let mut ancestors = Vec::new();
        assert!(collect_pci_device_path_ancestors(
            "\\_SB_.PCI0",
            "\\_SB_.PCI0.DEV0.SLOT",
            &namespace_devices,
            &mut ancestors,
        ));
        assert_eq!(ancestors, ["\\_SB_.PCI0.DEV0", "\\_SB_.PCI0.DEV0.SLOT"]);

        ancestors.clear();
        assert!(!collect_pci_device_path_ancestors(
            "\\_SB_.PCI0",
            "\\_SB_.OTHER.DEV0",
            &namespace_devices,
            &mut ancestors,
        ));
        assert!(ancestors.is_empty());
    }

    #[test]
    fn metered_pci_path_ancestor_search_charges_segments_and_binary_search() {
        let namespace_devices = vec![
            "\\_SB_.PCI0".to_string(),
            "\\_SB_.PCI0.DEV0".to_string(),
            "\\_SB_.PCI0.DEV0.SLOT".to_string(),
            "\\_SB_.PCI0.OTHR".to_string(),
        ];
        let mut selected = Vec::new();
        let mut charged_iterations = 0;
        assert!(
            collect_pci_device_path_ancestors_metered(
                "\\_SB_.PCI0",
                "\\_SB_.PCI0.DEV0.SLOT",
                &namespace_devices,
                &mut selected,
                || {
                    charged_iterations += 1;
                    Ok(())
                },
            )
            .unwrap()
        );
        assert_eq!(selected, ["\\_SB_.PCI0.DEV0", "\\_SB_.PCI0.DEV0.SLOT"]);
        assert!(charged_iterations > selected.len());

        let mut remaining_iterations = 1;
        assert_eq!(
            collect_pci_device_path_ancestors_metered(
                "\\_SB_.PCI0",
                "\\_SB_.PCI0.DEV0.SLOT",
                &namespace_devices,
                &mut selected,
                || {
                    if remaining_iterations == 0 {
                        return Err(LoadError::BudgetExceeded);
                    }
                    remaining_iterations -= 1;
                    Ok(())
                },
            ),
            Err(LoadError::BudgetExceeded)
        );
    }

    #[test]
    fn namespace_path_heapsort_orders_paths_and_charges_comparisons() {
        let dsdt = dsdt_with_s5(None, &[]);
        let executor = load_executor(
            &dsdt,
            &[],
            CompatibilityProfile::default(),
            ExecutionBudget::offline_default(),
            FirmwareIdentity::new("01000000000000000200000000000000", 17),
        )
        .unwrap();
        let mut paths = vec![
            "\\_SB_.PCI0.Z".to_string(),
            "\\_SB_.PCI0.A".to_string(),
            "\\_SB_.PCI0.M".to_string(),
            "\\_SB_.PCI0.B".to_string(),
        ];

        executor.sort_namespace_paths_metered(&mut paths).unwrap();

        assert_eq!(
            paths,
            [
                "\\_SB_.PCI0.A",
                "\\_SB_.PCI0.B",
                "\\_SB_.PCI0.M",
                "\\_SB_.PCI0.Z",
            ]
        );

        let mut over_budget = vec!["\\_SB_.PCI0.Z".to_string(), "\\_SB_.PCI0.A".to_string()];
        executor.set_loop_budget_to_current_usage_for_test();
        assert_eq!(
            executor.sort_namespace_paths_metered(&mut over_budget),
            Err(LoadError::BudgetExceeded)
        );
    }

    #[test]
    fn system_io_resource_copy_is_bounded_and_preserves_facts() {
        let resource = crate::region::SystemIoResource {
            parent_device_path: "\\DEV0".to_string(),
            base: 0x3f8,
            length: 8,
        };
        assert_eq!(try_copy_system_io_resource(&resource), Ok(resource.clone()));

        let oversized = crate::region::SystemIoResource {
            parent_device_path: "x".repeat(256),
            ..resource
        };
        assert_eq!(
            try_copy_system_io_resource(&oversized),
            Err(LoadError::BudgetExceeded)
        );
    }

    #[test]
    fn crs_normalization_charges_descriptors_and_propagates_budget_failure() {
        fn irq_descriptor(irq: u32) -> acpi::aml::resource::Resource {
            use acpi::aml::resource::{
                InterruptPolarity, InterruptTrigger, IrqDescriptor, Irqs, Resource,
            };

            Resource::Irq(IrqDescriptor {
                is_consumer: true,
                trigger: InterruptTrigger::Edge,
                polarity: InterruptPolarity::ActiveHigh,
                is_shared: false,
                is_wake_capable: false,
                irqs: Irqs::from_buf([irq]),
            })
        }

        let mut charged = 0;
        let mut deadline_checks = 0;
        assert!(
            super::interpreter::AcpiExecutor::normalize_crs_with_budget(
                "\\DEV0",
                vec![irq_descriptor(5), irq_descriptor(7)],
                || {
                    charged += 1;
                    Ok(())
                },
                || {
                    deadline_checks += 1;
                    Ok(())
                },
            )
            .is_ok()
        );
        assert_eq!(charged, 2);
        assert_eq!(deadline_checks, 1);

        let mut remaining = 0;
        assert_eq!(
            super::interpreter::AcpiExecutor::normalize_crs_with_budget(
                "\\DEV0",
                vec![irq_descriptor(5)],
                || {
                    if remaining == 0 {
                        return Err(LoadError::BudgetExceeded);
                    }
                    remaining -= 1;
                    Ok(())
                },
                || Ok(()),
            )
            .map(|_| ()),
            Err(LoadError::BudgetExceeded)
        );
    }

    struct CountingRegionBackend(Arc<AtomicU64>);

    impl RegionAccessor for CountingRegionBackend {
        fn lease_generation(&self, _lease_id: u64) -> Result<u64, RegionError> {
            Err(RegionError::NoMatchingLease)
        }

        fn read(&self, _lease_id: u64, _address: u64, _width: u8) -> Result<u64, RegionError> {
            Err(RegionError::NoMatchingLease)
        }

        fn write(
            &self,
            _lease_id: u64,
            _address: u64,
            _width: u8,
            _value: u64,
        ) -> Result<(), RegionError> {
            Err(RegionError::NoMatchingLease)
        }
    }

    impl NativeRegionBackend for CountingRegionBackend {
        fn resource_generation(
            &self,
            _region: &OpRegion,
            _parent_device_path: &str,
            _namespace_generation: u64,
            _mode: AccessMode,
        ) -> Result<u64, RegionError> {
            Ok(1)
        }

        fn authorize(
            &self,
            _region: &OpRegion,
            _parent_device_path: &str,
            _namespace_generation: u64,
            _mode: AccessMode,
        ) -> Result<RegionAccessToken, RegionError> {
            self.0.fetch_add(1, Ordering::AcqRel);
            Err(RegionError::NoMatchingLease)
        }
    }

    fn append_aml_method(aml: &mut Vec<u8>, name: &[u8; 4], flags: u8, body: &[u8]) {
        aml.push(0x14);
        aml.extend_from_slice(&aml_pkg_length(4 + 1 + body.len()));
        aml.extend_from_slice(name);
        aml.push(flags);
        aml.extend_from_slice(body);
    }

    fn aml_pkg_length(contents_length: usize) -> Vec<u8> {
        let mut encoded_length_bytes = 1;
        let package_length = loop {
            let package_length = contents_length + encoded_length_bytes;
            let required_bytes = if package_length < 0x40 {
                1
            } else if package_length < 0x1000 {
                2
            } else {
                panic!("test AML package exceeds supported PkgLength encoding");
            };
            if required_bytes == encoded_length_bytes {
                break package_length;
            }
            encoded_length_bytes = required_bytes;
        };

        if encoded_length_bytes == 1 {
            vec![package_length as u8]
        } else {
            vec![
                0x40 | (package_length as u8 & 0x0f),
                (package_length >> 4) as u8,
            ]
        }
    }

    fn dsdt_with_s5(s5: Option<(u8, u8)>, extra_aml: &[u8]) -> Vec<u8> {
        let mut aml = extra_aml.to_vec();
        if let Some((sleep_type_a, sleep_type_b)) = s5 {
            let package_contents = [2, 0x0a, sleep_type_a, 0x0a, sleep_type_b];
            aml.extend_from_slice(&[0x08]); // NameOp
            aml.extend_from_slice(b"_S5_");
            aml.push(0x12); // PackageOp
            aml.extend_from_slice(&aml_pkg_length(package_contents.len()));
            aml.extend_from_slice(&package_contents);
        }

        let total_length = 36 + aml.len();
        let mut table = vec![0; 36];
        table[..4].copy_from_slice(b"DSDT");
        table[4..8].copy_from_slice(&(total_length as u32).to_le_bytes());
        table[8] = 2;
        table.extend_from_slice(&aml);
        table
    }

    fn dsdt_with_region_methods() -> Vec<u8> {
        let mut aml = vec![0x5b, 0x80]; // OpRegion
        aml.extend_from_slice(b"REG0");
        aml.extend_from_slice(&[1, 0, 1]); // SystemIO, ZeroOp base, OneOp length

        let field_list = [b'F', b'0', b'0', b'0', 8];
        let field_body_length = 4 + 1 + field_list.len();
        aml.extend_from_slice(&[0x5b, 0x81, (field_body_length + 1) as u8]);
        aml.extend_from_slice(b"REG0");
        aml.push(0); // AnyAcc, NoLock, Preserve
        aml.extend_from_slice(&field_list);

        append_aml_method(&mut aml, b"_REG", 2, &[0xa4, b'F', b'0', b'0', b'0']);

        let mut device_body = Vec::new();
        append_aml_method(
            &mut device_body,
            b"_STA",
            0,
            &[0xa4, b'F', b'0', b'0', b'0'],
        );
        let device_package_length = 1 + 4 + device_body.len();
        assert!(device_package_length < 0x40);
        aml.extend_from_slice(&[0x5b, 0x82, device_package_length as u8]);
        aml.extend_from_slice(b"DEV0");
        aml.extend_from_slice(&device_body);

        let total_length = 36 + aml.len();
        let mut table = vec![0; 36];
        table[..4].copy_from_slice(b"DSDT");
        table[4..8].copy_from_slice(&(total_length as u32).to_le_bytes());
        table[8] = 2;
        table.extend_from_slice(&aml);
        table
    }

    fn dsdt_with_field_units(names: &[[u8; 4]]) -> Vec<u8> {
        let mut aml = vec![0x5b, 0x80]; // OpRegion
        aml.extend_from_slice(b"REG0");
        aml.extend_from_slice(&[1, 0, 1]); // SystemIO, ZeroOp base, OneOp length

        let field_list_bytes = names.len() * 5;
        let field_body_bytes = 4 + 1 + field_list_bytes; // RegionName, FieldFlags, FieldList
        let package_length = field_body_bytes + 1; // one-byte PkgLength includes itself
        assert!(package_length < 0x40);
        aml.extend_from_slice(&[0x5b, 0x81, package_length as u8]); // Field
        aml.extend_from_slice(b"REG0");
        aml.push(0); // AnyAcc, NoLock, Preserve
        for name in names {
            aml.extend_from_slice(name);
            aml.push(8); // one-byte FieldUnit
        }

        let total_length = 36 + aml.len();
        let mut table = vec![0; 36];
        table[..4].copy_from_slice(b"DSDT");
        table[4..8].copy_from_slice(&(total_length as u32).to_le_bytes());
        table[8] = 2;
        table.extend_from_slice(&aml);
        table
    }

    fn dsdt_with_sta_body(body: &[u8]) -> Vec<u8> {
        let mut aml = Vec::new();
        let mut device_body = Vec::new();
        append_aml_method(&mut device_body, b"_STA", 0, body);
        let device_package_length = 1 + 4 + device_body.len();
        assert!(device_package_length < 0x40);
        aml.extend_from_slice(&[0x5b, 0x82, device_package_length as u8]);
        aml.extend_from_slice(b"DEV0");
        aml.extend_from_slice(&device_body);

        let total_length = 36 + aml.len();
        let mut table = vec![0; 36];
        table[..4].copy_from_slice(b"DSDT");
        table[4..8].copy_from_slice(&(total_length as u32).to_le_bytes());
        table[8] = 2;
        table.extend_from_slice(&aml);
        table
    }

    fn dsdt_with_multiple_sta_devices(device_count: usize, noop_count: usize) -> Vec<u8> {
        let mut aml = Vec::new();
        for index in 0..device_count {
            let mut device_body = Vec::new();
            let mut sta_body = vec![0xa3; noop_count]; // Noop
            sta_body.extend_from_slice(&[0xa4, 0x0a, 0x0f]); // Return(ByteConst(0x0f))
            append_aml_method(&mut device_body, b"_STA", 0, &sta_body);

            aml.extend_from_slice(&[0x5b, 0x82]);
            aml.extend_from_slice(&aml_pkg_length(4 + device_body.len()));
            let name = format!("D{index:03}");
            aml.extend_from_slice(name.as_bytes());
            aml.extend_from_slice(&device_body);
        }

        let total_length = 36 + aml.len();
        let mut table = vec![0; 36];
        table[..4].copy_from_slice(b"DSDT");
        table[4..8].copy_from_slice(&(total_length as u32).to_le_bytes());
        table[8] = 2;
        table.extend_from_slice(&aml);
        table
    }

    fn dsdt_with_multiple_pci_roots(noop_count: usize, integer_hids: bool) -> Vec<u8> {
        let bus_resource = [
            0x88, 0x0d, 0x00, 0x02, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00,
            0x00, 0x01, 0x79, 0x00,
        ];
        let buffer_package_length = 1 + 2 + bus_resource.len();
        assert!(buffer_package_length < 0x40);

        let mut aml = Vec::new();
        let device_count = if integer_hids { 3 } else { 2 };
        for index in 0..device_count {
            let mut device_body = Vec::new();
            device_body.extend_from_slice(&[0x08, b'_', b'H', b'I', b'D']);
            if index < 2 && integer_hids {
                device_body.extend_from_slice(&aml_integer(if index == 0 {
                    0x030a_d041
                } else {
                    0x080a_d041
                }));
            } else {
                device_body.push(0x0d);
                let identifier: &[u8] = if index < 2 {
                    b"PNP0A08\0"
                } else {
                    b"ACME0001\0"
                };
                device_body.extend_from_slice(identifier);
            }
            if index == 2 {
                device_body.extend_from_slice(&[0x08, b'_', b'A', b'D', b'R']);
                device_body.extend_from_slice(&aml_integer(0x0001_0000));
            }

            let mut crs_body = vec![0xa3; noop_count]; // Noop
            crs_body.extend_from_slice(&[
                0xa4,
                0x11,
                buffer_package_length as u8,
                0x0a,
                bus_resource.len() as u8,
            ]);
            crs_body.extend_from_slice(&bus_resource);
            append_aml_method(&mut device_body, b"_CRS", 0, &crs_body);

            aml.extend_from_slice(&[0x5b, 0x82]);
            aml.extend_from_slice(&aml_pkg_length(4 + device_body.len()));
            let name = format!("D{index:03}");
            aml.extend_from_slice(name.as_bytes());
            aml.extend_from_slice(&device_body);
        }

        let total_length = 36 + aml.len();
        let mut table = vec![0; 36];
        table[..4].copy_from_slice(b"DSDT");
        table[4..8].copy_from_slice(&(total_length as u32).to_le_bytes());
        table[8] = 2;
        table.extend_from_slice(&aml);
        table
    }

    fn aml_integer(value: usize) -> Vec<u8> {
        if value <= u8::MAX as usize {
            vec![0x0a, value as u8]
        } else if value <= u16::MAX as usize {
            let mut encoded = vec![0x0b];
            encoded.extend_from_slice(&(value as u16).to_le_bytes());
            encoded
        } else if value <= u32::MAX as usize {
            let mut encoded = vec![0x0c];
            encoded.extend_from_slice(&(value as u32).to_le_bytes());
            encoded
        } else {
            let mut encoded = vec![0x0e];
            encoded.extend_from_slice(&(value as u64).to_le_bytes());
            encoded
        }
    }

    fn dsdt_with_device_crs_resources(resources: &[u8]) -> Vec<u8> {
        let mut buffer = vec![0x11];
        let buffer_length = aml_integer(resources.len());
        buffer.extend_from_slice(&aml_pkg_length(buffer_length.len() + resources.len()));
        buffer.extend_from_slice(&buffer_length);
        buffer.extend_from_slice(resources);
        let mut device_body = Vec::new();
        let mut crs_body = vec![0xa4]; // Return(Buffer(...))
        crs_body.extend_from_slice(&buffer);
        append_aml_method(&mut device_body, b"_CRS", 0, &crs_body);

        let mut aml = vec![0x5b, 0x82]; // Device
        aml.extend_from_slice(&aml_pkg_length(4 + device_body.len()));
        aml.extend_from_slice(b"DEV0");
        aml.extend_from_slice(&device_body);

        let total_length = 36 + aml.len();
        let mut table = vec![0; 36];
        table[..4].copy_from_slice(b"DSDT");
        table[4..8].copy_from_slice(&(total_length as u32).to_le_bytes());
        table[8] = 2;
        table.extend_from_slice(&aml);
        table
    }

    fn dsdt_with_device_static_crs_resources(resources: &[u8]) -> Vec<u8> {
        let mut buffer = vec![0x11];
        let buffer_length = aml_integer(resources.len());
        buffer.extend_from_slice(&aml_pkg_length(buffer_length.len() + resources.len()));
        buffer.extend_from_slice(&buffer_length);
        buffer.extend_from_slice(resources);

        let mut device_body = vec![0x08]; // Name (_CRS, Buffer (...))
        device_body.extend_from_slice(b"_CRS");
        device_body.extend_from_slice(&buffer);

        let mut aml = vec![0x5b, 0x82]; // Device
        aml.extend_from_slice(&aml_pkg_length(4 + device_body.len()));
        aml.extend_from_slice(b"DEV0");
        aml.extend_from_slice(&device_body);

        let total_length = 36 + aml.len();
        let mut table = vec![0; 36];
        table[..4].copy_from_slice(b"DSDT");
        table[4..8].copy_from_slice(&(total_length as u32).to_le_bytes());
        table[8] = 2;
        table.extend_from_slice(&aml);
        table
    }

    fn dsdt_with_device_io_crs(minimum: u16, maximum: u16, length: u8) -> Vec<u8> {
        let mut resources = vec![0x47, 0x01]; // I/O Port descriptor, full-address decode
        resources.extend_from_slice(&minimum.to_le_bytes());
        resources.extend_from_slice(&maximum.to_le_bytes());
        resources.extend_from_slice(&[1, length]); // alignment, range length
        resources.extend_from_slice(&[0x79, 0]); // EndTag
        dsdt_with_device_crs_resources(&resources)
    }

    fn extended_io_range_descriptor(
        general_flags: u8,
        type_specific_flags: u8,
        minimum: u64,
        maximum: u64,
        translation_offset: u64,
        length: u64,
    ) -> Vec<u8> {
        let mut descriptor = vec![
            0x8b,
            0x35,
            0x00,
            1,
            general_flags,
            type_specific_flags,
            1,
            0,
        ];
        for field in [0, minimum, maximum, translation_offset, length, 0] {
            descriptor.extend_from_slice(&field.to_le_bytes());
        }
        descriptor
    }

    fn extended_memory_range_descriptor(
        general_flags: u8,
        type_specific_flags: u8,
        minimum: u64,
        maximum: u64,
        translation_offset: u64,
        length: u64,
    ) -> Vec<u8> {
        let mut descriptor = vec![
            0x8b,
            0x35,
            0x00,
            0,
            general_flags,
            type_specific_flags,
            1,
            0,
        ];
        for field in [0, minimum, maximum, translation_offset, length, 0] {
            descriptor.extend_from_slice(&field.to_le_bytes());
        }
        descriptor
    }

    fn word_io_range_descriptor(
        general_flags: u8,
        minimum: u16,
        maximum: u16,
        translation_offset: u16,
        length: u16,
    ) -> Vec<u8> {
        let mut descriptor = vec![0x88, 0x0d, 0x00, 1, general_flags, 0];
        for field in [0, minimum, maximum, translation_offset, length] {
            descriptor.extend_from_slice(&field.to_le_bytes());
        }
        descriptor
    }

    fn aml_table_with_nops(signature: &[u8; 4], aml_bytes: usize) -> Vec<u8> {
        let total_length = 36 + aml_bytes;
        let mut table = vec![0; total_length];
        table[..4].copy_from_slice(signature);
        table[4..8].copy_from_slice(&(total_length as u32).to_le_bytes());
        table[8] = 2;
        table[36..].fill(0xa3); // AML Noop opcode
        table
    }

    fn dsdt_with_pic_method() -> Vec<u8> {
        let mut aml = Vec::new();
        append_aml_method(&mut aml, b"_PIC", 1, &[0xa4, 0x00]);
        let total_length = 36 + aml.len();
        let mut table = vec![0; 36];
        table[..4].copy_from_slice(b"DSDT");
        table[4..8].copy_from_slice(&(total_length as u32).to_le_bytes());
        table[8] = 2;
        table.extend_from_slice(&aml);
        table
    }

    fn dsdt_with_gpe_methods(methods: &[(&[u8; 4], &[u8])]) -> Vec<u8> {
        let mut gpe_body = Vec::new();
        for (name, body) in methods {
            append_aml_method(&mut gpe_body, name, 0, body);
        }

        let scope_name = [0x5c, b'_', b'G', b'P', b'E'];
        let mut aml = vec![0x10]; // ScopeOp
        aml.extend_from_slice(&aml_pkg_length(scope_name.len() + gpe_body.len()));
        aml.extend_from_slice(&scope_name);
        aml.extend_from_slice(&gpe_body);

        let total_length = 36 + aml.len();
        let mut table = vec![0; 36];
        table[..4].copy_from_slice(b"DSDT");
        table[4..8].copy_from_slice(&(total_length as u32).to_le_bytes());
        table[8] = 2;
        table.extend_from_slice(&aml);
        table
    }

    fn dsdt_with_failing_pic_and_sta(reg_body: &[u8]) -> Vec<u8> {
        let mut aml = vec![0x5b, 0x80]; // OpRegion
        aml.extend_from_slice(b"REG0");
        aml.extend_from_slice(&[1, 0, 1]); // SystemIO, ZeroOp base, OneOp length
        append_aml_method(&mut aml, b"_REG", 2, reg_body);
        append_aml_method(&mut aml, b"_PIC", 1, &[0xa4, 0x78, 0x0a, 1, 0x0a, 0, 0, 0]);

        let mut device_body = Vec::new();
        append_aml_method(&mut device_body, b"_STA", 0, &[0xa4, 0x01]);
        let device_package_length = 1 + 4 + device_body.len();
        assert!(device_package_length < 0x40);
        aml.extend_from_slice(&[0x5b, 0x82, device_package_length as u8]);
        aml.extend_from_slice(b"DEV0");
        aml.extend_from_slice(&device_body);

        let total_length = 36 + aml.len();
        let mut table = vec![0; 36];
        table[..4].copy_from_slice(b"DSDT");
        table[4..8].copy_from_slice(&(total_length as u32).to_le_bytes());
        table[8] = 2;
        table.extend_from_slice(&aml);
        table
    }

    #[test]
    fn instance_generations_are_monotonic_and_restart_scoped() {
        let first = next_instance_generation(21).expect("first instance generation");
        let second = next_instance_generation(21).expect("second instance generation");
        let restarted = next_instance_generation(22).expect("restarted process generation");

        assert!(first < second);
        assert!(second < restarted);
    }

    #[test]
    fn gpe_dispatch_executes_the_exact_level_or_edge_method() {
        let dsdt = dsdt_with_gpe_methods(&[(b"_L2A", &[0xa4, 0x00]), (b"_E2B", &[0xa4, 0x00])]);
        let executor = load_executor(
            &dsdt,
            &[],
            CompatibilityProfile::default(),
            ExecutionBudget::offline_default(),
            FirmwareIdentity::new("01000000000000000200000000000000", 201),
        )
        .unwrap();
        executor.activate_native_regions().unwrap();

        assert_eq!(
            executor.dispatch_gpe(0x2a),
            Ok(GpeDispatchResult::Handled(GpeHandlerKind::Level))
        );
        assert_eq!(
            executor.dispatch_gpe(0x2b),
            Ok(GpeDispatchResult::Handled(GpeHandlerKind::Edge))
        );
        assert_eq!(
            executor.dispatch_gpe(0x2c),
            Ok(GpeDispatchResult::NoHandler)
        );
    }

    #[test]
    fn gpe_handler_resolution_is_separate_from_type_checked_execution() {
        let dsdt = dsdt_with_gpe_methods(&[(b"_L2A", &[0xa4, 0x00]), (b"_E2B", &[0xa4, 0x00])]);
        let executor = load_executor(
            &dsdt,
            &[],
            CompatibilityProfile::default(),
            ExecutionBudget::offline_default(),
            FirmwareIdentity::new("01000000000000000200000000000000", 203),
        )
        .unwrap();
        executor.activate_native_regions().unwrap();

        assert_eq!(
            executor.resolve_gpe_handler(0x2a),
            Ok(GpeDispatchResult::Handled(GpeHandlerKind::Level))
        );
        assert_eq!(
            executor.resolve_gpe_handler(0x2b),
            Ok(GpeDispatchResult::Handled(GpeHandlerKind::Edge))
        );
        assert_eq!(
            executor.execute_gpe_handler(0x2a, GpeHandlerKind::Edge),
            Err(LoadError::AmlRejected)
        );
        assert_eq!(
            executor.execute_gpe_handler(0x2a, GpeHandlerKind::Level),
            Ok(())
        );
    }

    #[test]
    fn gpe_methods_share_the_aggregate_sci_opcode_budget() {
        let body = [0xa3, 0xa3, 0xa3, 0xa4, 0x0a, 0x00];
        let dsdt = dsdt_with_gpe_methods(&[(b"_L2A", &body), (b"_L2B", &body)]);
        let budget = ExecutionBudget {
            max_operation_opcode_fuel: 9,
            max_method_duration_ns: 0,
            max_operation_duration_ns: 0,
            ..ExecutionBudget::offline_default()
        };
        let executor = load_executor(
            &dsdt,
            &[],
            CompatibilityProfile::default(),
            budget,
            FirmwareIdentity::new("01000000000000000200000000000000", 204),
        )
        .unwrap();
        executor.activate_native_regions().unwrap();

        let sci_work_unit = executor.begin_sci_work_unit().unwrap();
        assert_eq!(
            executor.execute_gpe_handler(0x2a, GpeHandlerKind::Level),
            Ok(())
        );
        assert_eq!(
            executor.execute_gpe_handler(0x2b, GpeHandlerKind::Level),
            Err(LoadError::BudgetExceeded)
        );
        sci_work_unit.finish().unwrap();

        assert_eq!(
            executor.execute_gpe_handler(0x2b, GpeHandlerKind::Level),
            Ok(())
        );
    }

    #[test]
    fn gpe_dispatch_rejects_out_of_range_and_ambiguous_handlers() {
        let dsdt = dsdt_with_gpe_methods(&[(b"_L2A", &[0xa4, 0x00]), (b"_E2A", &[0xa4, 0x00])]);
        let executor = load_executor(
            &dsdt,
            &[],
            CompatibilityProfile::default(),
            ExecutionBudget::offline_default(),
            FirmwareIdentity::new("01000000000000000200000000000000", 202),
        )
        .unwrap();
        executor.activate_native_regions().unwrap();

        assert_eq!(executor.dispatch_gpe(256), Err(LoadError::InvalidGpeNumber));
        assert_eq!(executor.dispatch_gpe(0x2a), Err(LoadError::AmlRejected));
    }

    #[test]
    fn instance_generation_rejects_invalid_process_identity() {
        assert_eq!(next_instance_generation(0), Err(LoadError::InvalidIdentity));
        assert_eq!(
            next_instance_generation(u64::from(u32::MAX) + 1),
            Err(LoadError::InvalidIdentity)
        );
        assert!(!FirmwareIdentity::new("01000000000000000200000000000000", 0).valid());
        assert!(
            !FirmwareIdentity::new("01000000000000000200000000000000", u64::from(u32::MAX) + 1)
                .valid()
        );
    }

    #[test]
    fn compatibility_profile_hash_is_stable_and_sensitive() {
        let profile = CompatibilityProfile::default();
        assert_eq!(profile.stable_hash(), profile.stable_hash());
        assert_ne!(
            profile.stable_hash(),
            CompatibilityProfile {
                os_name: "Linux",
                osi_linux: true,
                ..profile
            }
            .stable_hash()
        );
        assert_ne!(
            profile.stable_hash(),
            CompatibilityProfile {
                os_revision: profile.os_revision | (1_u64 << 48),
                ..profile
            }
            .stable_hash()
        );
    }

    #[test]
    fn execution_state_enforces_each_resource_dimension() {
        let budget = ExecutionBudget {
            opcode_fuel: 2,
            max_call_depth: 1,
            max_loop_iterations: 1,
            max_namespace_objects: 1,
            max_package_bytes: 4,
            max_buffer_bytes: 4,
            max_heap_bytes: 8,
            ..ExecutionBudget::offline_default()
        };
        let mut state = ExecutionState::new(budget).unwrap();
        state.charge_opcode(2).unwrap();
        assert_eq!(state.charge_opcode(1), Err(LoadError::BudgetExceeded));
        state.enter_call().unwrap();
        assert_eq!(state.enter_call(), Err(LoadError::BudgetExceeded));
        state.leave_call();
        state.loop_iteration().unwrap();
        assert_eq!(state.loop_iteration(), Err(LoadError::BudgetExceeded));
        state.add_namespace_objects(1).unwrap();
        assert_eq!(
            state.add_namespace_objects(1),
            Err(LoadError::BudgetExceeded)
        );
        state.reserve_package(4).unwrap();
        state.reserve_buffer(4).unwrap();
        assert_eq!(state.reserve_heap(1), Err(LoadError::BudgetExceeded));
    }

    #[test]
    fn failed_budget_reservations_do_not_consume_other_dimensions() {
        let namespace_budget = ExecutionBudget {
            max_namespace_objects: 2,
            ..ExecutionBudget::offline_default()
        };
        let mut namespace_state = ExecutionState::new(namespace_budget).unwrap();
        namespace_state.add_namespace_objects(1).unwrap();
        assert_eq!(
            namespace_state.add_namespace_objects(2),
            Err(LoadError::BudgetExceeded)
        );
        namespace_state.add_namespace_objects(1).unwrap();
        assert_eq!(namespace_state.namespace_objects, 2);

        let package_heap_budget = ExecutionBudget {
            max_package_bytes: 100,
            max_heap_bytes: 10,
            ..ExecutionBudget::offline_default()
        };
        let mut package_heap_state = ExecutionState::new(package_heap_budget).unwrap();
        package_heap_state.reserve_package(8).unwrap();
        assert_eq!(
            package_heap_state.reserve_package(4),
            Err(LoadError::BudgetExceeded)
        );
        assert_eq!(package_heap_state.package_bytes, 8);
        assert_eq!(package_heap_state.heap_bytes, 8);
        package_heap_state.reserve_package(2).unwrap();

        let buffer_heap_budget = ExecutionBudget {
            max_buffer_bytes: 100,
            max_heap_bytes: 10,
            ..ExecutionBudget::offline_default()
        };
        let mut buffer_heap_state = ExecutionState::new(buffer_heap_budget).unwrap();
        buffer_heap_state.reserve_buffer(8).unwrap();
        assert_eq!(
            buffer_heap_state.reserve_buffer(4),
            Err(LoadError::BudgetExceeded)
        );
        assert_eq!(buffer_heap_state.buffer_bytes, 8);
        assert_eq!(buffer_heap_state.heap_bytes, 8);
        buffer_heap_state.reserve_buffer(2).unwrap();

        let package_limit_budget = ExecutionBudget {
            max_package_bytes: 10,
            max_heap_bytes: 100,
            ..ExecutionBudget::offline_default()
        };
        let mut package_limit_state = ExecutionState::new(package_limit_budget).unwrap();
        package_limit_state.reserve_package(8).unwrap();
        assert_eq!(
            package_limit_state.reserve_package(4),
            Err(LoadError::BudgetExceeded)
        );
        assert_eq!(package_limit_state.package_bytes, 8);
        package_limit_state.reserve_package(2).unwrap();

        let buffer_limit_budget = ExecutionBudget {
            max_buffer_bytes: 10,
            max_heap_bytes: 100,
            ..ExecutionBudget::offline_default()
        };
        let mut buffer_limit_state = ExecutionState::new(buffer_limit_budget).unwrap();
        buffer_limit_state.reserve_buffer(8).unwrap();
        assert_eq!(
            buffer_limit_state.reserve_buffer(4),
            Err(LoadError::BudgetExceeded)
        );
        assert_eq!(buffer_limit_state.buffer_bytes, 8);
        buffer_limit_state.reserve_buffer(2).unwrap();
    }

    #[test]
    fn package_and_buffer_limits_remain_charged_for_persistent_objects() {
        let budget = ExecutionBudget {
            max_package_bytes: 4,
            max_buffer_bytes: 4,
            max_heap_bytes: 16,
            max_method_duration_ns: 0,
            max_operation_duration_ns: 0,
            ..ExecutionBudget::offline_default()
        };
        let mut state = ExecutionState::new(budget).unwrap();

        state.begin_work_unit_at(0).unwrap();
        state.reserve_package(4).unwrap();
        state.reserve_buffer(4).unwrap();
        state.end_work_unit();
        assert_eq!(state.package_bytes, 4);
        assert_eq!(state.buffer_bytes, 4);
        assert_eq!(state.heap_bytes, 8);

        state.begin_work_unit_at(0).unwrap();
        assert_eq!(state.reserve_package(1), Err(LoadError::BudgetExceeded));
        assert_eq!(state.reserve_buffer(1), Err(LoadError::BudgetExceeded));
        assert_eq!(state.package_bytes, 4);
        assert_eq!(state.buffer_bytes, 4);
        state.end_work_unit();
    }

    #[test]
    fn failed_buffer_materialization_can_rollback_its_reservation() {
        let budget = ExecutionBudget {
            max_buffer_bytes: 8,
            max_heap_bytes: 16,
            max_method_duration_ns: 0,
            max_operation_duration_ns: 0,
            ..ExecutionBudget::offline_default()
        };
        let mut state = ExecutionState::new(budget).unwrap();
        state.reserve_heap(4).unwrap();
        state.reserve_buffer(8).unwrap();
        assert_eq!((state.buffer_bytes, state.heap_bytes), (8, 12));

        state.rollback_buffer_reservation(8);
        assert_eq!((state.buffer_bytes, state.heap_bytes), (0, 4));
        state.reserve_buffer(8).unwrap();
    }

    #[test]
    fn working_heap_reservations_share_the_limit_and_reset_per_work_unit() {
        let budget = ExecutionBudget {
            max_heap_bytes: 100,
            max_method_duration_ns: 0,
            max_operation_duration_ns: 0,
            ..ExecutionBudget::offline_default()
        };
        let mut state = ExecutionState::new(budget).unwrap();
        state.reserve_heap(60).unwrap();

        state.begin_work_unit_at(0).unwrap();
        state.reserve_working_heap(40).unwrap();
        assert_eq!(state.reserve_heap(1), Err(LoadError::BudgetExceeded));
        assert_eq!(
            state.reserve_working_heap(1),
            Err(LoadError::BudgetExceeded)
        );
        state.release_working_heap(40);
        state.reserve_working_heap(40).unwrap();
        state.release_working_heap(40);
        state.end_work_unit();
        assert_eq!(state.working_heap_bytes, 0);
        assert_eq!(state.stale_working_heap_bytes, 0);

        state.begin_work_unit_at(0).unwrap();
        state.reserve_working_heap(40).unwrap();
        state.release_working_heap(40);
        state.end_work_unit();
    }

    #[test]
    fn stale_work_unit_reservation_cannot_release_next_work_unit_heap() {
        let budget = ExecutionBudget {
            max_heap_bytes: 100,
            max_method_duration_ns: 0,
            max_operation_duration_ns: 0,
            ..ExecutionBudget::offline_default()
        };
        let mut state = ExecutionState::new(budget).unwrap();

        state.begin_work_unit_at(0).unwrap();
        let stale_generation = state.work_unit_generation;
        state.reserve_working_heap(40).unwrap();
        state.end_work_unit();

        state.begin_work_unit_at(0).unwrap();
        let active_generation = state.work_unit_generation;
        assert_ne!(stale_generation, active_generation);
        state.reserve_working_heap(40).unwrap();

        state.release_working_heap_for_generation(40, stale_generation);
        assert_eq!(state.stale_working_heap_bytes, 0);
        assert_eq!(
            state.commit_working_heap_for_generation(40, stale_generation),
            Err(LoadError::InvalidProfile)
        );
        assert_eq!(state.working_heap_bytes, 40);
        assert_eq!(state.heap_bytes, 0);
        assert_eq!(
            state.reserve_working_heap(61),
            Err(LoadError::BudgetExceeded)
        );

        state.release_working_heap_for_generation(40, active_generation);
        assert_eq!(state.working_heap_bytes, 0);
        state.end_work_unit();
    }

    #[test]
    fn outstanding_working_heap_remains_in_the_total_after_its_work_unit_ends() {
        let budget = ExecutionBudget {
            max_heap_bytes: 100,
            max_method_duration_ns: 0,
            max_operation_duration_ns: 0,
            ..ExecutionBudget::offline_default()
        };
        let mut state = ExecutionState::new(budget).unwrap();

        state.begin_work_unit_at(0).unwrap();
        let stale_generation = state.work_unit_generation;
        state.reserve_working_heap(70).unwrap();
        state.end_work_unit();
        assert_eq!(state.stale_working_heap_bytes, 70);

        state.begin_work_unit_at(0).unwrap();
        assert_eq!(
            state.reserve_working_heap(31),
            Err(LoadError::BudgetExceeded)
        );
        state.release_working_heap_for_generation(70, stale_generation);
        assert_eq!(state.stale_working_heap_bytes, 0);
        state.reserve_working_heap(100).unwrap();
        let active_generation = state.work_unit_generation;
        state.release_working_heap_for_generation(100, active_generation);
        state.end_work_unit();
    }

    #[test]
    fn retained_working_heap_can_be_committed_as_persistent_heap() {
        let budget = ExecutionBudget {
            max_heap_bytes: 100,
            max_method_duration_ns: 0,
            max_operation_duration_ns: 0,
            ..ExecutionBudget::offline_default()
        };
        let mut state = ExecutionState::new(budget).unwrap();
        state.reserve_heap(60).unwrap();
        state.begin_work_unit_at(0).unwrap();
        state.reserve_working_heap(40).unwrap();

        state.commit_working_heap(40).unwrap();
        state.end_work_unit();

        assert_eq!(state.reserve_heap(1), Err(LoadError::BudgetExceeded));
    }

    #[test]
    fn dynamic_object_allocation_tracks_heap_package_and_buffer_lifetimes() {
        let budget = ExecutionBudget {
            max_package_bytes: 20,
            max_buffer_bytes: 20,
            max_heap_bytes: 100,
            max_method_duration_ns: 0,
            max_operation_duration_ns: 0,
            ..ExecutionBudget::offline_default()
        };
        let mut state = ExecutionState::new(budget).unwrap();

        state.begin_work_unit_at(0).unwrap();
        let stale_generation = state.work_unit_generation;
        assert_eq!(
            state.reserve_working_allocation(50, 21, 0),
            Err(LoadError::BudgetExceeded)
        );
        assert_eq!(
            (
                state.working_heap_bytes,
                state.working_package_bytes,
                state.working_buffer_bytes,
            ),
            (0, 0, 0)
        );
        state.reserve_working_allocation(50, 20, 20).unwrap();
        state.end_work_unit();
        assert_eq!(
            (
                state.stale_working_heap_bytes,
                state.stale_working_package_bytes,
                state.stale_working_buffer_bytes,
            ),
            (50, 20, 20)
        );

        state.begin_work_unit_at(0).unwrap();
        assert_eq!(
            state.reserve_working_allocation(51, 0, 0),
            Err(LoadError::BudgetExceeded)
        );
        state.release_working_allocation_for_generation(50, 20, 20, stale_generation);
        assert_eq!(
            (
                state.stale_working_heap_bytes,
                state.stale_working_package_bytes,
                state.stale_working_buffer_bytes,
            ),
            (0, 0, 0)
        );

        let active_generation = state.work_unit_generation;
        state.reserve_working_allocation(60, 12, 8).unwrap();
        state
            .commit_working_allocation_for_generation(60, 12, 8, active_generation)
            .unwrap();
        assert_eq!(
            (state.heap_bytes, state.package_bytes, state.buffer_bytes),
            (60, 12, 8)
        );
        state.end_work_unit();
        state.release_persistent_allocation(60, 12, 8).unwrap();
        assert_eq!(
            (state.heap_bytes, state.package_bytes, state.buffer_bytes),
            (0, 0, 0)
        );
    }

    #[test]
    fn released_persistent_heap_restores_the_operation_budget() {
        let budget = ExecutionBudget {
            max_heap_bytes: 100,
            max_method_duration_ns: 0,
            max_operation_duration_ns: 0,
            ..ExecutionBudget::offline_default()
        };
        let mut state = ExecutionState::new(budget).unwrap();
        state.reserve_heap(60).unwrap();
        state.release_heap(60).unwrap();

        assert_eq!(state.heap_bytes, 0);
        state.reserve_heap(100).unwrap();
    }

    #[test]
    fn each_top_level_method_receives_fresh_opcode_and_loop_fuel() {
        let budget = ExecutionBudget {
            opcode_fuel: 3,
            max_loop_iterations: 1,
            max_method_duration_ns: 0,
            ..ExecutionBudget::offline_default()
        };
        let mut state = ExecutionState::new(budget).unwrap();
        state.charge_opcode(3).unwrap(); // table parsing consumes the initial load budget
        state.enter_call_at(0).unwrap();
        assert_eq!(state.opcode_fuel_remaining(), 3);
        state.charge_opcode(2).unwrap();
        state.loop_iteration().unwrap();

        state.enter_call_at(0).unwrap(); // nested methods share the outer operation budget
        assert_eq!(state.opcode_fuel_remaining(), 1);
        assert_eq!(state.loop_iteration(), Err(LoadError::BudgetExceeded));
        state.leave_call();
        state.leave_call();

        state.enter_call_at(0).unwrap(); // a later top-level method gets a new budget
        assert_eq!(state.opcode_fuel_remaining(), 3);
        assert_eq!(state.loop_iteration(), Ok(()));
    }

    #[test]
    fn work_unit_budget_accumulates_across_top_level_methods() {
        let budget = ExecutionBudget {
            opcode_fuel: 4,
            max_loop_iterations: 4,
            max_operation_opcode_fuel: 3,
            max_operation_loop_iterations: 2,
            max_method_duration_ns: 0,
            max_operation_duration_ns: 0,
            ..ExecutionBudget::offline_default()
        };
        let mut state = ExecutionState::new(budget).unwrap();
        state.begin_work_unit_at(0).unwrap();

        state.enter_call_at(0).unwrap();
        state.charge_opcode(2).unwrap();
        state.loop_iteration().unwrap();
        state.leave_call();

        state.enter_call_at(0).unwrap();
        state.charge_opcode(1).unwrap();
        state.loop_iteration().unwrap();
        assert_eq!(state.charge_opcode(1), Err(LoadError::BudgetExceeded));
        assert_eq!(state.loop_iteration(), Err(LoadError::BudgetExceeded));
        state.leave_call();
        state.end_work_unit();

        // A new work unit receives a fresh aggregate budget.
        state.begin_work_unit_at(0).unwrap();
        state.enter_call_at(0).unwrap();
        state.charge_opcode(3).unwrap();
    }

    #[test]
    fn device_snapshot_enforces_one_operation_opcode_budget_across_devices() {
        let budget = ExecutionBudget {
            opcode_fuel: 128,
            max_operation_opcode_fuel: 40,
            max_method_duration_ns: 0,
            max_operation_duration_ns: 0,
            ..ExecutionBudget::offline_default()
        };
        let executor = load_executor(
            &dsdt_with_multiple_sta_devices(4, 12),
            &[],
            CompatibilityProfile::default(),
            budget,
            FirmwareIdentity::new("01000000000000000200000000000000", 102),
        )
        .unwrap();
        executor.activate_native_regions().unwrap();

        let policy = DeviceSnapshotPolicy {
            evaluate_ini: false,
            evaluate_identity: false,
            evaluate_crs: false,
            evaluate_prt: false,
            ..DeviceSnapshotPolicy::offline_default()
        };
        assert_eq!(
            executor.snapshot_devices(4, policy).err(),
            Some(LoadError::BudgetExceeded)
        );
    }

    #[test]
    fn device_snapshot_rejects_candidates_beyond_the_requested_limit() {
        let executor = load_executor(
            &dsdt_with_multiple_pci_roots(0, false),
            &[],
            CompatibilityProfile::default(),
            ExecutionBudget::offline_default(),
            FirmwareIdentity::new("01000000000000000200000000000000", 106),
        )
        .unwrap();
        executor.activate_native_regions().unwrap();

        let policy = DeviceSnapshotPolicy {
            evaluate_ini: false,
            evaluate_identity: false,
            evaluate_crs: false,
            evaluate_prt: false,
            ..DeviceSnapshotPolicy::offline_default()
        };
        assert_eq!(
            executor.snapshot_devices(1, policy).err(),
            Some(LoadError::BudgetExceeded)
        );
    }

    #[test]
    fn pci_root_discovery_propagates_work_unit_fuel_exhaustion() {
        let budget = ExecutionBudget {
            opcode_fuel: 64,
            max_operation_opcode_fuel: 16,
            max_method_duration_ns: 0,
            max_operation_duration_ns: 0,
            ..ExecutionBudget::offline_default()
        };
        let executor = load_executor(
            &dsdt_with_multiple_pci_roots(12, false),
            &[],
            CompatibilityProfile::default(),
            budget,
            FirmwareIdentity::new("01000000000000000200000000000000", 103),
        )
        .unwrap();
        executor.activate_native_regions().unwrap();

        assert_eq!(executor.get_pci_roots(), Err(LoadError::BudgetExceeded));
    }

    #[test]
    fn pci_root_discovery_decodes_integer_eisa_hids_and_skips_bridge_crs() {
        let executor = load_executor(
            &dsdt_with_multiple_pci_roots(0, true),
            &[],
            CompatibilityProfile::default(),
            ExecutionBudget::offline_default(),
            FirmwareIdentity::new("01000000000000000200000000000000", 104),
        )
        .unwrap();
        executor.activate_native_regions().unwrap();

        let discovery = executor.get_pci_roots().unwrap();
        assert_eq!(discovery.hid_matches, 2);
        assert_eq!(discovery.bus_range_matches, 2);
        assert_eq!(discovery.roots.len(), 2);
        assert_eq!(discovery.roots[0].namespace_path, "\\D000");
        assert_eq!(discovery.roots[1].namespace_path, "\\D001");
    }

    #[test]
    fn preflights_fixed_system_io_crs_before_native_region_activation() {
        let executor = load_executor(
            &dsdt_with_device_io_crs(0x03f8, 0x03f8, 8),
            &[],
            CompatibilityProfile::default(),
            ExecutionBudget::offline_default(),
            FirmwareIdentity::new("01000000000000000200000000000000", 104),
        )
        .unwrap();

        assert_eq!(
            executor.preflight_system_io_resources().unwrap(),
            vec![crate::region::SystemIoResource {
                parent_device_path: "\\DEV0".to_string(),
                base: 0x03f8,
                length: 8,
            }]
        );
        assert_eq!(executor.activate_native_regions(), Ok(()));
        assert_eq!(
            executor.preflight_system_io_resources(),
            Err(LoadError::InvalidProfile)
        );
    }

    #[test]
    fn system_io_preflight_ignores_flexible_crs_port_ranges() {
        let executor = load_executor(
            &dsdt_with_device_io_crs(0x03f8, 0x0400, 8),
            &[],
            CompatibilityProfile::default(),
            ExecutionBudget::offline_default(),
            FirmwareIdentity::new("01000000000000000200000000000000", 105),
        )
        .unwrap();

        assert!(executor.preflight_system_io_resources().unwrap().is_empty());
    }

    #[test]
    fn crs_parser_working_set_is_reserved_before_expansion() {
        let dsdt = dsdt_with_device_crs_resources(&[0x22, 0x01, 0x00, 0x79, 0x00]);
        let budget = ExecutionBudget {
            max_heap_bytes: 8 << 10,
            max_method_duration_ns: 0,
            max_operation_duration_ns: 0,
            ..ExecutionBudget::offline_default()
        };
        let executor = load_executor(
            &dsdt,
            &[],
            CompatibilityProfile::default(),
            budget,
            FirmwareIdentity::new("01000000000000000200000000000000", 407),
        )
        .unwrap();

        assert_eq!(
            executor.preflight_system_io_resources(),
            Err(LoadError::BudgetExceeded)
        );
    }

    #[test]
    fn crs_parser_charges_unsupported_descriptors_to_the_work_unit_loop_budget() {
        const UNSUPPORTED_DESCRIPTOR_COUNT: usize = 1024;
        let mut descriptor = Vec::new();
        descriptor
            .try_reserve_exact(UNSUPPORTED_DESCRIPTOR_COUNT * 2 + 2)
            .unwrap();
        for _ in 0..UNSUPPORTED_DESCRIPTOR_COUNT {
            descriptor.extend_from_slice(&[0x71, 0x00]);
        }
        descriptor.extend_from_slice(&[0x79, 0x00]);

        let mut budget = ExecutionBudget::offline_default();
        budget.max_loop_iterations = 4096;
        budget.max_operation_loop_iterations = 128;
        budget.max_method_duration_ns = 0;
        budget.max_operation_duration_ns = 0;
        let executor = load_executor(
            &dsdt_with_device_static_crs_resources(&descriptor),
            &[],
            CompatibilityProfile::default(),
            budget,
            FirmwareIdentity::new("01000000000000000200000000000000", 408),
        )
        .unwrap();

        assert_eq!(
            executor.preflight_system_io_resources(),
            Err(LoadError::BudgetExceeded)
        );
    }

    #[test]
    fn system_io_preflight_accepts_only_extended_consumer_io_range_descriptors() {
        let cases = [
            (
                extended_io_range_descriptor(0x0d, 0x03, 0x03f8, 0x03ff, 0, 8),
                vec![crate::region::SystemIoResource {
                    parent_device_path: "\\DEV0".to_string(),
                    base: 0x03f8,
                    length: 8,
                }],
            ),
            (
                extended_io_range_descriptor(0x0c, 0x03, 0x02f8, 0x02f8, 0, 1),
                vec![],
            ),
            (
                extended_io_range_descriptor(0x01, 0x03, 0x0378, 0x0378, 0, 1),
                vec![],
            ),
            (
                extended_io_range_descriptor(0x0d, 0x13, 0x04e8, 0x04e8, 0, 1),
                vec![],
            ),
            (
                extended_io_range_descriptor(0x0d, 0x03, 0x04e8, 0x04e8, 1, 1),
                vec![],
            ),
            // Bit 0 is ignored for WORD descriptors, so it cannot establish a consumer resource.
            (word_io_range_descriptor(0x0c, 0x0378, 0x0378, 0, 1), vec![]),
        ];

        for (index, (mut descriptor, expected)) in cases.into_iter().enumerate() {
            descriptor.extend_from_slice(&[0x79, 0]);
            let executor = load_executor(
                &dsdt_with_device_crs_resources(&descriptor),
                &[],
                CompatibilityProfile::default(),
                ExecutionBudget::offline_default(),
                FirmwareIdentity::new("01000000000000000200000000000000", 106 + index as u64),
            )
            .unwrap();

            assert_eq!(executor.preflight_system_io_resources().unwrap(), expected);
        }
    }

    #[test]
    fn device_snapshot_preserves_full_address_space_descriptors() {
        let mut descriptor =
            extended_memory_range_descriptor(0x0c, 0x06, 0x8000_0000, 0x80ff_ffff, 0, 0x0100_0000);
        descriptor.extend_from_slice(&[0x79, 0]);
        let executor = load_executor(
            &dsdt_with_device_crs_resources(&descriptor),
            &[],
            CompatibilityProfile::default(),
            ExecutionBudget::offline_default(),
            FirmwareIdentity::new("01000000000000000200000000000000", 304),
        )
        .unwrap();
        executor.activate_native_regions().unwrap();

        let devices = executor
            .snapshot_devices(1, DeviceSnapshotPolicy::offline_default())
            .unwrap();
        assert_eq!(devices.len(), 1);
        assert_eq!(devices[0].address_spaces.len(), 1);
        assert_eq!(devices[0].address_spaces[0].resource_type, 0);
        assert_eq!(devices[0].address_spaces[0].descriptor_revision, 1);
        assert_eq!(devices[0].address_spaces[0].general_flags, 0x0c);
        assert_eq!(devices[0].address_spaces[0].type_specific_flags, 0x06);
        assert_eq!(devices[0].address_spaces[0].minimum, 0x8000_0000);
        assert_eq!(devices[0].address_spaces[0].maximum, 0x80ff_ffff);
        assert_eq!(devices[0].address_spaces[0].length, 0x0100_0000);

        let snapshot_bytes = devices[0].path.len()
            + 32
            + devices[0].resources.len() * 32
            + devices[0].address_spaces.len() * 64;
        let bounded_policy = DeviceSnapshotPolicy {
            max_snapshot_bytes: snapshot_bytes - 1,
            ..DeviceSnapshotPolicy::offline_default()
        };
        assert_eq!(
            executor.snapshot_devices(1, bounded_policy),
            Err(LoadError::BudgetExceeded)
        );
    }

    #[test]
    fn device_snapshot_size_budget_accounts_for_pci_routing_entries() {
        let route = PciRoutingEntry {
            address: 0x0002_ffff,
            pin: 0,
            source: PciRoutingSource::DirectGsi(23),
        };
        let without_routes = snapshot_entry_size_bytes("\\_SB_.PCI0", &[], &[], &[], &[])
            .expect("empty snapshot entry size should not overflow");
        let with_route = snapshot_entry_size_bytes("\\_SB_.PCI0", &[], &[], &[], &[route])
            .expect("one route should fit in the snapshot entry size");

        assert_eq!(with_route - without_routes, 32);
    }

    #[test]
    fn snapshot_working_heap_estimate_covers_limits_and_overflow() {
        let one_device = snapshot_working_heap_bytes(1, 1024).unwrap();
        let many_devices = snapshot_working_heap_bytes(256, 1024).unwrap();
        let large_snapshot = snapshot_working_heap_bytes(1, 64 * 1024).unwrap();

        assert!(many_devices > one_device);
        assert!(large_snapshot > one_device);
        assert_eq!(snapshot_working_heap_bytes(usize::MAX, 64 * 1024), None);
        assert_eq!(snapshot_working_heap_bytes(1, usize::MAX), None);
    }

    #[test]
    fn native_region_registration_heap_estimate_covers_persistent_and_callback_storage() {
        let (empty_total, empty_persistent) =
            acpi::aml::native_region_registration_heap_bytes(0, 0, 0).unwrap();
        let (one_total, one_persistent) =
            acpi::aml::native_region_registration_heap_bytes(1, 64, 32).unwrap();
        let (two_total, two_persistent) =
            acpi::aml::native_region_registration_heap_bytes(2, 128, 64).unwrap();

        assert!(empty_total > empty_persistent);
        assert!(one_persistent > empty_persistent);
        assert!(two_persistent > one_persistent);
        assert!(two_total > one_total);
        assert_eq!(
            acpi::aml::native_region_registration_heap_bytes(usize::MAX, 0, 0),
            None
        );
        assert_eq!(
            acpi::aml::native_region_registration_heap_bytes(1, u64::MAX, 0),
            None
        );

        let budget = ExecutionBudget {
            max_heap_bytes: one_total - 1,
            ..ExecutionBudget::offline_default()
        };
        let mut state = ExecutionState::new(budget).unwrap();
        state.begin_work_unit_at(0).unwrap();
        assert_eq!(
            state.reserve_working_heap(one_total),
            Err(LoadError::BudgetExceeded)
        );
        assert_eq!(state.working_heap_bytes, 0);
    }

    #[test]
    fn work_unit_deadline_spans_multiple_top_level_methods() {
        let budget = ExecutionBudget {
            max_method_duration_ns: 0,
            max_operation_duration_ns: 20,
            ..ExecutionBudget::offline_default()
        };
        let mut state = ExecutionState::new(budget).unwrap();
        state.begin_work_unit_at(100).unwrap();
        state.enter_call_at(105).unwrap();
        state.leave_call();
        state.enter_call_at(115).unwrap();
        assert_eq!(state.check_deadline(119), Ok(()));
        assert_eq!(state.check_deadline(120), Err(LoadError::DeadlineExceeded));
        state.leave_call();
        state.end_work_unit();
        assert!(state.deadline_failed());
    }

    #[test]
    fn namespace_object_budget_counts_actual_insertions_and_heap_cost() {
        let budget = ExecutionBudget {
            max_namespace_objects: 2,
            max_heap_bytes: 300,
            ..ExecutionBudget::offline_default()
        };
        let mut state = ExecutionState::new(budget).unwrap();
        state.reserve_namespace_object(100).unwrap();
        state.reserve_namespace_object(100).unwrap();
        assert_eq!(
            state.reserve_namespace_object(1),
            Err(LoadError::BudgetExceeded)
        );

        let heap_limited = ExecutionBudget {
            max_namespace_objects: 8,
            max_heap_bytes: 199,
            ..ExecutionBudget::offline_default()
        };
        let mut state = ExecutionState::new(heap_limited).unwrap();
        state.reserve_namespace_object(100).unwrap();
        assert_eq!(
            state.reserve_namespace_object(100),
            Err(LoadError::BudgetExceeded)
        );
        // A failed combined reservation must not consume either dimension.
        state.reserve_namespace_object(99).unwrap();

        let mut releasable = ExecutionState::new(ExecutionBudget::offline_default()).unwrap();
        releasable.reserve_namespace_object(100).unwrap();
        releasable.release_namespace_object(100).unwrap();
        assert_eq!(releasable.namespace_objects, 0);
        assert_eq!(releasable.heap_bytes, 0);
        assert_eq!(
            releasable.release_namespace_object(100),
            Err(LoadError::InvalidProfile)
        );
        releasable.reserve_namespace_object(100).unwrap();
    }

    fn minimum_heap_budget_for_load(table: &[u8], serial: u64) -> u64 {
        fn succeeds(table: &[u8], serial: u64, max_heap_bytes: u64) -> bool {
            load_executor(
                table,
                &[],
                CompatibilityProfile::default(),
                ExecutionBudget {
                    max_heap_bytes,
                    ..ExecutionBudget::offline_default()
                },
                FirmwareIdentity::new("01000000000000000200000000000000", serial),
            )
            .is_ok()
        }

        let mut lower = 0_u64;
        let mut upper = 1 << 16;
        assert!(
            succeeds(table, serial, upper),
            "probe ceiling must admit valid AML"
        );
        while lower + 1 < upper {
            let midpoint = lower + (upper - lower) / 2;
            if succeeds(table, serial, midpoint) {
                upper = midpoint;
            } else {
                lower = midpoint;
            }
        }
        upper
    }

    #[test]
    fn aml_string_literal_payload_is_charged_to_heap_budget() {
        let mut aml = vec![0x08]; // NameOp
        aml.extend_from_slice(b"STR0");
        aml.push(0x0d); // StringPrefix
        aml.extend(std::iter::repeat_n(b'A', 512));
        aml.push(0);
        let table = dsdt_with_s5(None, &aml);
        let empty_table = dsdt_with_s5(None, &[]);
        let empty_heap = minimum_heap_budget_for_load(&empty_table, 264);
        let string_heap = minimum_heap_budget_for_load(&table, 263);

        assert!(string_heap >= empty_heap + 512);
    }

    #[test]
    fn package_namepath_component_storage_is_charged_to_persistent_heap() {
        let mut aml = vec![0x08]; // NameOp
        aml.extend_from_slice(b"PKG0");
        aml.push(0x12); // PackageOp
        aml.extend_from_slice(&aml_pkg_length(5));
        aml.push(1); // one element
        aml.extend_from_slice(b"ABCD"); // unresolved package NameString
        let dsdt = dsdt_with_s5(None, &aml);
        let mut integer_aml = vec![0x08]; // NameOp
        integer_aml.extend_from_slice(b"PKG0");
        integer_aml.push(0x12); // PackageOp
        integer_aml.extend_from_slice(&aml_pkg_length(2));
        integer_aml.extend_from_slice(&[1, 0]); // one integer element
        let integer_dsdt = dsdt_with_s5(None, &integer_aml);

        let integer_heap = minimum_heap_budget_for_load(&integer_dsdt, 271);
        let namepath_heap = minimum_heap_budget_for_load(&dsdt, 272);

        assert!(namepath_heap >= integer_heap + 64);
    }

    #[test]
    fn varpackage_argument_scratch_is_bounded_before_vector_growth() {
        let budget = ExecutionBudget {
            max_heap_bytes: 8 * 1024,
            ..ExecutionBudget::offline_default()
        };
        let mut valid_aml = vec![0x08]; // NameOp
        valid_aml.extend_from_slice(b"PKG0");
        valid_aml.push(0x13); // VarPackage with one ZeroOp element
        valid_aml.extend_from_slice(&aml_pkg_length(3));
        valid_aml.extend_from_slice(&[0x0a, 1, 0]);
        assert!(
            load_executor(
                &dsdt_with_s5(None, &valid_aml),
                &[],
                CompatibilityProfile::default(),
                budget,
                FirmwareIdentity::new("01000000000000000200000000000000", 265),
            )
            .is_ok()
        );

        let mut oversized_aml = vec![0x08]; // NameOp
        oversized_aml.extend_from_slice(b"PKG0");
        oversized_aml.push(0x13); // VarPackage declares one element but encodes many
        oversized_aml.extend_from_slice(&aml_pkg_length(2 + 512));
        oversized_aml.extend_from_slice(&[0x0a, 1]);
        oversized_aml.extend(std::iter::repeat_n(0, 512));
        assert_eq!(
            load_executor(
                &dsdt_with_s5(None, &oversized_aml),
                &[],
                CompatibilityProfile::default(),
                budget,
                FirmwareIdentity::new("01000000000000000200000000000000", 266),
            )
            .map(|_| ()),
            Err(LoadError::BudgetExceeded)
        );
    }

    #[test]
    fn excessive_nested_aml_blocks_exhaust_the_depth_budget() {
        let mut term = vec![0]; // ZeroOp
        for _ in 0..257 {
            let mut package_contents = vec![1]; // one package element
            package_contents.extend_from_slice(&term);
            let mut package = vec![0x12]; // PackageOp
            package.extend_from_slice(&aml_pkg_length(package_contents.len()));
            package.extend_from_slice(&package_contents);
            term = package;
        }
        let mut aml = vec![0x08]; // NameOp
        aml.extend_from_slice(b"PKG0");
        aml.extend_from_slice(&term);

        assert_eq!(
            load_executor(
                &dsdt_with_s5(None, &aml),
                &[],
                CompatibilityProfile::default(),
                ExecutionBudget::offline_default(),
                FirmwareIdentity::new("01000000000000000200000000000000", 267),
            )
            .map(|_| ()),
            Err(LoadError::BudgetExceeded)
        );
    }

    #[test]
    fn aml_copy_object_package_clone_is_charged_before_allocation() {
        let mut aml = vec![0x08]; // NameOp
        aml.extend_from_slice(b"PKG0");
        aml.push(0x12); // PackageOp
        aml.extend_from_slice(&aml_pkg_length(5));
        aml.extend_from_slice(&[4, 0, 0, 0, 0]);

        let mut device_body = Vec::new();
        append_aml_method(
            &mut device_body,
            b"_STA",
            0,
            &[0x9d, b'P', b'K', b'G', b'0', 0x60, 0xa4, 0x0a, 0x0f],
        );
        let device_package_length = 1 + 4 + device_body.len();
        assert!(device_package_length < 0x40);
        aml.extend_from_slice(&[0x5b, 0x82, device_package_length as u8]);
        aml.extend_from_slice(b"DEV0");
        aml.extend_from_slice(&device_body);
        let table = dsdt_with_s5(None, &aml);
        let package_bytes = 4 * core::mem::size_of::<acpi::aml::object::WrappedObject>() as u64;
        let budget = ExecutionBudget {
            max_package_bytes: table.len() as u64 + package_bytes,
            ..ExecutionBudget::offline_default()
        };
        let executor = load_executor(
            &table,
            &[],
            CompatibilityProfile::default(),
            budget,
            FirmwareIdentity::new("01000000000000000200000000000000", 268),
        )
        .unwrap();
        executor.activate_native_regions().unwrap();

        assert_eq!(
            executor.snapshot_devices(1, DeviceSnapshotPolicy::offline_default()),
            Err(LoadError::BudgetExceeded)
        );
    }

    #[test]
    fn implicit_store_buffer_replacement_is_charged_before_allocation() {
        const BUFFER_BYTES: usize = 8;

        let mut aml = vec![0x08]; // NameOp
        aml.extend_from_slice(b"TGT0");
        aml.push(0x11); // BufferOp
        let mut target_buffer_contents = vec![0x0a, BUFFER_BYTES as u8];
        target_buffer_contents.extend_from_slice(&[0; BUFFER_BYTES]);
        aml.extend_from_slice(&aml_pkg_length(target_buffer_contents.len()));
        aml.extend_from_slice(&target_buffer_contents);

        let mut sta_body = vec![0x70, 0x0a, 0x5a]; // Store(ByteConst(0x5a), TGT0)
        sta_body.extend_from_slice(b"TGT0");
        sta_body.extend_from_slice(&[0xa4, 0x0a, 0x0f]); // Return(0x0f)

        let mut device_body = Vec::new();
        append_aml_method(&mut device_body, b"_STA", 0, &sta_body);
        let device_package_length = 1 + 4 + device_body.len();
        assert!(device_package_length < 0x40);
        aml.extend_from_slice(&[0x5b, 0x82, device_package_length as u8]);
        aml.extend_from_slice(b"DEV0");
        aml.extend_from_slice(&device_body);
        let table = dsdt_with_s5(None, &aml);
        let budget = ExecutionBudget {
            max_buffer_bytes: BUFFER_BYTES as u64,
            ..ExecutionBudget::offline_default()
        };
        let executor = load_executor(
            &table,
            &[],
            CompatibilityProfile::default(),
            budget,
            FirmwareIdentity::new("01000000000000000200000000000000", 269),
        )
        .unwrap();
        executor.activate_native_regions().unwrap();

        assert_eq!(
            executor.snapshot_devices(1, DeviceSnapshotPolicy::offline_default()),
            Err(LoadError::BudgetExceeded)
        );

        let within_budget = ExecutionBudget {
            max_buffer_bytes: (2 * BUFFER_BYTES) as u64,
            ..ExecutionBudget::offline_default()
        };
        let executor = load_executor(
            &table,
            &[],
            CompatibilityProfile::default(),
            within_budget,
            FirmwareIdentity::new("01000000000000000200000000000000", 270),
        )
        .unwrap();
        executor.activate_native_regions().unwrap();
        assert_eq!(
            executor
                .snapshot_devices(1, DeviceSnapshotPolicy::offline_default())
                .unwrap()
                .len(),
            1
        );
    }

    #[test]
    fn field_list_entries_are_charged_individually_during_aml_parse() {
        let budget = ExecutionBudget {
            max_namespace_objects: 12, // ten fixed objects, one OperationRegion, one FieldUnit
            ..ExecutionBudget::offline_default()
        };
        let identity = FirmwareIdentity::new("01000000000000000200000000000000", 91);
        let single_field = dsdt_with_field_units(&[*b"F000"]);
        assert!(
            load_executor(
                &single_field,
                &[],
                CompatibilityProfile::default(),
                budget,
                identity.clone()
            )
            .is_ok()
        );

        let multiple_fields = dsdt_with_field_units(&[*b"F000", *b"F001"]);
        assert_eq!(
            load_executor(
                &multiple_fields,
                &[],
                CompatibilityProfile::default(),
                budget,
                identity,
            )
            .err(),
            Some(LoadError::BudgetExceeded)
        );
    }

    #[test]
    fn oversized_buffer_initializer_is_bounded_without_panicking() {
        let body = [0xa4, 0x11, 5, 0x0a, 1, 0xaa, 0xbb];
        // PkgLength=5 includes itself, the two-byte BufferSize, and two initializer bytes.
        let table = dsdt_with_sta_body(&body);
        let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
            let executor = load_executor(
                &table,
                &[],
                CompatibilityProfile::default(),
                ExecutionBudget::offline_default(),
                FirmwareIdentity::new("01000000000000000200000000000000", 93),
            )
            .unwrap();
            executor.activate_native_regions().unwrap();
            executor.snapshot_devices(1, DeviceSnapshotPolicy::offline_default())
        }));
        let snapshots = result.expect("malformed AML must not panic").unwrap();
        assert_eq!(snapshots.len(), 1);
        assert!(snapshots[0].degraded);
    }

    #[test]
    fn varpackage_with_more_elements_than_declared_fails_without_panicking() {
        let table = dsdt_with_sta_body(&[0xa4, 0x13, 5, 0x0a, 1, 0x00, 0x00]);
        let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
            let executor = load_executor(
                &table,
                &[],
                CompatibilityProfile::default(),
                ExecutionBudget::offline_default(),
                FirmwareIdentity::new("01000000000000000200000000000000", 95),
            )
            .unwrap();
            executor.activate_native_regions().unwrap();
            executor.snapshot_devices(1, DeviceSnapshotPolicy::offline_default())
        }));
        let snapshots = result.expect("malformed AML must not panic").unwrap();
        assert_eq!(snapshots.len(), 1);
        assert!(snapshots[0].degraded);
    }

    #[test]
    fn aml_division_by_zero_returns_a_method_error_without_panicking() {
        let table = dsdt_with_sta_body(&[0xa4, 0x78, 0x0a, 1, 0x0a, 0, 0, 0]);
        let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
            let executor = load_executor(
                &table,
                &[],
                CompatibilityProfile::default(),
                ExecutionBudget::offline_default(),
                FirmwareIdentity::new("01000000000000000200000000000000", 96),
            )
            .unwrap();
            executor.activate_native_regions().unwrap();
            executor.snapshot_devices(1, DeviceSnapshotPolicy::offline_default())
        }));
        let snapshots = result.expect("divide-by-zero AML must not panic").unwrap();
        assert_eq!(snapshots.len(), 1);
        assert!(snapshots[0].degraded);
    }

    #[test]
    fn s5_sleep_types_are_read_from_the_namespace_as_bounded_integers() {
        let executor = load_executor(
            &dsdt_with_s5(Some((5, 6)), &[]),
            &[],
            CompatibilityProfile::default(),
            ExecutionBudget::offline_default(),
            FirmwareIdentity::new("01000000000000000200000000000000", 122),
        )
        .unwrap();

        assert_eq!(
            executor.get_s5_sleep_types(),
            Ok(SleepTypes {
                type_a: 5,
                type_b: 6,
            })
        );
    }

    #[test]
    fn s5_namespace_path_is_charged_to_the_work_unit_heap_budget() {
        let executor = load_executor(
            &dsdt_with_s5(Some((5, 6)), &[]),
            &[],
            CompatibilityProfile::default(),
            ExecutionBudget::offline_default(),
            FirmwareIdentity::new("01000000000000000200000000000000", 126),
        )
        .unwrap();
        executor.set_heap_budget_to_persistent_usage_for_test();

        assert_eq!(
            executor.get_s5_sleep_types(),
            Err(LoadError::BudgetExceeded)
        );
        assert_eq!(
            executor.get_s5_sleep_types(),
            Err(LoadError::BudgetExceeded)
        );
    }

    #[test]
    fn s5_sleep_types_ignore_unrelated_global_lock_methods() {
        let missing = load_executor(
            &dsdt_with_s5(None, &[]),
            &[],
            CompatibilityProfile::default(),
            ExecutionBudget::offline_default(),
            FirmwareIdentity::new("01000000000000000200000000000000", 123),
        )
        .unwrap();
        assert_eq!(
            missing.get_s5_sleep_types(),
            Err(LoadError::SleepStateUnavailable)
        );

        let malformed = load_executor(
            &dsdt_with_s5(Some((8, 0)), &[]),
            &[],
            CompatibilityProfile::default(),
            ExecutionBudget::offline_default(),
            FirmwareIdentity::new("01000000000000000200000000000000", 124),
        )
        .unwrap();
        assert_eq!(
            malformed.get_s5_sleep_types(),
            Err(LoadError::InvalidSleepState)
        );

        let mut global_lock_method = Vec::new();
        append_aml_method(
            &mut global_lock_method,
            b"_STA",
            0,
            &[0x5b, 0x23, 0x5c, b'_', b'G', b'L', b'_', 0, 0xa4, 0],
        );
        let global_lock = load_executor(
            &dsdt_with_s5(Some((5, 5)), &global_lock_method),
            &[],
            CompatibilityProfile::default(),
            ExecutionBudget::offline_default(),
            FirmwareIdentity::new("01000000000000000200000000000000", 125),
        )
        .unwrap();
        assert!(global_lock.summary().global_lock_dependency);
        assert_eq!(
            global_lock.get_s5_sleep_types(),
            Ok(SleepTypes {
                type_a: 5,
                type_b: 5
            })
        );
    }

    #[test]
    fn single_bit_aml_mutation_corpus_never_panics() {
        let baselines = [
            dsdt_with_sta_body(&[0xa4, 0x00]),
            dsdt_with_sta_body(&[0xa4, 0x01]),
            dsdt_with_sta_body(&[0xa4, 0x78, 0x0a, 1, 0x0a, 0, 0]),
            dsdt_with_sta_body(&[0xa4, 0x11, 5, 0x0a, 1, 0xaa, 0xbb]),
        ];

        for (baseline_index, baseline) in baselines.iter().enumerate() {
            for byte_index in 36..baseline.len() {
                for bit in 0..u8::BITS {
                    let mut mutated = baseline.clone();
                    mutated[byte_index] ^= 1 << bit;

                    let outcome = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                        let Ok(executor) = load_executor(
                            &mutated,
                            &[],
                            CompatibilityProfile::default(),
                            ExecutionBudget::offline_default(),
                            FirmwareIdentity::new(
                                "01000000000000000200000000000000",
                                101 + baseline_index as u64,
                            ),
                        ) else {
                            return;
                        };
                        if executor.activate_native_regions().is_ok() {
                            let _ = executor
                                .snapshot_devices(1, DeviceSnapshotPolicy::offline_default());
                        }
                    }));

                    assert!(
                        outcome.is_ok(),
                        "AML mutation panicked: baseline={baseline_index}, byte={byte_index}, bit={bit}"
                    );
                }
            }
        }
    }

    #[test]
    fn pic_is_sealed_at_load_and_evaluated_once_after_region_activation() {
        let executor = load_executor(
            &dsdt_with_pic_method(),
            &[],
            CompatibilityProfile::default(),
            ExecutionBudget::offline_default(),
            FirmwareIdentity::new("01000000000000000200000000000000", 94),
        )
        .unwrap();
        assert!(!executor.summary().pic_evaluated);

        executor.activate_native_regions().unwrap();
        assert!(executor.summary().pic_evaluated);
        assert_eq!(
            executor.activate_native_regions(),
            Err(LoadError::InvalidProfile)
        );
        assert!(executor.summary().pic_evaluated);
    }

    #[test]
    fn failed_pic_handshake_blocks_later_device_method_evaluation() {
        let executor = load_executor(
            &dsdt_with_failing_pic_and_sta(&[0xa3]),
            &[],
            CompatibilityProfile::default(),
            ExecutionBudget::offline_default(),
            FirmwareIdentity::new("01000000000000000200000000000000", 97),
        )
        .unwrap();

        assert_eq!(
            executor.activate_native_regions(),
            Err(LoadError::AmlRejected)
        );
        assert!(executor.summary().pic_degraded);
        assert_eq!(
            executor.snapshot_devices(1, DeviceSnapshotPolicy::offline_default()),
            Err(LoadError::InvalidProfile)
        );
        assert_eq!(executor.get_pci_roots(), Err(LoadError::InvalidProfile));
        assert_eq!(
            executor.negotiate_pci_features("\\DEV0", 0, 0),
            Err(LoadError::InvalidProfile)
        );
    }

    #[test]
    fn failed_reg_disconnect_after_pic_error_is_fatal_cleanup_failure() {
        let reg_body = [0xa0, 11, 0x92, 0x69, 0xa4, 0x78, 0x0a, 1, 0x0a, 0, 0, 0];
        let executor = load_executor(
            &dsdt_with_failing_pic_and_sta(&reg_body),
            &[],
            CompatibilityProfile::default(),
            ExecutionBudget::offline_default(),
            FirmwareIdentity::new("01000000000000000200000000000000", 98),
        )
        .unwrap();

        assert_eq!(
            executor.activate_native_regions(),
            Err(LoadError::CleanupFailed)
        );
        assert_eq!(
            executor.snapshot_devices(1, DeviceSnapshotPolicy::offline_default()),
            Err(LoadError::InvalidProfile)
        );
    }

    #[test]
    fn failed_reg_activation_rolls_back_and_blocks_later_region_dispatch() {
        let attempts = Arc::new(AtomicU64::new(0));
        let backend = Arc::new(CountingRegionBackend(attempts.clone()));
        let executor = load_executor_with_backend(
            &dsdt_with_region_methods(),
            &[],
            CompatibilityProfile::default(),
            ExecutionBudget::controlled_read_only(),
            FirmwareIdentity::new("01000000000000000200000000000000", 92),
            HardwareSideEffectProfile::controlled_read_only(),
            backend,
        )
        .unwrap();

        assert!(executor.activate_native_regions().is_err());
        let attempts_after_rollback = attempts.load(Ordering::Acquire);
        assert!(attempts_after_rollback >= 1);

        let _ = executor.snapshot_devices(
            256,
            DeviceSnapshotPolicy {
                hardware: HardwareSideEffectProfile::controlled_read_only(),
                ..DeviceSnapshotPolicy::offline_default()
            },
        );
        assert_eq!(attempts.load(Ordering::Acquire), attempts_after_rollback);
        assert!(executor.summary().region_registration_degraded);
    }

    #[test]
    fn pic_failure_can_deactivate_regions_at_the_minimum_heap_budget() {
        let mut aml = vec![0x5b, 0x80]; // OpRegion
        aml.extend_from_slice(b"REG0");
        aml.extend_from_slice(&[1, 0, 1]); // SystemIO, ZeroOp base, OneOp length
        let field_body_length = 4 + 1 + 5;
        aml.extend_from_slice(&[0x5b, 0x81, (field_body_length + 1) as u8]);
        aml.extend_from_slice(b"REG0");
        aml.push(0); // AnyAcc, NoLock, Preserve
        aml.extend_from_slice(b"F000\x08");
        append_aml_method(&mut aml, b"_REG", 2, &[0xa4, 0x00]); // Return Zero
        append_aml_method(&mut aml, b"_PIC", 1, &[0xa4, b'F', b'0', b'0', b'0']);
        let table = dsdt_with_s5(None, &aml);

        let reaches_pic = |max_heap_bytes: u64, process_id: u64| {
            let attempts = Arc::new(AtomicU64::new(0));
            let backend = Arc::new(CountingRegionBackend(attempts.clone()));
            let budget = ExecutionBudget {
                max_heap_bytes,
                ..ExecutionBudget::controlled_read_only()
            };
            let Ok(executor) = load_executor_with_backend(
                &table,
                &[],
                CompatibilityProfile::default(),
                budget,
                FirmwareIdentity::new("01000000000000000200000000000000", process_id),
                HardwareSideEffectProfile::controlled_read_only(),
                backend,
            ) else {
                return false;
            };
            let _ = executor.activate_native_regions();
            attempts.load(Ordering::Acquire) != 0
        };

        let mut lower = 0_u64;
        let mut upper = 1 << 16;
        assert!(reaches_pic(upper, 810));
        while lower + 1 < upper {
            let midpoint = lower + (upper - lower) / 2;
            if reaches_pic(midpoint, 811) {
                upper = midpoint;
            } else {
                lower = midpoint;
            }
        }

        let attempts = Arc::new(AtomicU64::new(0));
        let executor = load_executor_with_backend(
            &table,
            &[],
            CompatibilityProfile::default(),
            ExecutionBudget {
                max_heap_bytes: upper,
                ..ExecutionBudget::controlled_read_only()
            },
            FirmwareIdentity::new("01000000000000000200000000000000", 812),
            HardwareSideEffectProfile::controlled_read_only(),
            Arc::new(CountingRegionBackend(attempts.clone())),
        )
        .unwrap();
        assert!(executor.activate_native_regions().is_err());
        assert!(attempts.load(Ordering::Acquire) > 0);
        assert!(!executor.summary().region_registration_degraded);
    }

    #[test]
    fn device_identifiers_preserve_hid_cid_uid_kinds_and_enforce_bounds() {
        let hid = Object::String("PNP0A08".to_string()).wrap();
        let cids = Object::Package(vec![
            Object::String("PNP0A03".to_string()).wrap(),
            Object::Integer(0x030a_d041).wrap(),
        ])
        .wrap();
        let uid = Object::Integer(7).wrap();

        let identifiers =
            decode_device_identifiers(Some(&*hid), Some(&*cids), Some(&*uid)).unwrap();
        assert_eq!(
            identifiers,
            vec![
                DeviceIdentifier {
                    kind: DeviceIdentifierKind::Hid,
                    value: DeviceIdentifierValue::String("PNP0A08".to_string()),
                },
                DeviceIdentifier {
                    kind: DeviceIdentifierKind::Cid,
                    value: DeviceIdentifierValue::String("PNP0A03".to_string()),
                },
                DeviceIdentifier {
                    kind: DeviceIdentifierKind::Cid,
                    value: DeviceIdentifierValue::Integer(0x030a_d041),
                },
                DeviceIdentifier {
                    kind: DeviceIdentifierKind::Uid,
                    value: DeviceIdentifierValue::Integer(7),
                },
            ]
        );

        let too_many = Object::Package(
            (0..=MAX_DEVICE_CIDS)
                .map(|value| Object::Integer(value as u64).wrap())
                .collect(),
        )
        .wrap();
        assert_eq!(
            decode_device_identifiers(None, Some(&*too_many), None),
            Err(DeviceIdentifierError::TooManyCids)
        );
        let malformed_hid = Object::Package(Vec::new()).wrap();
        assert_eq!(
            decode_device_identifiers(Some(&*malformed_hid), None, None),
            Err(DeviceIdentifierError::InvalidValue)
        );
    }

    #[test]
    fn device_identifier_vector_reservation_reports_capacity_exhaustion() {
        let mut values = Vec::<u8>::new();
        assert_eq!(
            try_reserve_device_identifier_output(&mut values, usize::MAX),
            Err(DeviceIdentifierError::AllocationFailed)
        );
        assert!(values.is_empty());
    }

    #[test]
    fn namespace_device_ids_reserve_zero_for_absence() {
        const GENERATION: u64 = 2_813_947_644_080_581_817;
        const LINK_PATH: &str = "\\_SB_.PCI0.LNKA";

        assert_eq!(table_hash(GENERATION, LINK_PATH.as_bytes()), 0);
        assert_eq!(namespace_device_id(GENERATION, LINK_PATH), 1);
    }

    #[test]
    fn pci_prt_routes_preserve_gsi_and_opaque_link_device_identity() {
        use acpi::aml::namespace::{AmlName, Namespace, NamespaceLevelKind};
        use core::str::FromStr;

        let router_path = AmlName::from_str("\\_SB_.PCI0").unwrap();
        let link_path = AmlName::from_str("\\_SB_.PCI0.LNKA").unwrap();
        let mut namespace = Namespace::new(acpi::Handle(1));
        namespace
            .add_level(
                AmlName::from_str("\\_SB_").unwrap(),
                NamespaceLevelKind::Device,
            )
            .unwrap();
        namespace
            .add_level(router_path.clone(), NamespaceLevelKind::Device)
            .unwrap();
        namespace
            .add_level(link_path.clone(), NamespaceLevelKind::Device)
            .unwrap();
        let routing = Object::Package(vec![
            Object::Package(vec![
                Object::Integer(0x0002_ffff).wrap(),
                Object::Integer(0).wrap(),
                Object::Integer(0).wrap(),
                Object::Integer(23).wrap(),
            ])
            .wrap(),
            Object::Package(vec![
                Object::Integer(0x0003_ffff).wrap(),
                Object::Integer(3).wrap(),
                Object::NamePath {
                    name: AmlName::from_str("LNKA").unwrap(),
                    scope: router_path.clone(),
                }
                .wrap(),
                Object::Integer(1).wrap(),
            ])
            .wrap(),
        ]);

        let routes =
            interpreter::decode_pci_routing_table(&routing, &namespace, 9, || Ok(())).unwrap();
        assert_eq!(
            routes,
            vec![
                PciRoutingEntry {
                    address: 0x0002_ffff,
                    pin: 0,
                    source: PciRoutingSource::DirectGsi(23),
                },
                PciRoutingEntry {
                    address: 0x0003_ffff,
                    pin: 3,
                    source: PciRoutingSource::LinkDevice {
                        device_id: table_hash(9, link_path.to_string().as_bytes()).max(1),
                        source_index: 1,
                    },
                },
            ]
        );
    }

    #[test]
    fn pci_prt_rejects_malformed_and_duplicate_pin_mappings() {
        use acpi::aml::namespace::Namespace;

        let namespace = Namespace::new(acpi::Handle(1));
        let route = || {
            Object::Package(vec![
                Object::Integer(0x0002_ffff).wrap(),
                Object::Integer(0).wrap(),
                Object::Integer(0).wrap(),
                Object::Integer(23).wrap(),
            ])
            .wrap()
        };
        let duplicate = Object::Package(vec![route(), route()]);
        assert_eq!(
            interpreter::decode_pci_routing_table(&duplicate, &namespace, 9, || Ok(())),
            Err(PciRoutingError::DuplicatePin)
        );

        let invalid_pin = Object::Package(vec![
            Object::Package(vec![
                Object::Integer(0x0002_ffff).wrap(),
                Object::Integer(4).wrap(),
                Object::Integer(0).wrap(),
                Object::Integer(23).wrap(),
            ])
            .wrap(),
        ]);
        assert_eq!(
            interpreter::decode_pci_routing_table(&invalid_pin, &namespace, 9, || Ok(())),
            Err(PciRoutingError::InvalidEntry)
        );
    }

    #[test]
    fn pci_prt_rejects_string_literals_as_link_device_names() {
        use acpi::aml::namespace::{AmlName, Namespace, NamespaceLevelKind};
        use core::str::FromStr;

        let router_path = AmlName::from_str("\\_SB_.PCI0").unwrap();
        let mut namespace = Namespace::new(acpi::Handle(1));
        namespace
            .add_level(router_path.clone(), NamespaceLevelKind::Device)
            .unwrap();
        namespace
            .add_level(
                AmlName::from_str("\\_SB_.PCI0.LNKA").unwrap(),
                NamespaceLevelKind::Device,
            )
            .unwrap();
        let routing = Object::Package(vec![
            Object::Package(vec![
                Object::Integer(0x0002_ffff).wrap(),
                Object::Integer(0).wrap(),
                Object::String("LNKA".to_string()).wrap(),
                Object::Integer(1).wrap(),
            ])
            .wrap(),
        ]);

        assert_eq!(
            interpreter::decode_pci_routing_table(&routing, &namespace, 9, || Ok(())),
            Err(PciRoutingError::InvalidEntry)
        );
    }

    #[test]
    fn execution_deadline_is_per_top_level_method_and_shared_by_nested_calls() {
        let budget = ExecutionBudget {
            max_method_duration_ns: 10,
            ..ExecutionBudget::offline_default()
        };
        let mut state = ExecutionState::new(budget).unwrap();

        state.enter_call_at(100).unwrap();
        assert_eq!(state.check_deadline(109), Ok(()));
        state.enter_call_at(105).unwrap();
        assert_eq!(state.check_deadline(109), Ok(()));
        state.leave_call();
        state.leave_call();

        state.enter_call_at(200).unwrap();
        assert_eq!(state.check_deadline(209), Ok(()));
        assert_eq!(state.check_deadline(210), Err(LoadError::DeadlineExceeded));
        state.leave_call();
        assert!(state.deadline_failed());
        assert_eq!(state.enter_call_at(300), Err(LoadError::DeadlineExceeded));
    }

    #[test]
    fn deadline_clock_is_sampled_once_per_1024_opcodes() {
        let budget = ExecutionBudget {
            max_method_duration_ns: 10,
            ..ExecutionBudget::offline_default()
        };
        let mut state = ExecutionState::new(budget).unwrap();
        state.enter_call_at(100).unwrap();

        assert!(state.deadline_check_due());
        for _ in 0..1023 {
            assert!(!state.deadline_check_due());
        }
        assert!(state.deadline_check_due());
        state.leave_call();
        assert!(!state.deadline_check_due());
    }

    #[test]
    fn deadline_rejects_clock_overflow_instead_of_disabling_watchdog() {
        let budget = ExecutionBudget {
            max_method_duration_ns: 10,
            ..ExecutionBudget::offline_default()
        };
        let mut state = ExecutionState::new(budget).unwrap();
        assert_eq!(
            state.enter_call_at(u64::MAX),
            Err(LoadError::DeadlineExceeded)
        );
        assert!(state.deadline_failed());
    }

    #[test]
    fn snapshot_policy_is_explicit_and_bounded() {
        let policy = DeviceSnapshotPolicy::offline_default();
        assert!(policy.evaluate_sta);
        assert!(policy.evaluate_ini);
        assert!(policy.evaluate_crs);
        assert!(policy.evaluate_prt);
        assert_eq!(policy.hardware, HardwareSideEffectProfile::deny_all());
        assert_eq!(policy.max_resources_per_device, 256);
        assert_eq!(policy.max_snapshot_bytes, 64 * 1024);
        assert_eq!(policy.validate(), Ok(()));
    }

    #[test]
    fn snapshot_policy_keeps_native_profile_explicit_and_rejects_invalid_dependencies() {
        let mut policy = DeviceSnapshotPolicy::offline_default();
        policy.hardware.allow_system_memory = true;
        assert_eq!(policy.validate(), Ok(()));
        assert!(!policy.hardware.allow_writes);
        assert_eq!(
            HardwareSideEffectProfile::controlled_read_only(),
            HardwareSideEffectProfile {
                allow_system_memory: true,
                allow_system_io: true,
                allow_pci_config: false,
                allow_writes: false,
            }
        );

        let policy = DeviceSnapshotPolicy {
            evaluate_sta: false,
            evaluate_ini: true,
            ..DeviceSnapshotPolicy::offline_default()
        };
        assert_eq!(policy.validate(), Err(LoadError::InvalidProfile));

        let policy = DeviceSnapshotPolicy {
            max_snapshot_bytes: 64 * 1024 + 1,
            ..DeviceSnapshotPolicy::offline_default()
        };
        assert_eq!(policy.validate(), Err(LoadError::BudgetExceeded));
    }

    #[test]
    fn snapshot_hardware_policy_cannot_widen_the_executor_profile() {
        let dsdt = aml_table_with_nops(b"DSDT", 1);
        let executor = load_executor(
            &dsdt,
            &[],
            CompatibilityProfile::default(),
            ExecutionBudget::offline_default(),
            FirmwareIdentity::new("01000000000000000200000000000000", 260),
        )
        .unwrap();
        executor.activate_native_regions().unwrap();

        let policy = DeviceSnapshotPolicy {
            hardware: HardwareSideEffectProfile::controlled_read_only(),
            ..DeviceSnapshotPolicy::offline_default()
        };
        assert_eq!(
            executor.snapshot_devices(0, policy),
            Err(LoadError::InvalidProfile)
        );
    }

    #[test]
    fn zero_native_budget_is_fail_closed() {
        let mut state = ExecutionState::new(ExecutionBudget::offline_default()).unwrap();
        assert_eq!(state.native_access(), Err(LoadError::NativeAccessRejected));
    }

    #[test]
    fn global_lock_name_in_loaded_table_records_the_dependency() {
        let mut table = vec![0; 36];
        let aml = [0x08, b'_', b'G', b'L', b'K', 0x01];
        table[..4].copy_from_slice(b"DSDT");
        table[4..8].copy_from_slice(&((36 + aml.len()) as u32).to_le_bytes());
        table[8] = 2;
        table.extend_from_slice(&aml);

        let executor = load_executor(
            &table,
            &[],
            CompatibilityProfile::default(),
            ExecutionBudget::offline_default(),
            FirmwareIdentity::new("01000000000000000200000000000000", 99),
        )
        .unwrap();

        assert!(executor.summary().global_lock_dependency);
        assert!(!executor.summary().pic_evaluated);
        assert!(executor.preflight_system_io_resources().unwrap().is_empty());
        executor.activate_native_regions().unwrap();
        assert!(
            executor
                .snapshot_devices(1, DeviceSnapshotPolicy::offline_default())
                .unwrap()
                .is_empty()
        );
        assert!(!executor.summary().native_access_attempted);
    }

    #[test]
    fn global_lock_bytes_in_string_data_do_not_degrade_executor() {
        let mut table = vec![0; 36];
        let aml = [
            0x08, b'S', b'T', b'R', b'0', 0x0d, b'l', b'i', b't', b'e', b'r', b'a', b'l', b' ',
            b'_', b'G', b'L', b'K', 0,
        ];
        table[..4].copy_from_slice(b"DSDT");
        table[4..8].copy_from_slice(&((36 + aml.len()) as u32).to_le_bytes());
        table[8] = 2;
        table.extend_from_slice(&aml);

        let executor = load_executor(
            &table,
            &[],
            CompatibilityProfile::default(),
            ExecutionBudget::offline_default(),
            FirmwareIdentity::new("01000000000000000200000000000000", 100),
        )
        .unwrap();

        assert!(!executor.summary().global_lock_dependency);
    }

    #[test]
    fn global_lock_acquire_method_runs_with_the_host_backend() {
        let table = dsdt_with_sta_body(&[
            0x5b, 0x23, 0x5c, b'_', b'G', b'L', b'_', 0x00, 0x00, // Acquire (\\_GL_, NoWait)
            0x5b, 0x27, 0x5c, b'_', b'G', b'L', b'_', // Release (\\_GL_)
            0xa4, 0x0a, 0x0f, // Return (0x0f)
        ]);
        let executor = load_executor(
            &table,
            &[],
            CompatibilityProfile::default(),
            ExecutionBudget::offline_default(),
            FirmwareIdentity::new("01000000000000000200000000000000", 101),
        )
        .unwrap();

        assert!(executor.summary().global_lock_dependency);
        executor.activate_native_regions().unwrap();
        assert_eq!(executor.evaluate_method_for_test("\\DEV0._STA"), Ok(0x0f));
        let devices = executor
            .snapshot_devices(1, DeviceSnapshotPolicy::offline_default())
            .unwrap();
        assert_eq!(devices.len(), 1);
        assert!(!devices[0].degraded);
        assert_eq!(devices[0].status, 0x0f);
        assert!(!executor.summary().native_access_attempted);
    }

    #[test]
    fn non_integer_sta_is_degraded_and_not_admitted_as_present() {
        let table = dsdt_with_sta_body(&[0xa4, 0x0d, b'x', 0x00]); // Return ("x")
        let executor = load_executor(
            &table,
            &[],
            CompatibilityProfile::default(),
            ExecutionBudget::offline_default(),
            FirmwareIdentity::new("01000000000000000200000000000000", 104),
        )
        .unwrap();
        executor.activate_native_regions().unwrap();

        let devices = executor
            .snapshot_devices(1, DeviceSnapshotPolicy::offline_default())
            .unwrap();
        assert_eq!(devices.len(), 1);
        assert_eq!(devices[0].status, 0);
        assert!(devices[0].status_evaluated);
        assert!(devices[0].degraded);
        assert!(!devices[0].initialized);
        assert!(!devices[0].initialization_evaluated);
        assert!(devices[0].resources.is_empty());
    }

    #[test]
    fn global_lock_alias_method_acquires_the_same_host_lock() {
        let mut aml = Vec::new();
        let mut device_body = vec![0x06, 0x5c, b'_', b'G', b'L', b'_', b'M', b'U', b'T', b'0'];
        append_aml_method(
            &mut device_body,
            b"_STA",
            0,
            &[
                0x5b, 0x23, b'M', b'U', b'T', b'0', 0x00, 0x00, // Acquire (MUT0, NoWait)
                0x5b, 0x27, b'M', b'U', b'T', b'0', // Release (MUT0)
                0xa4, 0x0a, 0x0f, // Return (0x0f)
            ],
        );
        let device_package_length = 1 + 4 + device_body.len();
        aml.extend_from_slice(&[0x5b, 0x82, device_package_length as u8]);
        aml.extend_from_slice(b"DEV0");
        aml.extend_from_slice(&device_body);

        let mut table = vec![0; 36];
        table[..4].copy_from_slice(b"DSDT");
        table[4..8].copy_from_slice(&((36 + aml.len()) as u32).to_le_bytes());
        table[8] = 2;
        table.extend_from_slice(&aml);

        let executor = load_executor(
            &table,
            &[],
            CompatibilityProfile::default(),
            ExecutionBudget::offline_default(),
            FirmwareIdentity::new("01000000000000000200000000000000", 102),
        )
        .unwrap();

        assert!(executor.summary().global_lock_dependency);
        executor.activate_native_regions().unwrap();
        assert_eq!(executor.evaluate_method_for_test("\\DEV0._STA"), Ok(0x0f));
        let devices = executor
            .snapshot_devices(1, DeviceSnapshotPolicy::offline_default())
            .unwrap();
        assert_eq!(devices.len(), 1);
        assert!(!devices[0].degraded);
        assert_eq!(devices[0].status, 0x0f);
    }

    #[test]
    fn device_glk_flag_locks_only_methods_below_that_device() {
        let mut aml = Vec::new();
        let mut device_body = vec![0x08, b'_', b'G', b'L', b'K', 0x01];
        append_aml_method(&mut device_body, b"_STA", 0, &[0xa4, 0x0a, 0x0f]);
        let device_package_length = 1 + 4 + device_body.len();
        aml.extend_from_slice(&[0x5b, 0x82, device_package_length as u8]);
        aml.extend_from_slice(b"DEV0");
        aml.extend_from_slice(&device_body);
        let mut unrelated_device_body = Vec::new();
        append_aml_method(&mut unrelated_device_body, b"_STA", 0, &[0xa4, 0x0a, 0x0f]);
        let unrelated_device_package_length = 1 + 4 + unrelated_device_body.len();
        aml.extend_from_slice(&[0x5b, 0x82, unrelated_device_package_length as u8]);
        aml.extend_from_slice(b"DEV1");
        aml.extend_from_slice(&unrelated_device_body);

        let mut table = vec![0; 36];
        table[..4].copy_from_slice(b"DSDT");
        table[4..8].copy_from_slice(&((36 + aml.len()) as u32).to_le_bytes());
        table[8] = 2;
        table.extend_from_slice(&aml);
        let executor = load_executor(
            &table,
            &[],
            CompatibilityProfile::default(),
            ExecutionBudget::offline_default(),
            FirmwareIdentity::new("01000000000000000200000000000000", 103),
        )
        .unwrap();

        assert!(executor.summary().global_lock_dependency);
        executor.activate_native_regions().unwrap();
        let devices = executor
            .snapshot_devices(2, DeviceSnapshotPolicy::offline_default())
            .unwrap();
        assert_eq!(devices.len(), 2);
        let locked_device = devices
            .iter()
            .find(|device| device.path.ends_with("DEV0"))
            .unwrap();
        let unrelated_device = devices
            .iter()
            .find(|device| device.path.ends_with("DEV1"))
            .unwrap();
        assert!(!locked_device.degraded);
        assert_eq!(locked_device.status, 0x0f);
        assert!(!unrelated_device.degraded);
        assert_eq!(unrelated_device.status, 0x0f);
    }

    #[test]
    fn glk_device_discovery_returns_hid_cid_and_uid_after_locked_sta() {
        let mut device_body = vec![0x08, b'_', b'G', b'L', b'K', 0x01];
        append_aml_method(&mut device_body, b"_STA", 0, &[0xa4, 0x0a, 0x0f]);
        device_body.extend_from_slice(&[0x08, b'_', b'H', b'I', b'D', 0x0d]);
        device_body.extend_from_slice(b"ACME0001\0");
        device_body.extend_from_slice(&[0x08, b'_', b'C', b'I', b'D', 0x0d]);
        device_body.extend_from_slice(b"ACMECOMP\0");
        device_body.extend_from_slice(&[0x08, b'_', b'U', b'I', b'D', 0x0a, 0x2a]);

        let mut aml = vec![0x5b, 0x82, (1 + 4 + device_body.len()) as u8];
        aml.extend_from_slice(b"DEV0");
        aml.extend_from_slice(&device_body);
        let mut table = vec![0; 36];
        table[..4].copy_from_slice(b"DSDT");
        table[4..8].copy_from_slice(&((36 + aml.len()) as u32).to_le_bytes());
        table[8] = 2;
        table.extend_from_slice(&aml);

        let executor = load_executor(
            &table,
            &[],
            CompatibilityProfile::default(),
            ExecutionBudget::offline_default(),
            FirmwareIdentity::new("01000000000000000200000000000000", 104),
        )
        .unwrap();
        executor.activate_native_regions().unwrap();
        let devices = executor
            .snapshot_devices(1, DeviceSnapshotPolicy::offline_default())
            .unwrap();
        assert_eq!(devices.len(), 1);
        let device = &devices[0];
        assert!(!device.degraded);
        assert_eq!(device.status, 0x0f);
        assert!(device.identifiers.iter().any(|identifier| {
            identifier.kind == DeviceIdentifierKind::Hid
                && identifier.value == DeviceIdentifierValue::String("ACME0001".into())
        }));
        assert!(device.identifiers.iter().any(|identifier| {
            identifier.kind == DeviceIdentifierKind::Cid
                && identifier.value == DeviceIdentifierValue::String("ACMECOMP".into())
        }));
        assert!(device.identifiers.iter().any(|identifier| {
            identifier.kind == DeviceIdentifierKind::Uid
                && identifier.value == DeviceIdentifierValue::Integer(0x2a)
        }));
    }

    #[test]
    fn malformed_table_header_is_rejected_without_execution() {
        assert_eq!(
            load_namespace(
                b"DSDT",
                &[],
                CompatibilityProfile::default(),
                ExecutionBudget::offline_default(),
                FirmwareIdentity::new("01000000000000000200000000000000", 1)
            ),
            Err(LoadError::InvalidTable)
        );
    }

    #[test]
    fn executor_budget_accepts_aml_catalogs_larger_than_one_mibibyte() {
        let dsdt = aml_table_with_nops(b"DSDT", 600 * 1024);
        let ssdt = aml_table_with_nops(b"SSDT", 600 * 1024);
        let executor = load_executor(
            &dsdt,
            &[&ssdt],
            CompatibilityProfile::default(),
            ExecutionBudget::offline_default(),
            FirmwareIdentity::new("01000000000000000200000000000000", 113),
        )
        .expect("the executor budget must cover the kernel's 16 MiB firmware catalog limit");

        assert_eq!(executor.summary().tables_loaded, 2);
    }

    #[test]
    fn executor_loader_enforces_firmware_table_size_count_and_catalog_limits() {
        let identity = FirmwareIdentity::new("01000000000000000200000000000000", 114);
        let profile = CompatibilityProfile::default();
        let budget = ExecutionBudget::offline_default();
        let empty_dsdt = aml_table_with_nops(b"DSDT", 0);

        let oversized_dsdt = aml_table_with_nops(b"DSDT", MAX_AML_TABLE_BYTES as usize - 36 + 1);
        assert_eq!(
            load_executor(&oversized_dsdt, &[], profile, budget, identity.clone()).map(|_| ()),
            Err(LoadError::BudgetExceeded)
        );

        let oversized_ssdt = aml_table_with_nops(b"SSDT", MAX_AML_TABLE_BYTES as usize - 36 + 1);
        assert_eq!(
            load_executor(
                &empty_dsdt,
                &[oversized_ssdt.as_slice()],
                profile,
                budget,
                identity.clone(),
            )
            .map(|_| ()),
            Err(LoadError::BudgetExceeded)
        );

        let tiny_ssdt = aml_table_with_nops(b"SSDT", 0);
        let too_many_ssdts = vec![tiny_ssdt.as_slice(); MAX_AML_TABLE_COUNT];
        assert_eq!(
            load_executor(
                &empty_dsdt,
                &too_many_ssdts,
                profile,
                budget,
                identity.clone()
            )
            .map(|_| ()),
            Err(LoadError::BudgetExceeded)
        );

        let max_size_dsdt = aml_table_with_nops(b"DSDT", MAX_AML_TABLE_BYTES as usize - 36);
        let max_size_ssdt = aml_table_with_nops(b"SSDT", MAX_AML_TABLE_BYTES as usize - 36);
        let over_catalog_limit = vec![max_size_ssdt.as_slice(); 16];
        assert_eq!(
            load_executor(
                &max_size_dsdt,
                &over_catalog_limit,
                profile,
                budget,
                identity
            )
            .map(|_| ()),
            Err(LoadError::BudgetExceeded)
        );
    }

    #[test]
    fn table_load_work_unit_budget_accumulates_across_dsdt_and_ssdt() {
        let dsdt = aml_table_with_nops(b"DSDT", 4);
        let ssdt = aml_table_with_nops(b"SSDT", 4);
        let budget = ExecutionBudget {
            max_operation_opcode_fuel: 6,
            max_operation_duration_ns: 0,
            ..ExecutionBudget::offline_default()
        };

        assert!(
            load_executor(
                &dsdt,
                &[],
                CompatibilityProfile::default(),
                budget,
                FirmwareIdentity::new("01000000000000000200000000000000", 261),
            )
            .is_ok()
        );
        assert_eq!(
            load_executor(
                &dsdt,
                &[&ssdt],
                CompatibilityProfile::default(),
                budget,
                FirmwareIdentity::new("01000000000000000200000000000000", 262),
            )
            .map(|_| ()),
            Err(LoadError::BudgetExceeded)
        );
    }

    #[test]
    fn owned_ssdt_input_is_loaded_without_building_a_slice_reference_vector() {
        let dsdt = aml_table_with_nops(b"DSDT", 4);
        let ssdt = aml_table_with_nops(b"SSDT", 4);
        let ssdts = vec![ssdt.clone()];
        let executor = load_executor_with_owned_backend(
            &dsdt,
            &ssdts,
            CompatibilityProfile::default(),
            ExecutionBudget::offline_default(),
            FirmwareIdentity::new("01000000000000000200000000000000", 263),
            HardwareSideEffectProfile::deny_all(),
            Arc::new(CountingRegionBackend(Arc::new(AtomicU64::new(0)))),
        )
        .expect("owned immutable SSDT tables should load through the metered executor");

        assert_eq!(executor.summary().tables_loaded, 2);

        let limited_budget = ExecutionBudget {
            max_operation_opcode_fuel: 6,
            max_operation_duration_ns: 0,
            ..ExecutionBudget::offline_default()
        };
        assert_eq!(
            load_executor_with_owned_backend(
                &dsdt,
                &[ssdt],
                CompatibilityProfile::default(),
                limited_budget,
                FirmwareIdentity::new("01000000000000000200000000000000", 264),
                HardwareSideEffectProfile::deny_all(),
                Arc::new(CountingRegionBackend(Arc::new(AtomicU64::new(0)))),
            )
            .map(|_| ()),
            Err(LoadError::BudgetExceeded)
        );
    }

    #[test]
    fn predefined_namespace_objects_count_against_the_namespace_budget() {
        let dsdt = aml_table_with_nops(b"DSDT", 0);
        let budget = ExecutionBudget {
            max_namespace_objects: 9,
            ..ExecutionBudget::offline_default()
        };

        assert_eq!(
            load_executor(
                &dsdt,
                &[],
                CompatibilityProfile::default(),
                budget,
                FirmwareIdentity::new("01000000000000000200000000000000", 265),
            )
            .map(|_| ()),
            Err(LoadError::BudgetExceeded)
        );

        let exact_budget = ExecutionBudget {
            max_namespace_objects: 10,
            ..ExecutionBudget::offline_default()
        };
        assert!(
            load_executor(
                &dsdt,
                &[],
                CompatibilityProfile::default(),
                exact_budget,
                FirmwareIdentity::new("01000000000000000200000000000000", 267),
            )
            .is_ok()
        );
    }

    #[test]
    fn predefined_namespace_heap_counts_against_the_heap_budget() {
        let dsdt = aml_table_with_nops(b"DSDT", 0);
        let budget = ExecutionBudget {
            max_heap_bytes: 128,
            ..ExecutionBudget::offline_default()
        };

        assert_eq!(
            load_executor(
                &dsdt,
                &[],
                CompatibilityProfile::default(),
                budget,
                FirmwareIdentity::new("01000000000000000200000000000000", 268),
            )
            .map(|_| ()),
            Err(LoadError::BudgetExceeded)
        );
    }

    #[test]
    fn global_lock_namespace_scan_consumes_the_load_work_unit_loop_budget() {
        let dsdt = aml_table_with_nops(b"DSDT", 0);
        let budget = ExecutionBudget {
            max_operation_loop_iterations: 0,
            ..ExecutionBudget::offline_default()
        };

        assert_eq!(
            load_executor(
                &dsdt,
                &[],
                CompatibilityProfile::default(),
                budget,
                FirmwareIdentity::new("01000000000000000200000000000000", 266),
            )
            .map(|_| ()),
            Err(LoadError::BudgetExceeded)
        );
    }

    #[test]
    fn invalid_pic_profile_is_rejected_before_table_load() {
        assert_eq!(
            load_namespace(
                b"DSDT",
                &[],
                CompatibilityProfile {
                    pic_mode: 2,
                    ..CompatibilityProfile::default()
                },
                ExecutionBudget::offline_default(),
                FirmwareIdentity::new("01000000000000000200000000000000", 1)
            ),
            Err(LoadError::InvalidProfile)
        );
    }

    #[test]
    fn pci_osc_parser_accepts_query_and_tracks_denied_control_bits() {
        let response = [
            0_u32.to_le_bytes(),
            0x0000_0007_u32.to_le_bytes(),
            0x0000_0005_u32.to_le_bytes(),
        ]
        .concat();
        let parsed = parse_pci_osc_response(PciOscPhase::Query, 0x0000_0007, &response).unwrap();

        assert_eq!(parsed.error_bits, 0);
        assert_eq!(parsed.support, 0x0000_0007);
        assert_eq!(parsed.control, 0x0000_0005);
        assert_eq!(parsed.denied_control, 0x0000_0002);
    }

    #[test]
    fn pci_osc_query_flag_echo_is_not_mistaken_for_a_firmware_error() {
        let response = [
            OSC_QUERY_ENABLE.to_le_bytes(),
            0_u32.to_le_bytes(),
            0_u32.to_le_bytes(),
        ]
        .concat();

        let parsed = parse_pci_osc_response(PciOscPhase::Query, 0, &response).unwrap();

        assert_eq!(parsed.error_bits, 0);
    }

    #[test]
    fn pci_osc_parser_rejects_wrong_length_and_non_query_errors() {
        assert_eq!(
            parse_pci_osc_response(PciOscPhase::Commit, 0, &[0; 8]),
            Err(LoadError::OscInvalidResponse)
        );

        let response = [
            (OSC_REQUEST_ERROR | OSC_CAPABILITIES_MASK_ERROR).to_le_bytes(),
            0_u32.to_le_bytes(),
            0_u32.to_le_bytes(),
        ]
        .concat();
        assert_eq!(
            parse_pci_osc_response(PciOscPhase::Commit, 0, &response),
            Err(LoadError::OscRejected {
                error_bits: OSC_REQUEST_ERROR | OSC_CAPABILITIES_MASK_ERROR
            })
        );
    }

    #[test]
    fn pci_osc_keeps_capabilities_mask_as_partial_negotiation() {
        let response = [
            OSC_CAPABILITIES_MASK_ERROR.to_le_bytes(),
            0_u32.to_le_bytes(),
            0_u32.to_le_bytes(),
        ]
        .concat();
        assert!(parse_pci_osc_response(PciOscPhase::Query, 0, &response).is_ok());
        assert!(parse_pci_osc_response(PciOscPhase::Commit, 0, &response).is_ok());
    }

    #[test]
    fn pci_osc_request_rejects_unknown_feature_bits() {
        assert_eq!(
            validate_pci_osc_request(PCI_OSC_SUPPORT_MASK | (1 << 31), 0),
            Err(LoadError::OscInvalidRequest)
        );
        assert_eq!(
            validate_pci_osc_request(0, PCI_OSC_CONTROL_MASK | (1 << 31)),
            Err(LoadError::OscInvalidRequest)
        );
        assert_eq!(
            validate_pci_osc_request(PCI_OSC_SUPPORT_MASK, PCI_OSC_CONTROL_MASK),
            Ok(())
        );
    }

    #[test]
    fn pci_osc_scratch_reservation_is_bounded_and_fails_before_partial_charge() {
        let shortest_path = super::interpreter::pci_osc_working_heap_bytes(0).unwrap();
        let longest_path = super::interpreter::pci_osc_working_heap_bytes(255).unwrap();
        assert!(longest_path > shortest_path);
        assert_eq!(super::interpreter::pci_osc_working_heap_bytes(256), None);
        assert_eq!(
            super::interpreter::pci_osc_working_heap_bytes(usize::MAX),
            None
        );

        let budget = ExecutionBudget {
            max_heap_bytes: longest_path - 1,
            ..ExecutionBudget::offline_default()
        };
        let mut state = ExecutionState::new(budget).unwrap();
        state.begin_work_unit_at(0).unwrap();
        assert_eq!(
            state.reserve_working_heap(longest_path),
            Err(LoadError::BudgetExceeded)
        );
        assert_eq!(state.working_heap_bytes, 0);
    }

    #[test]
    fn gpe_path_scratch_reservation_is_charged_before_dispatch() {
        let scratch_bytes =
            super::interpreter::AcpiExecutor::gpe_path_working_heap_bytes().unwrap();
        let budget = ExecutionBudget {
            max_heap_bytes: scratch_bytes - 1,
            ..ExecutionBudget::offline_default()
        };
        let mut state = ExecutionState::new(budget).unwrap();
        state.begin_work_unit_at(0).unwrap();

        assert_eq!(
            state.reserve_working_heap(scratch_bytes),
            Err(LoadError::BudgetExceeded)
        );
        assert_eq!(state.working_heap_bytes, 0);
    }

    #[test]
    fn pci_osc_requeries_after_masking_and_commits_only_the_negotiated_control() {
        let requested_support = 0x7;
        let requested_control = 0x7;
        let mut calls = Vec::new();
        let exchange = negotiate_pci_osc_exchange(
            requested_support,
            requested_control,
            |query, support, control| {
                calls.push((query, support, control));
                Ok(PciOscResponse {
                    error_bits: if query && control == requested_control {
                        OSC_CAPABILITIES_MASK_ERROR
                    } else {
                        0
                    },
                    support: 0x7,
                    control: 0x5,
                    denied_control: 0,
                })
            },
        )
        .unwrap();

        assert_eq!(
            calls,
            vec![
                (true, requested_support, requested_control),
                (true, requested_support, 0x5),
                (false, requested_support, 0x5),
            ]
        );
        assert_eq!(exchange.query_granted_control, 0x5);
        assert_eq!(exchange.masked_control, 0x2);
        assert_eq!(exchange.granted_control, 0x5);
        assert_eq!(exchange.denied_control, 0x2);
    }

    #[test]
    fn pci_osc_query_masking_that_makes_no_progress_fails_before_commit() {
        let mut calls = Vec::new();
        let result = negotiate_pci_osc_exchange(0x7, 0x5, |query, support, control| {
            calls.push((query, support, control));
            Ok(PciOscResponse {
                error_bits: OSC_CAPABILITIES_MASK_ERROR,
                support,
                control,
                denied_control: 0,
            })
        });

        assert_eq!(
            result,
            Err(LoadError::OscRejected {
                error_bits: OSC_CAPABILITIES_MASK_ERROR,
            })
        );
        assert_eq!(calls, vec![(true, 0x7, 0x5)]);
    }

    #[test]
    fn pci_osc_query_cannot_silently_clear_capabilities_without_mask_error() {
        let mut calls = Vec::new();
        let result = negotiate_pci_osc_exchange(0x7, 0x5, |query, support, control| {
            calls.push((query, support, control));
            Ok(PciOscResponse {
                error_bits: 0,
                support: 0x7,
                control: 0x1,
                denied_control: 0,
            })
        });

        assert_eq!(
            result,
            Err(LoadError::OscRejected {
                error_bits: OSC_CAPABILITIES_MASK_ERROR,
            })
        );
        assert_eq!(calls, vec![(true, 0x7, 0x5)]);
    }

    #[test]
    fn pci_osc_requeries_a_narrowed_support_mask_before_commit() {
        let mut calls = Vec::new();
        let exchange = negotiate_pci_osc_exchange(0x7, 0x3, |query, support, control| {
            calls.push((query, support, control));
            if query && support == 0x7 {
                Ok(PciOscResponse {
                    error_bits: OSC_CAPABILITIES_MASK_ERROR,
                    support: 0x5,
                    control,
                    denied_control: 0,
                })
            } else {
                Ok(PciOscResponse {
                    error_bits: 0,
                    support,
                    control,
                    denied_control: 0,
                })
            }
        })
        .unwrap();

        assert_eq!(
            calls,
            vec![(true, 0x7, 0x3), (true, 0x5, 0x3), (false, 0x5, 0x3)]
        );
        assert_eq!(exchange.query_support, 0x5);
        assert_eq!(exchange.commit_support, 0x5);
        assert_eq!(exchange.granted_control, 0x3);
    }

    #[test]
    fn pci_osc_parser_does_not_propagate_unknown_capability_bits() {
        let response = [
            0_u32.to_le_bytes(),
            (PCI_OSC_SUPPORT_MASK | (1 << 31)).to_le_bytes(),
            (PCI_OSC_CONTROL_MASK | (1 << 31)).to_le_bytes(),
        ]
        .concat();
        let parsed =
            parse_pci_osc_response(PciOscPhase::Commit, PCI_OSC_CONTROL_MASK, &response).unwrap();
        assert_eq!(parsed.support, PCI_OSC_SUPPORT_MASK);
        assert_eq!(parsed.control, PCI_OSC_CONTROL_MASK);
        assert_eq!(parsed.denied_control, 0);
    }

    #[test]
    fn pci_root_resource_windows_preserve_host_apertures_and_validate_bus_range() {
        let windows = [
            PciRootResourceWindow {
                resource_type: 0,
                descriptor_revision: 0,
                general_flags: 0x0c,
                type_specific_flags: 0x03,
                granularity: 0,
                minimum: 0x8000_0000,
                maximum: 0xafff_ffff,
                translation_offset: 0,
                length: 0x3000_0000,
                type_specific_attributes: 0,
            },
            PciRootResourceWindow {
                resource_type: 2,
                descriptor_revision: 0,
                general_flags: 0x0c,
                type_specific_flags: 0,
                granularity: 0,
                minimum: 32,
                maximum: 63,
                translation_offset: 0,
                length: 32,
                type_specific_attributes: 0,
            },
        ];
        assert_eq!(pci_root_bus_window(&windows), Ok(Some((32, 63))));

        let mut overflowing = windows[1];
        overflowing.minimum = u64::MAX;
        overflowing.maximum = u64::MAX;
        overflowing.length = 2;
        assert_eq!(
            pci_root_bus_window(&[overflowing]),
            Err(LoadError::InvalidPciRoot)
        );
    }

    #[test]
    fn metered_pci_root_bus_window_charges_every_resource_window() {
        let windows = [
            PciRootResourceWindow {
                resource_type: 0,
                descriptor_revision: 0,
                general_flags: 0,
                type_specific_flags: 0,
                granularity: 0,
                minimum: 0,
                maximum: 0,
                translation_offset: 0,
                length: 0,
                type_specific_attributes: 0,
            },
            PciRootResourceWindow {
                resource_type: 2,
                descriptor_revision: 0,
                general_flags: 0,
                type_specific_flags: 0,
                granularity: 0,
                minimum: 32,
                maximum: 63,
                length: 32,
                translation_offset: 0,
                type_specific_attributes: 0,
            },
        ];
        let mut charged = 0;
        assert_eq!(
            pci_root_bus_window_with_budget(&windows, || {
                charged += 1;
                Ok(())
            }),
            Ok(Some((32, 63)))
        );
        assert_eq!(charged, windows.len());

        let mut remaining = 0;
        assert_eq!(
            pci_root_bus_window_with_budget(&windows, || {
                if remaining == 0 {
                    return Err(LoadError::BudgetExceeded);
                }
                remaining -= 1;
                Ok(())
            }),
            Err(LoadError::BudgetExceeded)
        );
    }

    #[test]
    fn pci_device_memory_windows_require_fixed_consumer_ranges_and_reject_ambiguity() {
        let memory = PciRootResourceWindow {
            resource_type: 0,
            descriptor_revision: 1,
            general_flags: 0x0d,
            type_specific_flags: 0x06,
            granularity: 0xffff,
            minimum: 0x8000_0000,
            maximum: 0x8000_0fff,
            translation_offset: 0,
            length: 0x1000,
            type_specific_attributes: 0,
        };
        let io = PciRootResourceWindow {
            resource_type: 1,
            ..memory
        };
        let bus = PciRootResourceWindow {
            resource_type: 2,
            ..memory
        };
        let vendor = PciRootResourceWindow {
            resource_type: 0xc0,
            ..memory
        };

        assert_eq!(
            pci_device_memory_windows(&[memory, io, bus, vendor]),
            Ok(vec![PciDeviceResourceWindow {
                base: 0x8000_0000,
                length: 0x1000,
                writable: false,
            }])
        );
        let writable_memory = PciRootResourceWindow {
            type_specific_flags: 0x07,
            ..memory
        };
        assert_eq!(
            pci_device_memory_windows(&[writable_memory]).unwrap()[0].writable,
            true
        );

        let producer = PciRootResourceWindow {
            general_flags: 0x0c,
            ..memory
        };
        assert_eq!(
            pci_device_memory_windows(&[producer]),
            Err(LoadError::InvalidPciRoot)
        );
        let flexible = PciRootResourceWindow {
            general_flags: 0x09,
            ..memory
        };
        assert_eq!(
            pci_device_memory_windows(&[flexible]),
            Err(LoadError::InvalidPciRoot)
        );
        let translated = PciRootResourceWindow {
            translation_offset: 0x1000,
            ..memory
        };
        assert_eq!(
            pci_device_memory_windows(&[translated]),
            Err(LoadError::InvalidPciRoot)
        );

        let mut overlapping = memory;
        overlapping.minimum += 0x800;
        overlapping.maximum += 0x800;
        assert_eq!(
            pci_device_memory_windows(&[memory, overlapping]),
            Err(LoadError::InvalidPciRoot)
        );

        let too_many: Vec<_> = (0..=MAX_PCI_DEVICE_RESOURCE_WINDOWS)
            .map(|index| {
                let base = 0x8000_0000 + (index as u64) * 0x1000;
                PciRootResourceWindow {
                    minimum: base,
                    maximum: base + 0xfff,
                    ..memory
                }
            })
            .collect();
        assert_eq!(
            pci_device_memory_windows(&too_many),
            Err(LoadError::BudgetExceeded)
        );
    }
}
