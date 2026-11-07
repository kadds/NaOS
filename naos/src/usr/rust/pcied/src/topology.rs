use naos_idl::platform_pci_topology::{
    PciRootFeatureSet, PciRootFeatureSupport, PlatformGeneration, PlatformPciResourceWindow,
    PlatformPciRoot,
};
use pci_types::PciAddress;
use std::collections::{BTreeMap, BTreeSet};
use std::ops::Deref;

pub(crate) struct PlatformGenerationSnapshot {
    pub firmware_version: String,
    pub value: PlatformGeneration,
}

impl Deref for PlatformGenerationSnapshot {
    type Target = PlatformGeneration;

    fn deref(&self) -> &Self::Target {
        &self.value
    }
}

#[derive(Clone)]
pub(crate) struct PlatformPciRootSnapshot {
    pub firmware_version: String,
    pub value: PlatformPciRoot,
}

impl Deref for PlatformPciRootSnapshot {
    type Target = PlatformPciRoot;

    fn deref(&self) -> &Self::Target {
        &self.value
    }
}

pub(crate) struct PciFunctionIdentitySnapshot {
    pub firmware_version: String,
    pub value: naos_idl::pci_function::PciFunctionIdentity,
}

impl Deref for PciFunctionIdentitySnapshot {
    type Target = naos_idl::pci_function::PciFunctionIdentity;

    fn deref(&self) -> &Self::Target {
        &self.value
    }
}

/// PlatformPciTopology.list_devices filter bits from the NaoIDL contract:
/// require both ACPI _STA Present and Functioning before driver matching.
pub(crate) const PLATFORM_DEVICE_FILTER_PRESENT_AND_FUNCTIONAL: u32 = (1 << 0) | (1 << 2);

const MAX_PCI_ROOTS: usize = 64;
const MAX_PCI_RESOURCE_WINDOWS_PER_ROOT: usize = 12;
const MAX_PCI_RESOURCE_WINDOWS: usize = MAX_PCI_ROOTS * MAX_PCI_RESOURCE_WINDOWS_PER_ROOT;
pub(crate) const MAX_PCI_FUNCTIONS_PER_FABRIC: usize = 4096;
pub(crate) const MAX_PCI_DEVICE_FUNCTION_PATH: usize = 16;
pub(crate) const MAX_PLATFORM_DEVICE_PAGE: usize = 4;
pub(crate) const MAX_PLATFORM_DEVICE_SNAPSHOTS: usize = 256;
pub(crate) const HEALTH_DEGRADED: u32 = 1 << 0;
pub(crate) const IMPLEMENTED_PCI_ROOT_SUPPORT_MASK: u32 =
    PciRootFeatureSupport::segment_groups as u32;

pub(crate) fn valid_device_page_cursor(
    cursor: u64,
    next_cursor: u64,
    returned_count: usize,
) -> bool {
    next_cursor == 0
        || (returned_count == MAX_PLATFORM_DEVICE_PAGE
            && next_cursor > cursor
            && next_cursor <= MAX_PLATFORM_DEVICE_SNAPSHOTS as u64)
}

pub(crate) fn admit_pci_function(function_count: &mut usize) -> bool {
    let Some(next_count) = function_count.checked_add(1) else {
        return false;
    };
    if next_count > MAX_PCI_FUNCTIONS_PER_FABRIC {
        return false;
    }
    *function_count = next_count;
    true
}

/// Encode the PCI requester ID carried in IOMMU fault records and MSI sources.
pub(crate) fn pci_requester_id(address: PciAddress) -> u16 {
    (u16::from(address.bus()) << 8)
        | (u16::from(address.device()) << 3)
        | u16::from(address.function())
}

pub(crate) fn interrupt_event_matches_generation(
    event: &naos_idl::interrupt_source::InterruptEvent,
    firmware_version: &str,
    generation: &PlatformGenerationSnapshot,
) -> bool {
    interrupt_event_matches_lineage(
        event,
        firmware_version,
        &GenerationLineage::from(generation),
    )
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub(crate) struct GenerationLineage {
    pub firmware_version: String,
    pub aml_namespace_generation: u64,
    pub platform_topology_generation: u64,
    pub pci_fabric_generation: u64,
}

impl From<&PlatformGenerationSnapshot> for GenerationLineage {
    fn from(generation: &PlatformGenerationSnapshot) -> Self {
        Self {
            firmware_version: generation.firmware_version.clone(),
            aml_namespace_generation: generation.aml_namespace_generation,
            platform_topology_generation: generation.platform_topology_generation,
            pci_fabric_generation: generation.pci_fabric_generation,
        }
    }
}

impl From<&PciFunctionIdentitySnapshot> for GenerationLineage {
    fn from(identity: &PciFunctionIdentitySnapshot) -> Self {
        Self {
            firmware_version: identity.firmware_version.clone(),
            aml_namespace_generation: identity.aml_namespace_generation,
            platform_topology_generation: identity.platform_topology_generation,
            pci_fabric_generation: identity.pci_fabric_generation,
        }
    }
}

pub(crate) fn interrupt_event_matches_lineage(
    event: &naos_idl::interrupt_source::InterruptEvent,
    firmware_version: &str,
    lineage: &GenerationLineage,
) -> bool {
    firmware_version == lineage.firmware_version
        && event.aml_namespace_generation == lineage.aml_namespace_generation
        && event.platform_topology_generation == lineage.platform_topology_generation
        && event.pci_fabric_generation == lineage.pci_fabric_generation
}

pub(crate) fn dma_fault_batch_matches_lineage(
    batch: &naos_idl::pci_dma_fault_source::take_response,
    lineage: &GenerationLineage,
) -> bool {
    batch.firmware_version == lineage.firmware_version
        && batch.aml_namespace_generation == lineage.aml_namespace_generation
        && batch.platform_topology_generation == lineage.platform_topology_generation
        && batch.pci_fabric_generation == lineage.pci_fabric_generation
}

#[cfg(feature = "pci-msi-route-smoke")]
pub(crate) fn dma_fault_batch_matches_denied_write(
    batch: &naos_idl::pci_dma_fault_source::take_response,
    expected_requester_id: u16,
    iova: u64,
    byte_length: u64,
) -> bool {
    let Some(end) = iova.checked_add(byte_length) else {
        return false;
    };
    batch.count != 0
        && batch.dropped_records == 0
        && batch.unit_overflow == 0
        && batch.records.iter().all(|fault| {
            fault.iova >= iova
                && fault.iova < end
                && fault.requester_id == expected_requester_id
                && (fault.direction == 1 || fault.direction == 2)
        })
}

/// Race a generation-bound operation against the peer's lifecycle signal.
/// Dropping the operation future also drops every partially built local tree
/// and owned capability in it.
pub async fn race_topology_lifecycle<F, L, T>(operation: F, lifecycle: L) -> Result<T, ()>
where
    F: core::future::Future<Output = T>,
    L: core::future::Future<Output = ()>,
{
    tokio::select! {
        result = operation => Ok(result),
        _ = lifecycle => Err(()),
    }
}

pub(crate) fn intersect_bus_window(
    region_start: u32,
    region_end: u32,
    root_start: u32,
    root_end: u32,
) -> Option<(u32, u32)> {
    if region_start > region_end || root_start > root_end {
        return None;
    }
    let start = region_start.max(root_start);
    let end = region_end.min(root_end);
    (start <= end).then_some((start, end))
}

/// Read one aligned ECAM dword through a volatile load.  Ordinary slice loads
/// are not a valid MMIO access primitive because the compiler may cache or
/// elide reads from device memory.
pub(crate) fn read_ecam_dword(bytes: &[u8], offset: usize) -> u32 {
    if offset % core::mem::align_of::<u32>() != 0 {
        return u32::MAX;
    }
    let Some(end) = offset.checked_add(core::mem::size_of::<u32>()) else {
        return u32::MAX;
    };
    if end > bytes.len() {
        return u32::MAX;
    }

    let pointer = bytes.as_ptr().wrapping_add(offset);
    if (pointer as usize) % core::mem::align_of::<u32>() != 0 {
        return u32::MAX;
    }
    // SAFETY: `offset + sizeof(u32)` is within the supplied ECAM mapping and
    // the pointer is checked for dword alignment. ECAM reads must be volatile.
    u32::from_le(unsafe { pointer.cast::<u32>().read_volatile() })
}

fn valid_address_space_granularity(granularity: u64) -> bool {
    granularity == u64::MAX || granularity.wrapping_add(1).is_power_of_two()
}

fn translated_address_range(
    minimum: u64,
    length: u64,
    translation_offset: u64,
) -> Option<(u64, u64)> {
    let bus_end = minimum.checked_add(length.checked_sub(1)?)?;
    if translation_offset & (1 << 63) == 0 {
        Some((
            minimum.checked_add(translation_offset)?,
            bus_end.checked_add(translation_offset)?,
        ))
    } else {
        let magnitude = translation_offset.wrapping_neg();
        Some((
            minimum.checked_sub(magnitude)?,
            bus_end.checked_sub(magnitude)?,
        ))
    }
}

pub(crate) fn validate_pci_root_window_table(
    roots: &[PlatformPciRoot],
    windows: &[PlatformPciResourceWindow],
    compatibility_profile_hash: u64,
) -> bool {
    if compatibility_profile_hash == 0
        || roots.len() > MAX_PCI_ROOTS
        || windows.len() > MAX_PCI_RESOURCE_WINDOWS
    {
        return false;
    }

    let mut referenced_windows = 0_usize;
    for (root_index, root) in roots.iter().enumerate() {
        if root.root_id == 0
            || root.device_id == 0
            || root.compatibility_profile_hash != compatibility_profile_hash
            || root.segment > u32::from(u16::MAX)
            || root.start_bus > root.end_bus
            || root.end_bus > u32::from(u8::MAX)
            || root.resource_window_count == 0
            || root.resource_window_count as usize > MAX_PCI_RESOURCE_WINDOWS_PER_ROOT
        {
            return false;
        }
        if roots[..root_index].iter().any(|previous| {
            previous.root_id == root.root_id || previous.device_id == root.device_id
        }) {
            return false;
        }

        let start = root.resource_window_start as usize;
        let Some(end) = start.checked_add(root.resource_window_count as usize) else {
            return false;
        };
        if end > windows.len() {
            return false;
        }
        for previous in &roots[..root_index] {
            let previous_start = previous.resource_window_start as usize;
            let Some(previous_end) =
                previous_start.checked_add(previous.resource_window_count as usize)
            else {
                return false;
            };
            if start < previous_end && previous_start < end {
                return false;
            }
        }
        referenced_windows = match referenced_windows.checked_add(end - start) {
            Some(count) => count,
            None => return false,
        };

        let root_windows = &windows[start..end];
        let mut bus_window = None;
        for window in root_windows {
            match window.resource_type {
                0..=2 => {
                    if window.descriptor_revision > 1
                        || window.general_flags & 0xf0 != 0
                        || window.length == 0
                        || window.minimum > window.maximum
                    {
                        return false;
                    }
                    let Some(window_end) = window.minimum.checked_add(window.length - 1) else {
                        return false;
                    };
                    if window_end > window.maximum {
                        return false;
                    }
                    if window.resource_type == 2 {
                        if window.type_specific_flags != 0
                            || window.granularity != 0
                            || window.translation_offset != 0
                            || window.minimum > u64::from(u8::MAX)
                            || window_end > u64::from(u8::MAX)
                            || bus_window
                                .replace((window.minimum as u32, window_end as u32))
                                .is_some()
                        {
                            return false;
                        }
                    } else if !valid_address_space_granularity(window.granularity)
                        || translated_address_range(
                            window.minimum,
                            window.length,
                            window.translation_offset,
                        )
                        .is_none()
                    {
                        return false;
                    }
                }
                192..=255 => {}
                _ => return false,
            }
        }
        if bus_window != Some((root.start_bus, root.end_bus)) {
            return false;
        }
    }
    referenced_windows == windows.len()
}

/// Validate the root lineage and zero-control policy without treating a
/// firmware negotiation error as authority. Incomplete support evidence may
/// still accompany degraded, read-only discovery.
pub(crate) fn pci_root_feature_set_is_bound_and_read_only(
    root: &PlatformPciRoot,
    features: &PciRootFeatureSet,
    firmware_version: &str,
    expected_firmware_version: &str,
) -> bool {
    features.root_id == root.root_id
        && features.requested_support == IMPLEMENTED_PCI_ROOT_SUPPORT_MASK
        && features.requested_control == 0
        && features.query_granted_control == 0
        && features.granted_control == 0
        && features.denied_control == 0
        && features.masked_control == 0
        && !firmware_version.is_empty()
        && firmware_version == expected_firmware_version
        && features.aml_namespace_generation == root.aml_namespace_generation
        && features.platform_topology_generation == root.platform_topology_generation
        && features.compatibility_profile_hash == root.compatibility_profile_hash
        && features.pci_fabric_generation == root.pci_fabric_generation
}

pub(crate) fn pci_root_feature_set_is_complete(features: &PciRootFeatureSet) -> bool {
    features.requested_support == IMPLEMENTED_PCI_ROOT_SUPPORT_MASK
        && features.query_support & !features.requested_support == 0
        && features.commit_support == features.query_support
        && features.query_errors == 0
        && features.commit_errors == 0
}

/// Accept only successful evidence for the current zero-control negotiation.
pub(crate) fn validate_pci_root_feature_set(
    root: &PlatformPciRoot,
    features: &PciRootFeatureSet,
    firmware_version: &str,
    expected_firmware_version: &str,
) -> bool {
    pci_root_feature_set_is_bound_and_read_only(
        root,
        features,
        firmware_version,
        expected_firmware_version,
    ) && pci_root_feature_set_is_complete(features)
}

/// Health bit 0 is informational (for example, `_OSC` denied optional native
/// features) and still permits read-only discovery. AML deadline, platform
/// discovery failure, and unknown bits fail closed before PCI resources bind.
pub(crate) fn health_allows_pci_discovery(health: u32) -> bool {
    health & !HEALTH_DEGRADED == 0
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) struct BridgeBusRange {
    pub address: PciAddress,
    pub primary: u8,
    pub secondary: u8,
    pub subordinate: u8,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum BridgeTopologyError {
    InvalidRootRange,
    BridgeOutsideRoot,
    InvalidBridgeBusNumbers,
    DuplicateFunction,
    DuplicateBridge,
    DuplicateBusRange,
    CrossingBusRanges,
    MissingBridgeFunction,
    UnparentedFunction,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub(crate) struct PlatformPciDeviceFunctionPath {
    pub device_id: u64,
    pub root_id: u64,
    /// Packed PCI device/function values from the host root to this device.
    pub path: Vec<u8>,
    pub compatible_identifiers: Vec<PlatformCompatibleIdentifier>,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum PlatformIdentifierKind {
    Primary,
    Additional,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub(crate) enum PlatformIdentifierValue {
    Text(Vec<u8>),
    Integer(u64),
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub(crate) struct PlatformCompatibleIdentifier {
    pub kind: PlatformIdentifierKind,
    pub value: PlatformIdentifierValue,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum PciDevicePathError {
    InvalidFunctionTopology,
    InvalidPlatformPath,
    InvalidPlatformIdentifiers,
    DuplicatePath,
    StaleDeviceGeneration,
    DuplicateDevice,
}

/// Validate and append the root-relative ACPI `_ADR` paths in one
/// `PlatformPciTopology.list_devices` page. The caller keeps the device-ID set
/// across pages so a repeated or forged device cannot be associated twice.
pub(crate) fn append_platform_device_paths(
    response: &naos_idl::platform_pci_topology::list_devices_response,
    generation: &PlatformGenerationSnapshot,
    root_ids: &BTreeSet<u64>,
    seen_device_ids: &mut BTreeSet<u64>,
    paths: &mut Vec<PlatformPciDeviceFunctionPath>,
) -> Result<(), PciDevicePathError> {
    if response.firmware_version != generation.firmware_version
        || response.devices.len() > MAX_PLATFORM_DEVICE_PAGE
        || response.pci_device_function_path.len()
            > MAX_PLATFORM_DEVICE_PAGE * MAX_PCI_DEVICE_FUNCTION_PATH
        || response.identifiers.len() > MAX_PLATFORM_DEVICE_PAGE * 18
        || response.identifier_text.len() > 18_360
    {
        return Err(PciDevicePathError::InvalidPlatformPath);
    }

    let mut expected_path_start = 0;
    let mut expected_identifier_start = 0;
    let mut expected_identifier_text_start = 0;
    for device in &response.devices {
        if device.device_id == 0 {
            return Err(PciDevicePathError::InvalidPlatformPath);
        }
        if device.namespace_generation != generation.aml_namespace_generation
            || device.platform_topology_generation != generation.platform_topology_generation
            || device.pci_fabric_generation != generation.pci_fabric_generation
        {
            return Err(PciDevicePathError::StaleDeviceGeneration);
        }
        if !seen_device_ids.insert(device.device_id) {
            return Err(PciDevicePathError::DuplicateDevice);
        }
        if seen_device_ids.len() > MAX_PLATFORM_DEVICE_SNAPSHOTS {
            return Err(PciDevicePathError::InvalidPlatformPath);
        }

        let identifier_start = usize::try_from(device.identifier_start)
            .map_err(|_| PciDevicePathError::InvalidPlatformIdentifiers)?;
        let identifier_count = usize::try_from(device.identifier_count)
            .map_err(|_| PciDevicePathError::InvalidPlatformIdentifiers)?;
        let identifier_end = identifier_start
            .checked_add(identifier_count)
            .ok_or(PciDevicePathError::InvalidPlatformIdentifiers)?;
        if identifier_start != expected_identifier_start
            || identifier_count > 18
            || identifier_end > response.identifiers.len()
        {
            return Err(PciDevicePathError::InvalidPlatformIdentifiers);
        }
        expected_identifier_start = identifier_end;
        let mut compatible_identifiers = Vec::with_capacity(identifier_count);
        let mut unique_id_seen = false;
        for identifier in &response.identifiers[identifier_start..identifier_end] {
            let kind = match identifier.kind {
                1 => Some(PlatformIdentifierKind::Primary),
                2 => Some(PlatformIdentifierKind::Additional),
                3 if !unique_id_seen => {
                    unique_id_seen = true;
                    None
                }
                _ => return Err(PciDevicePathError::InvalidPlatformIdentifiers),
            };
            let text_start = usize::try_from(identifier.text_start)
                .map_err(|_| PciDevicePathError::InvalidPlatformIdentifiers)?;
            let text_length = usize::try_from(identifier.text_length)
                .map_err(|_| PciDevicePathError::InvalidPlatformIdentifiers)?;
            let text_end = text_start
                .checked_add(text_length)
                .ok_or(PciDevicePathError::InvalidPlatformIdentifiers)?;
            let value = match identifier.value_kind {
                1 if text_length != 0
                    && text_start == expected_identifier_text_start
                    && text_end <= response.identifier_text.len() =>
                {
                    expected_identifier_text_start = text_end;
                    PlatformIdentifierValue::Text(
                        response.identifier_text[text_start..text_end].to_vec(),
                    )
                }
                2 if text_length == 0 && text_start == expected_identifier_text_start => {
                    PlatformIdentifierValue::Integer(identifier.integer)
                }
                _ => return Err(PciDevicePathError::InvalidPlatformIdentifiers),
            };
            if let Some(kind) = kind {
                compatible_identifiers.push(PlatformCompatibleIdentifier { kind, value });
            }
        }

        let start = usize::try_from(device.pci_path_start)
            .map_err(|_| PciDevicePathError::InvalidPlatformPath)?;
        let count = usize::try_from(device.pci_path_count)
            .map_err(|_| PciDevicePathError::InvalidPlatformPath)?;
        let end = start
            .checked_add(count)
            .ok_or(PciDevicePathError::InvalidPlatformPath)?;
        if start != expected_path_start
            || end > response.pci_device_function_path.len()
            || count > MAX_PCI_DEVICE_FUNCTION_PATH
        {
            return Err(PciDevicePathError::InvalidPlatformPath);
        }
        expected_path_start = end;
        if count == 0 {
            if device.pci_root_id != 0 {
                return Err(PciDevicePathError::InvalidPlatformPath);
            }
            continue;
        }
        if device.pci_root_id == 0 || !root_ids.contains(&device.pci_root_id) {
            return Err(PciDevicePathError::InvalidPlatformPath);
        }

        paths.push(PlatformPciDeviceFunctionPath {
            device_id: device.device_id,
            root_id: device.pci_root_id,
            path: response.pci_device_function_path[start..end].to_vec(),
            compatible_identifiers,
        });
    }

    if expected_path_start != response.pci_device_function_path.len() {
        return Err(PciDevicePathError::InvalidPlatformPath);
    }
    if expected_identifier_start != response.identifiers.len()
        || expected_identifier_text_start != response.identifier_text.len()
    {
        return Err(PciDevicePathError::InvalidPlatformIdentifiers);
    }

    Ok(())
}

/// Derive nearest-bridge parent links from the validated bus-number windows
/// read from the root's PCI-to-PCI and CardBus bridge headers.  The result is
/// parallel to `functions`; `None` denotes a direct child of the host root.
pub(crate) fn assign_bridge_parents(
    root_start: u8,
    root_end: u8,
    functions: &[PciAddress],
    bridges: &[BridgeBusRange],
) -> Result<Vec<Option<PciAddress>>, BridgeTopologyError> {
    if root_start > root_end {
        return Err(BridgeTopologyError::InvalidRootRange);
    }

    let function_set: BTreeSet<_> = functions.iter().copied().collect();
    if function_set.len() != functions.len() {
        return Err(BridgeTopologyError::DuplicateFunction);
    }

    let mut ordered = bridges.to_vec();
    ordered.sort_unstable_by_key(|bridge| {
        (
            bridge.secondary,
            std::cmp::Reverse(bridge.subordinate),
            bridge.address,
        )
    });

    let mut bridge_addresses = BTreeSet::new();
    let mut interval_stack = Vec::new();
    for bridge in &ordered {
        if bridge.secondary < root_start || bridge.subordinate > root_end {
            return Err(BridgeTopologyError::BridgeOutsideRoot);
        }
        if bridge.address.bus() < root_start
            || bridge.address.bus() > root_end
            || bridge.primary != bridge.address.bus()
            || bridge.secondary <= bridge.primary
            || bridge.subordinate < bridge.secondary
        {
            return Err(BridgeTopologyError::InvalidBridgeBusNumbers);
        }
        if !function_set.contains(&bridge.address) {
            return Err(BridgeTopologyError::MissingBridgeFunction);
        }
        if !bridge_addresses.insert(bridge.address) {
            return Err(BridgeTopologyError::DuplicateBridge);
        }

        while interval_stack
            .last()
            .is_some_and(|parent: &BridgeBusRange| parent.subordinate < bridge.secondary)
        {
            interval_stack.pop();
        }
        if let Some(parent) = interval_stack.last() {
            if bridge.subordinate > parent.subordinate {
                return Err(BridgeTopologyError::CrossingBusRanges);
            }
            if bridge.secondary == parent.secondary && bridge.subordinate == parent.subordinate {
                return Err(BridgeTopologyError::DuplicateBusRange);
            }
        }
        interval_stack.push(*bridge);
    }

    let mut parents_by_bus = [None; 256];
    let mut active = Vec::new();
    let mut next_bridge = 0;
    for bus in root_start..=root_end {
        while active
            .last()
            .is_some_and(|bridge: &BridgeBusRange| bridge.subordinate < bus)
        {
            active.pop();
        }
        while ordered
            .get(next_bridge)
            .is_some_and(|bridge| bridge.secondary == bus)
        {
            active.push(ordered[next_bridge]);
            next_bridge += 1;
        }
        parents_by_bus[usize::from(bus)] = active.last().map(|bridge| bridge.address);
    }

    let mut parents = Vec::with_capacity(functions.len());
    for function in functions {
        let parent = parents_by_bus[usize::from(function.bus())];
        if function.bus() != root_start && parent.is_none() {
            return Err(BridgeTopologyError::UnparentedFunction);
        }
        parents.push(parent);
    }
    Ok(parents)
}

fn pci_function_address_path(
    root_start: u8,
    address: PciAddress,
    functions: &[PciAddress],
    parents: &[Option<PciAddress>],
) -> Result<Option<Vec<u8>>, PciDevicePathError> {
    if functions.len() != parents.len() || !functions.contains(&address) {
        return Err(PciDevicePathError::InvalidFunctionTopology);
    }

    let segment = address.segment();
    let mut current = address;
    let mut seen = BTreeSet::new();
    let mut path = Vec::new();
    loop {
        if path.len() == MAX_PCI_DEVICE_FUNCTION_PATH || !seen.insert(current) {
            return Err(PciDevicePathError::InvalidFunctionTopology);
        }
        let Some(index) = functions.iter().position(|function| *function == current) else {
            return Err(PciDevicePathError::InvalidFunctionTopology);
        };
        path.push(current.device() | (current.function() << 5));
        match parents[index] {
            Some(parent) if parent.segment() == segment && parent.bus() < current.bus() => {
                current = parent;
            }
            Some(_) => return Err(PciDevicePathError::InvalidFunctionTopology),
            None if current.bus() == root_start => break,
            // MCFG may begin below the ACPI root's start bus. The function is
            // still safe to discover, but its missing upstream bridge path
            // prevents an ACPI device identity claim.
            None => return Ok(None),
        }
    }
    path.reverse();
    Ok(Some(path))
}

pub(crate) fn associate_platform_device_paths(
    root_id: u64,
    root_start: u8,
    functions: &[PciAddress],
    parents: &[Option<PciAddress>],
    platform_paths: &[PlatformPciDeviceFunctionPath],
) -> Result<Vec<Option<u64>>, PciDevicePathError> {
    if root_id == 0 || functions.len() != parents.len() {
        return Err(PciDevicePathError::InvalidFunctionTopology);
    }

    let mut path_to_device = BTreeMap::new();
    for platform_path in platform_paths.iter().filter(|path| path.root_id == root_id) {
        if platform_path.device_id == 0
            || platform_path.path.is_empty()
            || platform_path.path.len() > MAX_PCI_DEVICE_FUNCTION_PATH
        {
            return Err(PciDevicePathError::InvalidPlatformPath);
        }
        if path_to_device
            .insert(platform_path.path.as_slice(), platform_path.device_id)
            .is_some()
        {
            return Err(PciDevicePathError::DuplicatePath);
        }
    }

    let mut matched_device_ids = BTreeSet::new();
    functions
        .iter()
        .map(|function| {
            let Some(path) = pci_function_address_path(root_start, *function, functions, parents)?
            else {
                return Ok(None);
            };
            let device_id = path_to_device.get(path.as_slice()).copied();
            if device_id.is_some_and(|device_id| !matched_device_ids.insert(device_id)) {
                return Err(PciDevicePathError::DuplicatePath);
            }
            Ok(device_id)
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::{
        BridgeBusRange, BridgeTopologyError, GenerationLineage, MAX_PCI_FUNCTIONS_PER_FABRIC,
        PciDevicePathError, PlatformCompatibleIdentifier, PlatformGenerationSnapshot,
        PlatformIdentifierKind, PlatformIdentifierValue, PlatformPciDeviceFunctionPath,
        admit_pci_function, append_platform_device_paths, assign_bridge_parents,
        associate_platform_device_paths, dma_fault_batch_matches_lineage,
        interrupt_event_matches_generation, intersect_bus_window, pci_requester_id,
        read_ecam_dword, valid_device_page_cursor, validate_pci_root_feature_set,
        validate_pci_root_window_table,
    };

    const TEST_FIRMWARE_VERSION: &str = "01000000000000000200000000000000";
    use naos_idl::platform_pci_topology::{
        PciRootFeatureSet, PciRootFeatureSupport, PlatformGeneration, PlatformPciResourceWindow,
        PlatformPciRoot,
    };
    use pci_types::PciAddress;
    use std::collections::BTreeSet;

    #[test]
    fn filtered_device_pages_allow_source_cursor_gaps_but_require_progress() {
        assert!(valid_device_page_cursor(
            0,
            7,
            super::MAX_PLATFORM_DEVICE_PAGE
        ));
        assert!(valid_device_page_cursor(7, 0, 1));
        assert!(!valid_device_page_cursor(
            7,
            7,
            super::MAX_PLATFORM_DEVICE_PAGE
        ));
        assert!(!valid_device_page_cursor(
            7,
            8,
            super::MAX_PLATFORM_DEVICE_PAGE - 1
        ));
        assert!(!valid_device_page_cursor(
            7,
            super::MAX_PLATFORM_DEVICE_SNAPSHOTS as u64 + 1,
            super::MAX_PLATFORM_DEVICE_PAGE
        ));
    }

    #[cfg(feature = "pci-msi-route-smoke")]
    #[test]
    fn denied_write_fault_matcher_accepts_specified_or_unknown_direction_only() {
        use super::dma_fault_batch_matches_denied_write;
        use naos_idl::pci_dma_fault_source::{PciDmaFaultRecord, take_response};

        let make_batch = |direction| take_response {
            sequence: 1,
            count: 1,
            dropped_records: 0,
            records: vec![PciDmaFaultRecord {
                iova: 0x5000,
                requester_id: 0x1234,
                reason: 2,
                direction,
            }],
            unit_overflow: 0,
            firmware_version: TEST_FIRMWARE_VERSION,
            aml_namespace_generation: 3,
            platform_topology_generation: 4,
            pci_fabric_generation: 5,
        };

        for direction in [1, 2] {
            assert!(dma_fault_batch_matches_denied_write(
                &make_batch(direction),
                0x1234,
                0x5000,
                0x1000,
            ));
        }
        assert!(!dma_fault_batch_matches_denied_write(
            &make_batch(0),
            0x1234,
            0x5000,
            0x1000,
        ));
    }

    #[test]
    fn pci_requester_id_encodes_bus_device_and_function_without_segment() {
        assert_eq!(pci_requester_id(PciAddress::new(0, 0, 3, 0)), 0x0018);
        assert_eq!(pci_requester_id(PciAddress::new(0, 0, 4, 0)), 0x0020);
        assert_eq!(pci_requester_id(PciAddress::new(2, 7, 6, 3)), 0x0733);
    }

    #[test]
    fn interrupt_event_must_match_all_five_incarnation_values() {
        let generation = expected_platform_generation();
        let mut event = naos_idl::interrupt_source::InterruptEvent {
            sequence: 1,
            flags: 1,
            aml_namespace_generation: generation.aml_namespace_generation,
            platform_topology_generation: generation.platform_topology_generation,
            pci_fabric_generation: generation.pci_fabric_generation,
        };
        assert!(interrupt_event_matches_generation(
            &event,
            TEST_FIRMWARE_VERSION,
            &generation
        ));

        event.pci_fabric_generation += 1;
        assert!(!interrupt_event_matches_generation(
            &event,
            TEST_FIRMWARE_VERSION,
            &generation
        ));
    }

    #[test]
    fn dma_fault_batch_must_match_all_five_incarnation_values() {
        let lineage = GenerationLineage {
            firmware_version: TEST_FIRMWARE_VERSION.to_owned(),
            aml_namespace_generation: 13,
            platform_topology_generation: 14,
            pci_fabric_generation: 15,
        };
        let mut batch = naos_idl::pci_dma_fault_source::take_response {
            sequence: 1,
            count: 0,
            dropped_records: 0,
            unit_overflow: 0,
            firmware_version: TEST_FIRMWARE_VERSION,
            aml_namespace_generation: lineage.aml_namespace_generation,
            platform_topology_generation: lineage.platform_topology_generation,
            pci_fabric_generation: lineage.pci_fabric_generation,
            records: Vec::new(),
        };
        assert!(dma_fault_batch_matches_lineage(&batch, &lineage));

        batch.firmware_version = "stale";
        assert!(!dma_fault_batch_matches_lineage(&batch, &lineage));
        batch.firmware_version = TEST_FIRMWARE_VERSION;
        batch.aml_namespace_generation += 1;
        assert!(!dma_fault_batch_matches_lineage(&batch, &lineage));
        batch.aml_namespace_generation -= 1;
        batch.platform_topology_generation += 1;
        assert!(!dma_fault_batch_matches_lineage(&batch, &lineage));
        batch.platform_topology_generation -= 1;
        batch.pci_fabric_generation += 1;
        assert!(!dma_fault_batch_matches_lineage(&batch, &lineage));
    }

    #[test]
    fn pci_function_inventory_has_a_fixed_admission_limit() {
        let mut count = 0;
        for _ in 0..MAX_PCI_FUNCTIONS_PER_FABRIC - 1 {
            assert!(admit_pci_function(&mut count));
        }
        assert_eq!(count, MAX_PCI_FUNCTIONS_PER_FABRIC - 1);
        assert!(admit_pci_function(&mut count));
        assert_eq!(count, MAX_PCI_FUNCTIONS_PER_FABRIC);
        assert!(!admit_pci_function(&mut count));
        assert_eq!(count, MAX_PCI_FUNCTIONS_PER_FABRIC);

        let mut overflowing_count = usize::MAX;
        assert!(!admit_pci_function(&mut overflowing_count));
        assert_eq!(overflowing_count, usize::MAX);
    }

    fn root(resource_window_count: u32) -> PlatformPciRoot {
        PlatformPciRoot {
            root_id: 1,
            segment: 0,
            start_bus: 32,
            end_bus: 63,
            aml_namespace_generation: 3,
            platform_topology_generation: 4,
            pci_fabric_generation: 5,
            device_id: 6,
            resource_window_start: 0,
            resource_window_count,
            compatibility_profile_hash: 7,
        }
    }

    fn window(
        resource_type: u32,
        minimum: u64,
        maximum: u64,
        length: u64,
    ) -> PlatformPciResourceWindow {
        PlatformPciResourceWindow {
            resource_type,
            descriptor_revision: 0,
            general_flags: 0,
            type_specific_flags: 0,
            granularity: 0,
            minimum,
            maximum,
            translation_offset: 0,
            length,
            type_specific_attributes: 0,
        }
    }

    fn feature_set(root: &PlatformPciRoot) -> PciRootFeatureSet {
        let support_mask = PciRootFeatureSupport::segment_groups as u32;
        PciRootFeatureSet {
            root_id: root.root_id,
            requested_support: support_mask,
            requested_control: 0,
            query_support: support_mask,
            granted_control: 0,
            denied_control: 0,
            masked_control: 0,
            query_errors: 0,
            commit_errors: 0,
            aml_namespace_generation: root.aml_namespace_generation,
            platform_topology_generation: root.platform_topology_generation,
            pci_fabric_generation: root.pci_fabric_generation,
            compatibility_profile_hash: root.compatibility_profile_hash,
            query_granted_control: 0,
            commit_support: support_mask,
        }
    }

    #[test]
    fn read_only_feature_result_is_bound_to_root_lineage_and_cannot_grant_controls() {
        let platform_root = root(2);
        let feature = feature_set(&platform_root);
        assert!(validate_pci_root_feature_set(
            &platform_root,
            &feature,
            TEST_FIRMWARE_VERSION,
            TEST_FIRMWARE_VERSION
        ));

        assert!(!validate_pci_root_feature_set(
            &platform_root,
            &feature,
            "stale",
            TEST_FIRMWARE_VERSION
        ));

        let mut stale_fabric = feature;
        stale_fabric.pci_fabric_generation += 1;
        assert!(!validate_pci_root_feature_set(
            &platform_root,
            &stale_fabric,
            TEST_FIRMWARE_VERSION,
            TEST_FIRMWARE_VERSION
        ));

        let mut unadvertised = feature;
        unadvertised.requested_support = 0;
        assert!(!validate_pci_root_feature_set(
            &platform_root,
            &unadvertised,
            TEST_FIRMWARE_VERSION,
            TEST_FIRMWARE_VERSION
        ));

        let mut unexpected_grant = feature;
        unexpected_grant.granted_control = 1 << 2;
        assert!(!validate_pci_root_feature_set(
            &platform_root,
            &unexpected_grant,
            TEST_FIRMWARE_VERSION,
            TEST_FIRMWARE_VERSION
        ));

        let mut query_support_mismatch = feature;
        query_support_mismatch.query_support = 0;
        assert!(!validate_pci_root_feature_set(
            &platform_root,
            &query_support_mismatch,
            TEST_FIRMWARE_VERSION,
            TEST_FIRMWARE_VERSION
        ));

        let mut narrowed_support = feature;
        narrowed_support.query_support = 0;
        narrowed_support.commit_support = 0;
        assert!(validate_pci_root_feature_set(
            &platform_root,
            &narrowed_support,
            TEST_FIRMWARE_VERSION,
            TEST_FIRMWARE_VERSION
        ));

        let mut unrequested_support = feature;
        unrequested_support.query_support |= 1 << 31;
        assert!(!validate_pci_root_feature_set(
            &platform_root,
            &unrequested_support,
            TEST_FIRMWARE_VERSION,
            TEST_FIRMWARE_VERSION
        ));

        let mut commit_support_mismatch = feature;
        commit_support_mismatch.commit_support = 0;
        assert!(!validate_pci_root_feature_set(
            &platform_root,
            &commit_support_mismatch,
            TEST_FIRMWARE_VERSION,
            TEST_FIRMWARE_VERSION
        ));

        let mut query_error = feature;
        query_error.query_errors = 1;
        assert!(!validate_pci_root_feature_set(
            &platform_root,
            &query_error,
            TEST_FIRMWARE_VERSION,
            TEST_FIRMWARE_VERSION
        ));

        let mut commit_error = feature;
        commit_error.commit_errors = 1;
        assert!(!validate_pci_root_feature_set(
            &platform_root,
            &commit_error,
            TEST_FIRMWARE_VERSION,
            TEST_FIRMWARE_VERSION
        ));
    }

    #[test]
    fn validates_root_window_ownership_and_bus_aperture() {
        let roots = [root(2)];
        let windows = [
            window(0, 0x8000_0000, 0x8fff_ffff, 0x1000_0000),
            window(2, 32, 63, 32),
        ];
        assert!(validate_pci_root_window_table(&roots, &windows, 7));

        let wrong_bus = [windows[0], window(2, 16, 31, 16)];
        assert!(!validate_pci_root_window_table(&roots, &wrong_bus, 7));
        assert!(!validate_pci_root_window_table(&[root(13)], &windows, 7));
        assert!(!validate_pci_root_window_table(&[root(3)], &windows, 7));

        let mut stale_profile = root(2);
        stale_profile.compatibility_profile_hash = 8;
        assert!(!validate_pci_root_window_table(
            &[stale_profile],
            &windows,
            7
        ));

        let overflow = [window(0, u64::MAX, u64::MAX, 2), windows[1]];
        assert!(!validate_pci_root_window_table(&roots, &overflow, 7));
    }

    #[test]
    fn rejects_invalid_root_window_translation_granularity_and_reserved_flags() {
        let roots = [root(2)];
        let valid = [
            window(0, 0x8000_0000, 0x8fff_ffff, 0x1000_0000),
            window(2, 32, 63, 32),
        ];

        let mut positive_translation = valid;
        positive_translation[0].translation_offset = 0x1000;
        assert!(validate_pci_root_window_table(
            &roots,
            &positive_translation,
            7
        ));

        let mut negative_translation = valid;
        negative_translation[0].minimum = 0x1000;
        negative_translation[0].maximum = 0x1fff;
        negative_translation[0].length = 0x1000;
        negative_translation[0].translation_offset = u64::MAX - 0xfff;
        assert!(validate_pci_root_window_table(
            &roots,
            &negative_translation,
            7
        ));

        let mut positive_translation_overflow = valid;
        positive_translation_overflow[0].minimum = u64::MAX;
        positive_translation_overflow[0].maximum = u64::MAX;
        positive_translation_overflow[0].length = 1;
        positive_translation_overflow[0].translation_offset = 1;
        assert!(!validate_pci_root_window_table(
            &roots,
            &positive_translation_overflow,
            7
        ));

        let mut negative_translation_underflow = valid;
        negative_translation_underflow[0].minimum = 0;
        negative_translation_underflow[0].maximum = 0;
        negative_translation_underflow[0].length = 1;
        negative_translation_underflow[0].translation_offset = u64::MAX;
        assert!(!validate_pci_root_window_table(
            &roots,
            &negative_translation_underflow,
            7
        ));

        let mut invalid_granularity = valid;
        invalid_granularity[0].granularity = 0x5;
        assert!(!validate_pci_root_window_table(
            &roots,
            &invalid_granularity,
            7
        ));

        let mut reserved_general_flags = valid;
        reserved_general_flags[0].general_flags = 0x10;
        assert!(!validate_pci_root_window_table(
            &roots,
            &reserved_general_flags,
            7
        ));

        let mut bus_window_translation = valid;
        bus_window_translation[1].translation_offset = 1;
        assert!(!validate_pci_root_window_table(
            &roots,
            &bus_window_translation,
            7
        ));
    }

    #[test]
    fn clips_a_platform_root_to_the_discovery_region() {
        assert_eq!(intersect_bus_window(0, 255, 32, 63), Some((32, 63)));
        assert_eq!(intersect_bus_window(32, 63, 0, 255), Some((32, 63)));
        assert_eq!(intersect_bus_window(0, 127, 96, 255), Some((96, 127)));
    }

    #[test]
    fn rejects_disjoint_or_malformed_bus_windows() {
        assert_eq!(intersect_bus_window(0, 31, 32, 63), None);
        assert_eq!(intersect_bus_window(64, 32, 0, 255), None);
        assert_eq!(intersect_bus_window(0, 255, 64, 32), None);
    }

    #[test]
    fn builds_nearest_parent_links_for_nested_pci_bridges() {
        let root_bridge = PciAddress::new(0, 0, 1, 0);
        let endpoint = PciAddress::new(0, 0, 2, 0);
        let nested_bridge = PciAddress::new(0, 1, 3, 0);
        let nested_endpoint = PciAddress::new(0, 2, 4, 0);
        let functions = [root_bridge, endpoint, nested_bridge, nested_endpoint];
        let bridges = [
            BridgeBusRange {
                address: root_bridge,
                primary: 0,
                secondary: 1,
                subordinate: 3,
            },
            BridgeBusRange {
                address: nested_bridge,
                primary: 1,
                secondary: 2,
                subordinate: 3,
            },
        ];

        assert_eq!(
            assign_bridge_parents(0, 3, &functions, &bridges),
            Ok(vec![None, None, Some(root_bridge), Some(nested_bridge)])
        );
    }

    #[test]
    fn associates_platform_adr_paths_with_the_scanned_pci_tree() {
        let root_bridge = PciAddress::new(0, 0, 1, 0);
        let root_endpoint = PciAddress::new(0, 0, 2, 0);
        let nested_bridge = PciAddress::new(0, 1, 3, 1);
        let nested_endpoint = PciAddress::new(0, 2, 4, 2);
        let functions = [root_bridge, root_endpoint, nested_bridge, nested_endpoint];
        let bridges = [
            BridgeBusRange {
                address: root_bridge,
                primary: 0,
                secondary: 1,
                subordinate: 3,
            },
            BridgeBusRange {
                address: nested_bridge,
                primary: 1,
                secondary: 2,
                subordinate: 3,
            },
        ];
        let parents = assign_bridge_parents(0, 3, &functions, &bridges).unwrap();
        let platform_paths = [
            PlatformPciDeviceFunctionPath {
                device_id: 101,
                root_id: 7,
                path: vec![1],
                compatible_identifiers: Vec::new(),
            },
            PlatformPciDeviceFunctionPath {
                device_id: 102,
                root_id: 7,
                path: vec![2],
                compatible_identifiers: Vec::new(),
            },
            PlatformPciDeviceFunctionPath {
                device_id: 103,
                root_id: 7,
                path: vec![1, 0x23],
                compatible_identifiers: Vec::new(),
            },
            PlatformPciDeviceFunctionPath {
                device_id: 104,
                root_id: 7,
                path: vec![1, 0x23, 0x44],
                compatible_identifiers: Vec::new(),
            },
            PlatformPciDeviceFunctionPath {
                device_id: 201,
                root_id: 8,
                path: vec![1],
                compatible_identifiers: Vec::new(),
            },
        ];

        assert_eq!(
            associate_platform_device_paths(7, 0, &functions, &parents, &platform_paths),
            Ok(vec![Some(101), Some(102), Some(103), Some(104)])
        );
    }

    #[test]
    fn rejects_ambiguous_platform_adr_paths() {
        let function = PciAddress::new(0, 0, 1, 0);
        let paths = [
            PlatformPciDeviceFunctionPath {
                device_id: 1,
                root_id: 7,
                path: vec![1],
                compatible_identifiers: Vec::new(),
            },
            PlatformPciDeviceFunctionPath {
                device_id: 2,
                root_id: 7,
                path: vec![1],
                compatible_identifiers: Vec::new(),
            },
        ];
        assert_eq!(
            associate_platform_device_paths(7, 0, &[function], &[None], &paths),
            Err(PciDevicePathError::DuplicatePath)
        );
    }

    #[test]
    fn partial_mcfg_intersection_keeps_unmatched_functions_without_guessing_root_paths() {
        let bridge = PciAddress::new(0, 96, 1, 0);
        let endpoint = PciAddress::new(0, 97, 2, 0);
        let functions = [bridge, endpoint];
        let parents = [None, Some(bridge)];
        let platform_paths = [
            PlatformPciDeviceFunctionPath {
                device_id: 205,
                root_id: 7,
                path: vec![1],
                compatible_identifiers: Vec::new(),
            },
            PlatformPciDeviceFunctionPath {
                device_id: 206,
                root_id: 7,
                path: vec![1, 2],
                compatible_identifiers: Vec::new(),
            },
        ];

        assert_eq!(
            associate_platform_device_paths(7, 0, &functions, &parents, &platform_paths),
            Ok(vec![None, None])
        );
    }

    #[test]
    fn partial_mcfg_association_still_rejects_a_non_ancestor_parent_link() {
        let child = PciAddress::new(0, 96, 1, 0);
        let later_bus = PciAddress::new(0, 97, 2, 0);
        let functions = [child, later_bus];
        let parents = [Some(later_bus), None];
        let paths = [PlatformPciDeviceFunctionPath {
            device_id: 207,
            root_id: 7,
            path: vec![1],
            compatible_identifiers: Vec::new(),
        }];

        assert_eq!(
            associate_platform_device_paths(7, 0, &functions, &parents, &paths),
            Err(PciDevicePathError::InvalidFunctionTopology)
        );
    }

    fn platform_device_page(
        device_id: u64,
        namespace_generation: u64,
        root_id: u64,
        path_start: u32,
        path_count: u32,
        path: Vec<u8>,
    ) -> naos_idl::platform_pci_topology::list_devices_response<'static> {
        use naos_idl::platform_pci_topology as idl;

        idl::list_devices_response {
            firmware_version: TEST_FIRMWARE_VERSION,
            devices: vec![idl::PlatformDeviceSnapshot {
                device_id,
                namespace_generation,
                platform_topology_generation: 4,
                pci_fabric_generation: 5,
                status: 0,
                evaluation_flags: 0,
                initialized: 0,
                degraded: 0,
                identifier_start: 0,
                identifier_count: 0,
                resource_start: 0,
                resource_count: 0,
                routing_entries: 0,
                pci_root_id: root_id,
                pci_path_start: path_start,
                pci_path_count: path_count,
            }],
            identifiers: Vec::new(),
            resources: Vec::new(),
            identifier_text: Vec::new(),
            next_cursor: 0,
            pci_device_function_path: path,
        }
    }

    #[test]
    fn platform_device_snapshot_carries_full_incarnation_lineage() {
        let response = platform_device_page(42, 3, 7, 0, 1, vec![1]);
        assert_eq!(response.firmware_version, TEST_FIRMWARE_VERSION);
        let device = &response.devices[0];
        assert_eq!(device.namespace_generation, 3);
        assert_eq!(device.platform_topology_generation, 4);
        assert_eq!(device.pci_fabric_generation, 5);
    }

    fn expected_platform_generation() -> PlatformGenerationSnapshot {
        PlatformGenerationSnapshot {
            firmware_version: TEST_FIRMWARE_VERSION.to_owned(),
            value: PlatformGeneration {
                aml_namespace_generation: 3,
                platform_topology_generation: 4,
                pci_fabric_generation: 5,
                health: 0,
                compatibility_profile_hash: 6,
            },
        }
    }

    #[test]
    fn preserves_normalized_platform_compatible_identifiers_for_pci_binding() {
        use naos_idl::platform_pci_topology as idl;
        use std::collections::BTreeSet;

        let mut page = platform_device_page(42, 3, 7, 0, 1, vec![1]);
        page.devices[0].identifier_count = 3;
        page.identifiers = vec![
            idl::PlatformDeviceIdentifier {
                kind: 1,
                value_kind: 1,
                text_start: 0,
                text_length: 8,
                integer: 0,
            },
            idl::PlatformDeviceIdentifier {
                kind: 2,
                value_kind: 1,
                text_start: 8,
                text_length: 8,
                integer: 0,
            },
            idl::PlatformDeviceIdentifier {
                kind: 3,
                value_kind: 2,
                text_start: 16,
                text_length: 0,
                integer: 42,
            },
        ];
        page.identifier_text = b"QEMU0002QEMU0003".to_vec();

        let roots = BTreeSet::from([7]);
        let mut seen = BTreeSet::new();
        let mut paths = Vec::new();
        append_platform_device_paths(
            &page,
            &expected_platform_generation(),
            &roots,
            &mut seen,
            &mut paths,
        )
        .unwrap();

        assert_eq!(
            paths[0].compatible_identifiers,
            [
                PlatformCompatibleIdentifier {
                    kind: PlatformIdentifierKind::Primary,
                    value: PlatformIdentifierValue::Text(b"QEMU0002".to_vec()),
                },
                PlatformCompatibleIdentifier {
                    kind: PlatformIdentifierKind::Additional,
                    value: PlatformIdentifierValue::Text(b"QEMU0003".to_vec()),
                },
            ]
        );

        let mut duplicate_uid_page = page.clone();
        duplicate_uid_page.devices[0].device_id = 43;
        duplicate_uid_page.devices[0].identifier_count = 4;
        duplicate_uid_page
            .identifiers
            .push(idl::PlatformDeviceIdentifier {
                kind: 3,
                value_kind: 1,
                text_start: 16,
                text_length: 1,
                integer: 0,
            });
        duplicate_uid_page.identifier_text.push(b'X');
        let mut seen = BTreeSet::new();
        let mut paths = Vec::new();
        assert_eq!(
            append_platform_device_paths(
                &duplicate_uid_page,
                &expected_platform_generation(),
                &roots,
                &mut seen,
                &mut paths,
            ),
            Err(PciDevicePathError::InvalidPlatformIdentifiers)
        );
    }

    #[test]
    fn preserves_integer_compatible_identifiers_and_rejects_bad_text_offsets() {
        use naos_idl::platform_pci_topology as idl;
        use std::collections::BTreeSet;

        let mut integer_page = platform_device_page(46, 3, 7, 0, 1, vec![1]);
        integer_page.devices[0].identifier_count = 1;
        integer_page
            .identifiers
            .push(idl::PlatformDeviceIdentifier {
                kind: 1,
                value_kind: 2,
                text_start: 0,
                text_length: 0,
                integer: 0x030a_d041,
            });
        let roots = BTreeSet::from([7]);
        let mut seen = BTreeSet::new();
        let mut paths = Vec::new();
        append_platform_device_paths(
            &integer_page,
            &expected_platform_generation(),
            &roots,
            &mut seen,
            &mut paths,
        )
        .unwrap();
        assert_eq!(
            paths[0].compatible_identifiers,
            [PlatformCompatibleIdentifier {
                kind: PlatformIdentifierKind::Primary,
                value: PlatformIdentifierValue::Integer(0x030a_d041),
            }]
        );

        let mut malformed_page = platform_device_page(47, 3, 7, 0, 1, vec![1]);
        malformed_page.devices[0].identifier_count = 1;
        malformed_page
            .identifiers
            .push(idl::PlatformDeviceIdentifier {
                kind: 1,
                value_kind: 1,
                text_start: 1,
                text_length: 1,
                integer: 0,
            });
        malformed_page.identifier_text.push(b'X');
        let mut seen = BTreeSet::new();
        let mut paths = Vec::new();
        assert_eq!(
            append_platform_device_paths(
                &malformed_page,
                &expected_platform_generation(),
                &roots,
                &mut seen,
                &mut paths
            ),
            Err(PciDevicePathError::InvalidPlatformIdentifiers)
        );
    }

    #[test]
    fn validates_platform_device_path_slices_and_generation_before_association() {
        use std::collections::BTreeSet;

        let roots = BTreeSet::from([7]);
        let mut seen = BTreeSet::new();
        let mut paths = Vec::new();
        let valid = platform_device_page(42, 3, 7, 0, 2, vec![1, 0x23]);
        append_platform_device_paths(
            &valid,
            &expected_platform_generation(),
            &roots,
            &mut seen,
            &mut paths,
        )
        .unwrap();
        assert_eq!(
            paths,
            [PlatformPciDeviceFunctionPath {
                device_id: 42,
                root_id: 7,
                path: vec![1, 0x23],
                compatible_identifiers: Vec::new(),
            }]
        );

        let mut seen = BTreeSet::new();
        let mut paths = Vec::new();
        let bad_range = platform_device_page(43, 3, 7, 1, 2, vec![1, 0x23]);
        assert_eq!(
            append_platform_device_paths(
                &bad_range,
                &expected_platform_generation(),
                &roots,
                &mut seen,
                &mut paths
            ),
            Err(PciDevicePathError::InvalidPlatformPath)
        );

        let mut seen = BTreeSet::new();
        let mut paths = Vec::new();
        let unknown_root = platform_device_page(44, 3, 8, 0, 1, vec![1]);
        assert_eq!(
            append_platform_device_paths(
                &unknown_root,
                &expected_platform_generation(),
                &roots,
                &mut seen,
                &mut paths
            ),
            Err(PciDevicePathError::InvalidPlatformPath)
        );

        let mut seen = BTreeSet::new();
        let mut paths = Vec::new();
        let stale = platform_device_page(45, 2, 7, 0, 1, vec![1]);
        assert_eq!(
            append_platform_device_paths(
                &stale,
                &expected_platform_generation(),
                &roots,
                &mut seen,
                &mut paths
            ),
            Err(PciDevicePathError::StaleDeviceGeneration)
        );
    }

    #[test]
    fn rejects_device_snapshots_with_stale_firmware_and_platform_generations() {
        let roots = BTreeSet::from([7]);
        let mut stale_version = platform_device_page(47, 3, 7, 0, 1, vec![1]);
        stale_version.firmware_version = "stale";
        let mut seen = BTreeSet::new();
        let mut paths = Vec::new();
        assert_eq!(
            append_platform_device_paths(
                &stale_version,
                &expected_platform_generation(),
                &roots,
                &mut seen,
                &mut paths,
            ),
            Err(PciDevicePathError::InvalidPlatformPath)
        );

        for stale_dimension in 0..3 {
            let mut page = platform_device_page(48, 3, 7, 0, 1, vec![1]);
            let device = &mut page.devices[0];
            match stale_dimension {
                0 => device.namespace_generation += 1,
                1 => device.platform_topology_generation += 1,
                2 => device.pci_fabric_generation += 1,
                _ => unreachable!(),
            }
            let mut seen = BTreeSet::new();
            let mut paths = Vec::new();

            assert_eq!(
                append_platform_device_paths(
                    &page,
                    &expected_platform_generation(),
                    &roots,
                    &mut seen,
                    &mut paths,
                ),
                Err(PciDevicePathError::StaleDeviceGeneration)
            );
            assert!(paths.is_empty());
        }
    }

    #[test]
    fn rejects_repeated_platform_device_ids_across_pages() {
        use std::collections::BTreeSet;

        let roots = BTreeSet::from([7]);
        let mut seen = BTreeSet::from([42]);
        let mut paths = Vec::new();
        let page = platform_device_page(42, 3, 0, 0, 0, Vec::new());
        assert_eq!(
            append_platform_device_paths(
                &page,
                &expected_platform_generation(),
                &roots,
                &mut seen,
                &mut paths
            ),
            Err(PciDevicePathError::DuplicateDevice)
        );
    }

    #[test]
    fn rejects_crossing_or_out_of_root_bridge_ranges() {
        let first = PciAddress::new(0, 0, 1, 0);
        let second = PciAddress::new(0, 0, 2, 0);
        let functions = [first, second];
        let crossing = [
            BridgeBusRange {
                address: first,
                primary: 0,
                secondary: 1,
                subordinate: 4,
            },
            BridgeBusRange {
                address: second,
                primary: 0,
                secondary: 2,
                subordinate: 5,
            },
        ];
        assert_eq!(
            assign_bridge_parents(0, 5, &functions, &crossing),
            Err(BridgeTopologyError::CrossingBusRanges)
        );

        let outside = [BridgeBusRange {
            address: first,
            primary: 0,
            secondary: 1,
            subordinate: 6,
        }];
        assert_eq!(
            assign_bridge_parents(0, 5, &functions, &outside),
            Err(BridgeTopologyError::BridgeOutsideRoot)
        );
    }

    #[test]
    fn ecam_dword_reads_are_little_endian_aligned_and_bounded() {
        #[repr(align(4))]
        struct AlignedBytes([u8; 8]);

        let bytes = AlignedBytes([0x78, 0x56, 0x34, 0x12, 0, 0, 0, 0]);
        assert_eq!(read_ecam_dword(&bytes.0, 0), 0x1234_5678);
        assert_eq!(read_ecam_dword(&bytes.0, 1), u32::MAX);
        assert_eq!(read_ecam_dword(&bytes.0, 8), u32::MAX);
    }

    #[test]
    fn stops_pci_discovery_after_deadline_discovery_failure_or_unknown_health() {
        assert!(super::health_allows_pci_discovery(0));
        assert!(super::health_allows_pci_discovery(1));
        assert!(!super::health_allows_pci_discovery(2));
        assert!(!super::health_allows_pci_discovery(3));
        assert!(!super::health_allows_pci_discovery(4)); // ACPI discovery failure
        assert!(!super::health_allows_pci_discovery(8));
    }

    #[tokio::test]
    async fn topology_close_cancels_startup_scan_and_drops_partial_tree() {
        use std::sync::{
            Arc,
            atomic::{AtomicBool, Ordering},
        };

        struct DropFlag(Arc<AtomicBool>);
        impl Drop for DropFlag {
            fn drop(&mut self) {
                self.0.store(true, Ordering::Release);
            }
        }

        let dropped = Arc::new(AtomicBool::new(false));
        let flag = DropFlag(dropped.clone());
        let work = async move {
            let _partial_tree = flag;
            core::future::pending::<usize>().await
        };
        let (close_sender, close_receiver) = tokio::sync::oneshot::channel();
        tokio::spawn(async move {
            let _ = close_sender.send(());
        });

        assert_eq!(
            super::race_topology_lifecycle(work, async move {
                let _ = close_receiver.await;
            })
            .await,
            Err(())
        );
        assert!(dropped.load(Ordering::Acquire));
    }
}
