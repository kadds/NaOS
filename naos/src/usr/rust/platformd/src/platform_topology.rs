use crate::topology_health::{HEALTH_DEGRADED, HEALTH_DISCOVERY_FAILED};
use naos_idl::platform_pci_topology as idl;
use rust_acpi_naos::{DeviceIdentifierKind, DeviceIdentifierValue, DeviceSnapshot};
use std::ops::{Deref, DerefMut};
use std::sync::Arc;

pub const MAX_DEVICES_PER_PAGE: usize = 4;
pub const MAX_IDENTIFIERS_PER_DEVICE: usize = rust_acpi_naos::MAX_DEVICE_IDENTIFIERS;
pub const MAX_RESOURCES_PER_DEVICE: usize = 256;
pub const MAX_PCI_ROUTING_PAGE: usize = 64;
pub const FILTER_PRESENT_ONLY: u32 = 1 << 0;
pub const FILTER_DEGRADED_ONLY: u32 = 1 << 1;
pub const FILTER_FUNCTIONAL_ONLY: u32 = 1 << 2;
pub const DEVICE_FILTER_MASK: u32 =
    FILTER_PRESENT_ONLY | FILTER_DEGRADED_ONLY | FILTER_FUNCTIONAL_ONLY;
pub const DEVICE_EVALUATED_IDENTIFIERS: u32 = 1 << 0;
pub const DEVICE_EVALUATED_STATUS: u32 = 1 << 1;
pub const DEVICE_EVALUATED_INITIALIZATION: u32 = 1 << 2;
pub const DEVICE_EVALUATED_RESOURCES: u32 = 1 << 3;
pub const DEVICE_EVALUATED_ROUTING: u32 = 1 << 4;
pub const IMPLEMENTED_PCI_ROOT_SUPPORT_MASK: u32 =
    idl::PciRootFeatureSupport::segment_groups as u32;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum RootFeatureEvidenceError {
    MissingScope,
    StaleLineage,
    IncompleteNegotiation,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum RootResourceEvidenceError {
    MissingScope,
    StaleLineage,
    InvalidWindowTable,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum DeviceResourceEvidenceError {
    StaleLineage,
    InvalidDevice,
    InvalidPciPath,
    ResourcesUnavailable,
    InvalidWindows,
}

/// Build the exact root `_CRS` evidence sent to the kernel-owned resource
/// attestation gate. The kernel independently validates the descriptor table
/// and binds it to this platformd-owned PCI config root scope.
pub fn build_pci_root_resource_evidence<'a>(
    root: &rust_acpi_naos::PciRootBridgeDescriptor,
    pci_config_scope_id: u64,
    generation: &'a PlatformGenerationSnapshot,
) -> Result<
    naos_idl::acpi_resource_authority::mint_pci_root_resource_attestation_request<'a>,
    RootResourceEvidenceError,
> {
    if pci_config_scope_id == 0 {
        return Err(RootResourceEvidenceError::MissingScope);
    }
    if root.root_id == 0
        || root.device_id == 0
        || generation.firmware_version.is_empty()
        || generation.aml_namespace_generation == 0
        || generation.platform_topology_generation == 0
        || generation.pci_fabric_generation == 0
        || generation.compatibility_profile_hash == 0
        || root.aml_namespace_generation != generation.aml_namespace_generation
    {
        return Err(RootResourceEvidenceError::StaleLineage);
    }
    if root.resource_windows.is_empty()
        || root.resource_windows.len() > rust_acpi_naos::MAX_PCI_ROOT_RESOURCE_WINDOWS
    {
        return Err(RootResourceEvidenceError::InvalidWindowTable);
    }

    let windows: Vec<_> = root
        .resource_windows
        .iter()
        .map(
            |window| naos_idl::acpi_resource_authority::PciRootResourceWindow {
                resource_type: u32::from(window.resource_type),
                descriptor_revision: window.descriptor_revision,
                general_flags: window.general_flags,
                type_specific_flags: window.type_specific_flags,
                granularity: window.granularity,
                minimum: window.minimum,
                maximum: window.maximum,
                translation_offset: window.translation_offset,
                length: window.length,
                type_specific_attributes: window.type_specific_attributes,
            },
        )
        .collect();
    if rust_acpi_naos::pci_root_bus_window(&root.resource_windows)
        .ok()
        .flatten()
        != Some((root.start_bus, root.end_bus))
        || !root
            .resource_windows
            .iter()
            .any(|window| window.resource_type == 0)
    {
        return Err(RootResourceEvidenceError::InvalidWindowTable);
    }

    Ok(
        naos_idl::acpi_resource_authority::mint_pci_root_resource_attestation_request {
            pci_config_scope_id,
            root_id: root.root_id,
            firmware_version: generation.firmware_version.as_str(),
            aml_namespace_generation: generation.aml_namespace_generation,
            platform_topology_generation: generation.platform_topology_generation,
            pci_fabric_generation: generation.pci_fabric_generation,
            compatibility_profile_hash: generation.compatibility_profile_hash,
            segment: u32::from(root.segment),
            start_bus: u32::from(root.start_bus),
            end_bus: u32::from(root.end_bus),
            windows,
        },
    )
}

/// Build the device-scoped `_CRS` claim carried to the kernel root
/// attestation. Only fixed consumer memory ranges with no leaf translation
/// are eligible; the kernel still checks exact root-window containment.
pub fn build_pci_device_resource_evidence(
    root: &rust_acpi_naos::PciRootBridgeDescriptor,
    device: &DeviceSnapshot,
    path: &rust_acpi_naos::PciDeviceFunctionPath,
    generation: &PlatformGenerationSnapshot,
) -> Result<
    naos_idl::pci_root_resource_attestation::mint_device_resources_request,
    DeviceResourceEvidenceError,
> {
    if root.root_id == 0 || device.device_id == 0 {
        return Err(DeviceResourceEvidenceError::InvalidDevice);
    }
    if path.device_id != device.device_id {
        return Err(DeviceResourceEvidenceError::InvalidPciPath);
    }
    if generation.firmware_version.is_empty()
        || generation.aml_namespace_generation == 0
        || generation.platform_topology_generation == 0
        || generation.pci_fabric_generation == 0
        || generation.compatibility_profile_hash == 0
        || root.aml_namespace_generation != generation.aml_namespace_generation
        || device.namespace_generation != generation.aml_namespace_generation
    {
        return Err(DeviceResourceEvidenceError::StaleLineage);
    }
    if path.root_id != root.root_id
        || path.device_function_path.is_empty()
        || path.device_function_path.len() > rust_acpi_naos::MAX_PCI_DEVICE_FUNCTION_PATH
    {
        return Err(DeviceResourceEvidenceError::InvalidPciPath);
    }
    if !device.resources_evaluated || device.degraded {
        return Err(DeviceResourceEvidenceError::ResourcesUnavailable);
    }
    let normalized = rust_acpi_naos::pci_device_memory_windows(&device.address_spaces)
        .map_err(|_| DeviceResourceEvidenceError::InvalidWindows)?;

    let mut device_function_path = [0_u8; rust_acpi_naos::MAX_PCI_DEVICE_FUNCTION_PATH];
    device_function_path[..path.device_function_path.len()]
        .copy_from_slice(&path.device_function_path);
    let memory_windows = normalized
        .into_iter()
        .map(
            |window| naos_idl::pci_root_resource_attestation::PciDeviceResourceRequestWindow {
                base: window.base,
                length: window.length,
                flags: u32::from(window.writable),
            },
        )
        .collect();
    Ok(
        naos_idl::pci_root_resource_attestation::mint_device_resources_request {
            device_id: device.device_id,
            path_length: path.device_function_path.len() as u32,
            device_function_path,
            memory_windows,
        },
    )
}

/// Construct the exact evidence sent to the kernel's root-feature authority
/// gate. This only accepts the boot policy's complete, zero-control
/// negotiation; it cannot turn caller-selected root metadata into authority.
pub fn build_pci_root_feature_evidence(
    root: &rust_acpi_naos::PciRootBridgeDescriptor,
    pci_config_scope_id: u64,
    generation: &PlatformGenerationSnapshot,
    negotiation: &rust_acpi_naos::PciOscNegotiation,
) -> Result<naos_idl::acpi_resource_authority::PciRootFeatureEvidence, RootFeatureEvidenceError> {
    if pci_config_scope_id == 0 {
        return Err(RootFeatureEvidenceError::MissingScope);
    }
    if root.root_id == 0
        || generation.firmware_version.is_empty()
        || generation.aml_namespace_generation == 0
        || generation.platform_topology_generation == 0
        || generation.pci_fabric_generation == 0
        || generation.compatibility_profile_hash == 0
        || root.aml_namespace_generation != generation.aml_namespace_generation
        || negotiation.firmware_version != generation.firmware_version
        || negotiation.aml_namespace_generation != generation.aml_namespace_generation
        || negotiation.compatibility_profile_hash != generation.compatibility_profile_hash
    {
        return Err(RootFeatureEvidenceError::StaleLineage);
    }
    if !pci_osc_negotiation_is_complete(negotiation) {
        return Err(RootFeatureEvidenceError::IncompleteNegotiation);
    }

    Ok(naos_idl::acpi_resource_authority::PciRootFeatureEvidence {
        pci_config_scope_id,
        root_id: root.root_id,
        aml_namespace_generation: generation.aml_namespace_generation,
        platform_topology_generation: generation.platform_topology_generation,
        pci_fabric_generation: generation.pci_fabric_generation,
        compatibility_profile_hash: generation.compatibility_profile_hash,
        segment: u32::from(root.segment),
        start_bus: u32::from(root.start_bus),
        end_bus: u32::from(root.end_bus),
        requested_support: negotiation.requested_support,
        requested_control: negotiation.requested_control,
        query_support: negotiation.query_support,
        query_granted_control: negotiation.query_granted_control,
        commit_support: negotiation.commit_support,
        granted_control: negotiation.granted_control,
        denied_control: negotiation.denied_control,
        masked_control: negotiation.masked_control,
        query_errors: negotiation.query_errors,
        commit_errors: negotiation.commit_errors,
    })
}

pub fn pci_osc_negotiation_is_complete(negotiation: &rust_acpi_naos::PciOscNegotiation) -> bool {
    negotiation.requested_support == IMPLEMENTED_PCI_ROOT_SUPPORT_MASK
        && negotiation.requested_control == 0
        && negotiation.query_errors == 0
        && negotiation.query_support & !negotiation.requested_support == 0
        && negotiation.query_granted_control == 0
        && negotiation.masked_control == 0
        && negotiation.commit_errors == 0
        && negotiation.commit_support == negotiation.query_support
        && negotiation.granted_control == 0
        && negotiation.denied_control == 0
}

/// Remove PCI roots whose device snapshot does not confirm that the bridge is
/// present and functioning. The snapshot is the single `_STA` evaluation used
/// for topology publication; evaluating `_STA` again during root discovery
/// could produce inconsistent results for stateful firmware methods.
pub fn retain_functional_pci_roots(
    roots: &mut Vec<rust_acpi_naos::PciRootBridgeDescriptor>,
    devices: &[DeviceSnapshot],
) -> usize {
    const STATUS_PRESENT: u64 = 1 << 0;
    const STATUS_FUNCTIONING: u64 = 1 << 3;
    const REQUIRED_STATUS: u64 = STATUS_PRESENT | STATUS_FUNCTIONING;

    let before = roots.len();
    roots.retain(|root| {
        devices.iter().any(|device| {
            device.device_id == root.device_id
                && device.namespace_generation == root.aml_namespace_generation
                && device.path == root.namespace_path
                && device.status_evaluated
                && device.status & REQUIRED_STATUS == REQUIRED_STATUS
        })
    });
    before - roots.len()
}

#[derive(Clone, Debug)]
pub struct PlatformGenerationSnapshot {
    pub firmware_version: String,
    pub value: idl::PlatformGeneration,
}

impl Deref for PlatformGenerationSnapshot {
    type Target = idl::PlatformGeneration;

    fn deref(&self) -> &Self::Target {
        &self.value
    }
}

impl DerefMut for PlatformGenerationSnapshot {
    fn deref_mut(&mut self) -> &mut Self::Target {
        &mut self.value
    }
}

pub struct TopologyState {
    pub generation: PlatformGenerationSnapshot,
    pub roots: Vec<idl::PlatformPciRoot>,
    pub resource_windows: Vec<idl::PlatformPciResourceWindow>,
    pub features: Vec<idl::PciRootFeatureSet>,
    pub devices: Arc<Vec<DeviceSnapshot>>,
    pub pci_device_paths: Vec<rust_acpi_naos::PciDeviceFunctionPath>,
}

/// Signals clients to drop generation-bound capabilities when the published
/// topology changes. The current protocol has no in-band change subscription,
/// so invalidation closes the serving connection instead of leaving clients
/// silently bound to stale roots and function objects.
pub struct TopologyInvalidation {
    sender: tokio::sync::watch::Sender<Option<u64>>,
}

impl TopologyInvalidation {
    pub fn channel() -> (Self, tokio::sync::watch::Receiver<Option<u64>>) {
        let (sender, receiver) = tokio::sync::watch::channel(None);
        (Self { sender }, receiver)
    }

    fn invalidate(&self, generation: u64) {
        self.sender.send_replace(Some(generation));
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum TopologyServeOutcome {
    ServerStopped(i32),
    Invalidated(u64),
    InvalidationChannelClosed,
}

/// Keep serving the current topology incarnation until either the server
/// returns or a generation update requires all connected consumers to drop
/// their generation-bound capabilities.
pub async fn serve_until_invalidation<F>(
    server: F,
    invalidated: &mut tokio::sync::watch::Receiver<Option<u64>>,
) -> TopologyServeOutcome
where
    F: core::future::Future<Output = i32>,
{
    tokio::select! {
        status = server => TopologyServeOutcome::ServerStopped(status),
        changed = invalidated.changed() => match changed {
            Ok(()) => match *invalidated.borrow_and_update() {
                Some(generation) => TopologyServeOutcome::Invalidated(generation),
                None => TopologyServeOutcome::InvalidationChannelClosed,
            },
            Err(_) => TopologyServeOutcome::InvalidationChannelClosed,
        }
    }
}

/// Publish a new topology incarnation when the SCI event consumer can no
/// longer wait safely. Read-only PCI discovery remains permitted, but every
/// previously issued topology generation becomes stale.
pub fn mark_sci_consumer_failed(
    state: &mut TopologyState,
    generation: u64,
    invalidation: &TopologyInvalidation,
) -> bool {
    if generation == 0 || generation <= state.generation.platform_topology_generation {
        return false;
    }

    state.generation.platform_topology_generation = generation;
    state.generation.health |= HEALTH_DEGRADED;
    for root in &mut state.roots {
        root.platform_topology_generation = generation;
    }
    for feature in &mut state.features {
        feature.platform_topology_generation = generation;
    }
    invalidation.invalidate(generation);
    true
}

pub fn unavailable_generation(
    firmware_version: String,
    platform_topology_generation: u64,
    pci_fabric_generation: u64,
) -> PlatformGenerationSnapshot {
    PlatformGenerationSnapshot {
        firmware_version,
        value: idl::PlatformGeneration {
            aml_namespace_generation: 0,
            platform_topology_generation,
            pci_fabric_generation,
            health: HEALTH_DEGRADED | HEALTH_DISCOVERY_FAILED,
            compatibility_profile_hash: 0,
        },
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum PageError {
    InvalidLimit,
    InvalidFilter,
    CursorOutOfRange,
}

/// Select a bounded page while keeping cursors tied to the immutable source
/// array, rather than to the filtered view.
pub fn select_device_page(
    devices: &[DeviceSnapshot],
    cursor: u64,
    limit: usize,
    filter_flags: u32,
) -> Result<(Vec<usize>, u64), PageError> {
    if !(1..=MAX_DEVICES_PER_PAGE).contains(&limit) {
        return Err(PageError::InvalidLimit);
    }
    if filter_flags & !DEVICE_FILTER_MASK != 0 {
        return Err(PageError::InvalidFilter);
    }
    let cursor = usize::try_from(cursor).map_err(|_| PageError::CursorOutOfRange)?;
    if cursor > devices.len() {
        return Err(PageError::CursorOutOfRange);
    }

    let mut indices = Vec::with_capacity(limit);
    let mut next = cursor;
    while next < devices.len() && indices.len() < limit {
        let index = next;
        next += 1;
        let device = &devices[index];
        let present = device.status_evaluated && device.status & 1 != 0;
        let functional = present && device.status & (1 << 3) != 0;
        let include = (filter_flags & FILTER_PRESENT_ONLY == 0 || present)
            && (filter_flags & FILTER_DEGRADED_ONLY == 0 || device.degraded)
            && (filter_flags & FILTER_FUNCTIONAL_ONLY == 0 || functional);
        if include {
            indices.push(index);
        }
    }
    let next_cursor = if next == devices.len() {
        0
    } else {
        u64::try_from(next).map_err(|_| PageError::CursorOutOfRange)?
    };
    Ok((indices, next_cursor))
}

pub fn normalized_identifiers(
    device: &DeviceSnapshot,
) -> Vec<(idl::PlatformDeviceIdentifier, Vec<u8>)> {
    device
        .identifiers
        .iter()
        .filter_map(|identifier| {
            let kind = match identifier.kind {
                DeviceIdentifierKind::Hid => 1,
                DeviceIdentifierKind::Cid => 2,
                DeviceIdentifierKind::Uid => 3,
            };
            let (value_kind, text, integer) = match &identifier.value {
                DeviceIdentifierValue::String(value) => (1, value.as_bytes().to_vec(), 0),
                DeviceIdentifierValue::Integer(value) => (2, Vec::new(), *value),
            };
            Some((
                idl::PlatformDeviceIdentifier {
                    kind,
                    value_kind,
                    text_start: 0,
                    text_length: u32::try_from(text.len()).ok()?,
                    integer,
                },
                text,
            ))
        })
        .collect()
}

pub fn generation_response<'a>(
    state: &TopologyState,
    firmware_version: &'a str,
) -> idl::generation_response<'a> {
    idl::generation_response {
        value: state.generation.value,
        firmware_version,
    }
}

pub fn get_pci_roots_response<'a>(
    request: &idl::get_pci_roots_request<'_>,
    state: &TopologyState,
    firmware_version: &'a str,
) -> Result<idl::get_pci_roots_response<'a>, i64> {
    ensure_current_generation(
        request.firmware_version,
        request.platform_topology_generation,
        state,
    )?;
    Ok(idl::get_pci_roots_response {
        roots: state.roots.clone(),
        resource_windows: state.resource_windows.clone(),
        firmware_version,
    })
}

pub fn negotiate_pci_features_response<'a>(
    request: &idl::negotiate_pci_features_request<'_>,
    state: &TopologyState,
    firmware_version: &'a str,
) -> Result<idl::negotiate_pci_features_response<'a>, i64> {
    ensure_current_generation(
        request.firmware_version,
        request.platform_topology_generation,
        state,
    )?;
    if request.requested_support != IMPLEMENTED_PCI_ROOT_SUPPORT_MASK
        || request.requested_control != 0
    {
        log::warn!(
            "rejecting PCI feature request root_id={:016x} requested_support={:08x} policy_support={:08x} requested_control={:08x}",
            request.root_id,
            request.requested_support,
            IMPLEMENTED_PCI_ROOT_SUPPORT_MASK,
            request.requested_control
        );
        return Err(-95);
    }
    let feature = state
        .features
        .iter()
        .find(|feature| feature.root_id == request.root_id)
        .ok_or(-2_i64)?;
    Ok(idl::negotiate_pci_features_response {
        value: *feature,
        firmware_version,
    })
}

pub fn list_devices_response<'a>(
    request: &idl::list_devices_request<'_>,
    state: &TopologyState,
    firmware_version: &'a str,
) -> Result<idl::list_devices_response<'a>, i64> {
    ensure_current_generation(
        request.firmware_version,
        request.platform_topology_generation,
        state,
    )?;
    let limit = usize::try_from(request.limit).map_err(|_| -22_i64)?;
    let (indices, next_cursor) =
        select_device_page(&state.devices, request.cursor, limit, request.filter_flags)
            .map_err(|_| -22_i64)?;
    let mut devices = Vec::with_capacity(indices.len());
    let mut identifiers = Vec::new();
    let mut resources = Vec::new();
    let mut identifier_text = Vec::new();
    let mut pci_device_function_path = Vec::new();
    for index in indices {
        let device = &state.devices[index];
        let identifier_start = u32::try_from(identifiers.len()).map_err(|_| -75_i64)?;
        let normalized = normalized_identifiers(device);
        if normalized.len() > MAX_IDENTIFIERS_PER_DEVICE {
            return Err(-75);
        }
        let identifier_count = u32::try_from(normalized.len()).map_err(|_| -75_i64)?;
        for (mut identifier, text) in normalized {
            identifier.text_start = u32::try_from(identifier_text.len()).map_err(|_| -75_i64)?;
            identifier_text.extend_from_slice(&text);
            identifiers.push(identifier);
        }

        let resource_start = u32::try_from(resources.len()).map_err(|_| -75_i64)?;
        if device.resources.len() > MAX_RESOURCES_PER_DEVICE {
            return Err(-75);
        }
        let resource_count = u32::try_from(device.resources.len()).map_err(|_| -75_i64)?;
        resources.extend(
            device
                .resources
                .iter()
                .map(|resource| idl::PlatformDeviceResource {
                    kind: u32::from(resource.kind),
                    base: resource.base,
                    length: resource.length,
                    flags: resource.flags,
                }),
        );

        let mut evaluation_flags = 0;
        if device.identifiers_evaluated {
            evaluation_flags |= DEVICE_EVALUATED_IDENTIFIERS;
        }
        if device.status_evaluated {
            evaluation_flags |= DEVICE_EVALUATED_STATUS;
        }
        if device.initialization_evaluated {
            evaluation_flags |= DEVICE_EVALUATED_INITIALIZATION;
        }
        if device.resources_evaluated {
            evaluation_flags |= DEVICE_EVALUATED_RESOURCES;
        }
        if device.routing_evaluated {
            evaluation_flags |= DEVICE_EVALUATED_ROUTING;
        }
        let pci_path = state
            .pci_device_paths
            .iter()
            .find(|path| path.device_id == device.device_id);
        let (pci_root_id, pci_path_start, pci_path_count) = match pci_path {
            Some(path)
                if path.root_id != 0
                    && !path.device_function_path.is_empty()
                    && path.device_function_path.len()
                        <= rust_acpi_naos::MAX_PCI_DEVICE_FUNCTION_PATH
                    && state.roots.iter().any(|root| root.root_id == path.root_id) =>
            {
                let start = u32::try_from(pci_device_function_path.len()).map_err(|_| -75_i64)?;
                let count = u32::try_from(path.device_function_path.len()).map_err(|_| -75_i64)?;
                pci_device_function_path.extend_from_slice(&path.device_function_path);
                (path.root_id, start, count)
            }
            Some(_) => return Err(-22),
            None => (
                0,
                u32::try_from(pci_device_function_path.len()).map_err(|_| -75_i64)?,
                0,
            ),
        };
        if pci_device_function_path.len() > 64 {
            return Err(-75);
        }
        devices.push(idl::PlatformDeviceSnapshot {
            device_id: device.device_id,
            namespace_generation: device.namespace_generation,
            status: device.status,
            evaluation_flags,
            initialized: u8::from(device.initialized),
            degraded: u8::from(device.degraded),
            identifier_start,
            identifier_count,
            resource_start,
            resource_count,
            routing_entries: device.routing_entries,
            pci_root_id,
            pci_path_start,
            pci_path_count,
            platform_topology_generation: state.generation.platform_topology_generation,
            pci_fabric_generation: state.generation.pci_fabric_generation,
        });
    }
    Ok(idl::list_devices_response {
        devices,
        identifiers,
        resources,
        identifier_text,
        next_cursor,
        pci_device_function_path,
        firmware_version,
    })
}

pub fn get_resources_response(
    request: &idl::get_resources_request,
    state: &TopologyState,
) -> Result<idl::get_resources_response, i64> {
    ensure_current_generation(
        request.firmware_version,
        request.platform_topology_generation,
        state,
    )?;
    let device = state
        .devices
        .iter()
        .find(|device| device.device_id == request.device_id)
        .ok_or(-2_i64)?;
    if device.resources.len() > MAX_RESOURCES_PER_DEVICE {
        return Err(-75);
    }
    let resources = device
        .resources
        .iter()
        .map(|resource| idl::PlatformDeviceResource {
            kind: u32::from(resource.kind),
            base: resource.base,
            length: resource.length,
            flags: resource.flags,
        })
        .collect();
    Ok(idl::get_resources_response {
        resources,
        evaluated: u8::from(device.resources_evaluated),
        degraded: u8::from(device.degraded),
    })
}

pub fn get_interrupt_routing_response(
    request: &idl::get_interrupt_routing_request,
    state: &TopologyState,
) -> Result<idl::get_interrupt_routing_response, i64> {
    ensure_current_generation(
        request.firmware_version,
        request.platform_topology_generation,
        state,
    )?;
    let limit = usize::try_from(request.limit).map_err(|_| -22_i64)?;
    let cursor = usize::try_from(request.cursor).map_err(|_| -22_i64)?;
    if limit == 0 || limit > MAX_PCI_ROUTING_PAGE {
        return Err(-22);
    }
    let device = state
        .devices
        .iter()
        .find(|device| device.device_id == request.device_id)
        .ok_or(-2_i64)?;
    if device.routing.len() > rust_acpi_naos::MAX_PCI_ROUTING_ENTRIES
        || device.routing_entries as usize != device.routing.len()
        || cursor > device.routing.len()
    {
        return Err(-22);
    }
    let end = cursor.saturating_add(limit).min(device.routing.len());
    let routes = device.routing[cursor..end]
        .iter()
        .map(|route| {
            let (source_kind, source_device_id, source_index) = match route.source {
                rust_acpi_naos::PciRoutingSource::DirectGsi(gsi) => {
                    (idl::PciRoutingSourceKind::direct_gsi, 0, gsi)
                }
                rust_acpi_naos::PciRoutingSource::LinkDevice {
                    device_id,
                    source_index,
                } => (
                    idl::PciRoutingSourceKind::link_device,
                    device_id,
                    source_index,
                ),
            };
            idl::PlatformPciRoutingEntry {
                address: route.address,
                pin: route.pin,
                source_kind,
                source_device_id,
                source_index,
            }
        })
        .collect();
    let next_cursor = if end < device.routing.len() {
        u64::try_from(end).map_err(|_| -75_i64)?
    } else {
        0
    };
    Ok(idl::get_interrupt_routing_response {
        routes,
        next_cursor,
        evaluated: u8::from(device.routing_evaluated),
        complete: u8::from(device.routing_complete),
    })
}

fn ensure_current_generation(
    firmware_version: &str,
    topology_generation: u64,
    state: &TopologyState,
) -> Result<(), i64> {
    if firmware_version != state.generation.firmware_version
        || topology_generation != state.generation.platform_topology_generation
    {
        Err(-116)
    } else {
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use rust_acpi_naos::{DeviceIdentifier, PciRoutingEntry, PciRoutingSource, ResourceSnapshot};

    fn dispatch(method_id: u64, payload: &[u8], state: &TopologyState) -> Result<Vec<u8>, i64> {
        let mut wire = vec![0; 65_536];
        let written = match method_id {
            idl::METHOD_GENERATION => {
                idl::decode_generation_request(payload).map_err(|_| -22_i64)?;
                idl::encode_generation_response(
                    &generation_response(state, state.generation.firmware_version.as_str()),
                    &mut wire,
                )
                .map_err(|_| -75_i64)?
            }
            idl::METHOD_GET_PCI_ROOTS => {
                let request = idl::decode_get_pci_roots_request(payload).map_err(|_| -22_i64)?;
                let response = get_pci_roots_response(
                    &request,
                    state,
                    state.generation.firmware_version.as_str(),
                )?;
                idl::encode_get_pci_roots_response(&response, &mut wire).map_err(|_| -75_i64)?
            }
            idl::METHOD_NEGOTIATE_PCI_FEATURES => {
                let request =
                    idl::decode_negotiate_pci_features_request(payload).map_err(|_| -22_i64)?;
                let response = negotiate_pci_features_response(
                    &request,
                    state,
                    state.generation.firmware_version.as_str(),
                )?;
                idl::encode_negotiate_pci_features_response(&response, &mut wire)
                    .map_err(|_| -75_i64)?
            }
            idl::METHOD_LIST_DEVICES => {
                let request = idl::decode_list_devices_request(payload).map_err(|_| -22_i64)?;
                let response = list_devices_response(
                    &request,
                    state,
                    state.generation.firmware_version.as_str(),
                )?;
                idl::encode_list_devices_response(&response, &mut wire).map_err(|_| -75_i64)?
            }
            idl::METHOD_GET_RESOURCES => {
                let request = idl::decode_get_resources_request(payload).map_err(|_| -22_i64)?;
                let response = get_resources_response(&request, state)?;
                idl::encode_get_resources_response(&response, &mut wire).map_err(|_| -75_i64)?
            }
            idl::METHOD_GET_INTERRUPT_ROUTING => {
                let request =
                    idl::decode_get_interrupt_routing_request(payload).map_err(|_| -22_i64)?;
                let response = get_interrupt_routing_response(&request, state)?;
                idl::encode_get_interrupt_routing_response(&response, &mut wire)
                    .map_err(|_| -75_i64)?
            }
            _ => return Err(-38),
        };
        wire.truncate(written);
        Ok(wire)
    }

    fn osc_root() -> rust_acpi_naos::PciRootBridgeDescriptor {
        rust_acpi_naos::PciRootBridgeDescriptor {
            root_id: 6,
            device_id: 7,
            namespace_path: "\\_SB_.PCI0".to_string(),
            segment: 0,
            start_bus: 0,
            end_bus: 31,
            aml_namespace_generation: 9,
            resource_windows: Vec::new(),
        }
    }

    #[test]
    fn pci_root_publication_requires_matching_present_functional_sta_snapshot() {
        let active_root = osc_root();
        let mut absent_root = active_root.clone();
        absent_root.root_id = 8;
        absent_root.device_id = 9;
        absent_root.namespace_path = "\\_SB_.PCI1".to_string();
        let mut nonfunctional_root = active_root.clone();
        nonfunctional_root.root_id = 10;
        nonfunctional_root.device_id = 11;
        nonfunctional_root.namespace_path = "\\_SB_.PCI2".to_string();
        let mut unevaluated_root = active_root.clone();
        unevaluated_root.root_id = 12;
        unevaluated_root.device_id = 13;
        unevaluated_root.namespace_path = "\\_SB_.PCI3".to_string();
        let mut stale_root = active_root.clone();
        stale_root.root_id = 14;
        stale_root.device_id = 15;
        stale_root.namespace_path = "\\_SB_.PCI4".to_string();

        let mut active_device = device(7, 0x0f, false);
        active_device.path = active_root.namespace_path.clone();
        let mut absent_device = device(9, 0x0e, false);
        absent_device.path = absent_root.namespace_path.clone();
        let mut nonfunctional_device = device(11, 0x07, false);
        nonfunctional_device.path = nonfunctional_root.namespace_path.clone();
        let mut unevaluated_device = device(13, 0x0f, false);
        unevaluated_device.path = unevaluated_root.namespace_path.clone();
        unevaluated_device.status_evaluated = false;
        let mut stale_device = device(15, 0x0f, false);
        stale_device.path = stale_root.namespace_path.clone();
        stale_device.namespace_generation += 1;

        let mut roots = vec![
            active_root.clone(),
            absent_root,
            nonfunctional_root,
            unevaluated_root,
            stale_root,
        ];
        let devices = vec![
            active_device,
            absent_device,
            nonfunctional_device,
            unevaluated_device,
            stale_device,
        ];

        assert_eq!(retain_functional_pci_roots(&mut roots, &devices), 4);
        assert_eq!(roots, vec![active_root]);
    }

    fn osc_generation() -> PlatformGenerationSnapshot {
        PlatformGenerationSnapshot {
            firmware_version: "test-firmware-v1".to_owned(),
            value: idl::PlatformGeneration {
                aml_namespace_generation: 9,
                platform_topology_generation: 3,
                pci_fabric_generation: 4,
                health: 0,
                compatibility_profile_hash: 5,
            },
        }
    }

    fn root_with_resources() -> rust_acpi_naos::PciRootBridgeDescriptor {
        rust_acpi_naos::PciRootBridgeDescriptor {
            root_id: 6,
            device_id: 7,
            namespace_path: "\\_SB_.PCI0".to_string(),
            segment: 0,
            start_bus: 0,
            end_bus: 255,
            aml_namespace_generation: 9,
            resource_windows: vec![
                rust_acpi_naos::PciRootResourceWindow {
                    resource_type: 2,
                    descriptor_revision: 1,
                    general_flags: 0,
                    type_specific_flags: 0,
                    granularity: 0,
                    minimum: 0,
                    maximum: 255,
                    translation_offset: 0,
                    length: 256,
                    type_specific_attributes: 0,
                },
                rust_acpi_naos::PciRootResourceWindow {
                    resource_type: 0,
                    descriptor_revision: 1,
                    general_flags: 0,
                    type_specific_flags: 0,
                    granularity: 0xffff,
                    minimum: 0x8000_0000,
                    maximum: 0x80ff_ffff,
                    translation_offset: 0,
                    length: 0x0100_0000,
                    type_specific_attributes: 0,
                },
            ],
        }
    }

    fn osc_negotiation() -> rust_acpi_naos::PciOscNegotiation {
        rust_acpi_naos::PciOscNegotiation {
            firmware_version: "test-firmware-v1".to_owned(),
            aml_namespace_generation: 9,
            compatibility_profile_hash: 5,
            requested_support: IMPLEMENTED_PCI_ROOT_SUPPORT_MASK,
            requested_control: 0,
            query_errors: 0,
            query_support: IMPLEMENTED_PCI_ROOT_SUPPORT_MASK,
            query_granted_control: 0,
            masked_control: 0,
            commit_errors: 0,
            commit_support: IMPLEMENTED_PCI_ROOT_SUPPORT_MASK,
            granted_control: 0,
            denied_control: 0,
        }
    }

    #[test]
    fn kernel_mint_evidence_binds_complete_osc_result_to_root_scope_and_generations() {
        let root = osc_root();
        let generation = osc_generation();
        let negotiation = osc_negotiation();

        let evidence = build_pci_root_feature_evidence(&root, 17, &generation, &negotiation)
            .expect("complete root-scoped evidence");

        assert_eq!(evidence.pci_config_scope_id, 17);
        assert_eq!(evidence.root_id, root.root_id);
        assert_eq!(evidence.segment, u32::from(root.segment));
        assert_eq!(evidence.start_bus, u32::from(root.start_bus));
        assert_eq!(evidence.end_bus, u32::from(root.end_bus));
        assert_eq!(negotiation.firmware_version, generation.firmware_version);
        assert_eq!(
            evidence.aml_namespace_generation,
            generation.aml_namespace_generation
        );
        assert_eq!(
            evidence.platform_topology_generation,
            generation.platform_topology_generation
        );
        assert_eq!(
            evidence.pci_fabric_generation,
            generation.pci_fabric_generation
        );
        assert_eq!(
            evidence.compatibility_profile_hash,
            generation.compatibility_profile_hash
        );
        assert_eq!(evidence.requested_support, negotiation.requested_support);
        assert_eq!(evidence.query_support, negotiation.query_support);
        assert_eq!(evidence.commit_support, negotiation.commit_support);
        assert_eq!(evidence.granted_control, 0);
    }

    #[test]
    fn kernel_mint_evidence_rejects_missing_scope_stale_lineage_and_incomplete_osc() {
        let root = osc_root();
        let generation = osc_generation();
        let negotiation = osc_negotiation();

        assert!(matches!(
            build_pci_root_feature_evidence(&root, 0, &generation, &negotiation),
            Err(RootFeatureEvidenceError::MissingScope)
        ));

        let mut stale_generation = generation.clone();
        stale_generation.aml_namespace_generation += 1;
        assert!(matches!(
            build_pci_root_feature_evidence(&root, 17, &stale_generation, &negotiation),
            Err(RootFeatureEvidenceError::StaleLineage)
        ));

        let mut incomplete = negotiation;
        incomplete.commit_errors = 1;
        assert!(matches!(
            build_pci_root_feature_evidence(&root, 17, &generation, &incomplete),
            Err(RootFeatureEvidenceError::IncompleteNegotiation)
        ));
    }

    #[test]
    fn root_resource_evidence_binds_complete_crs_windows_to_root_lineage() {
        let root = root_with_resources();
        let generation = osc_generation();
        let evidence = build_pci_root_resource_evidence(&root, 17, &generation)
            .expect("validated root resource evidence");

        assert_eq!(evidence.pci_config_scope_id, 17);
        assert_eq!(evidence.root_id, root.root_id);
        assert_eq!(evidence.firmware_version, generation.firmware_version);
        assert_eq!(
            evidence.platform_topology_generation,
            generation.platform_topology_generation
        );
        assert_eq!(evidence.windows.len(), 2);
        assert_eq!(evidence.windows[1].minimum, 0x8000_0000);
        assert_eq!(evidence.windows[1].length, 0x0100_0000);
    }

    #[test]
    fn root_resource_evidence_rejects_missing_scope_stale_lineage_and_bad_crs() {
        let root = root_with_resources();
        let generation = osc_generation();
        assert!(matches!(
            build_pci_root_resource_evidence(&root, 0, &generation),
            Err(RootResourceEvidenceError::MissingScope)
        ));

        let mut stale_generation = generation.clone();
        stale_generation.aml_namespace_generation += 1;
        assert!(matches!(
            build_pci_root_resource_evidence(&root, 17, &stale_generation),
            Err(RootResourceEvidenceError::StaleLineage)
        ));

        let mut malformed_root = root;
        malformed_root.resource_windows[0].length = 255;
        assert!(matches!(
            build_pci_root_resource_evidence(&malformed_root, 17, &generation),
            Err(RootResourceEvidenceError::InvalidWindowTable)
        ));
    }

    #[test]
    fn device_resource_evidence_binds_snapshot_ranges_to_root_and_adr_path() {
        let root = root_with_resources();
        let generation = osc_generation();
        let mut device = device(55, 0x0f, false);
        device.resources_evaluated = true;
        device.address_spaces = vec![rust_acpi_naos::PciRootResourceWindow {
            resource_type: 0,
            descriptor_revision: 1,
            general_flags: 0x0d,
            type_specific_flags: 0x06,
            granularity: 0x0fff,
            minimum: 0x8000_1000,
            maximum: 0x8000_1fff,
            translation_offset: 0,
            length: 0x1000,
            type_specific_attributes: 0,
        }];
        let path = rust_acpi_naos::PciDeviceFunctionPath {
            device_id: device.device_id,
            root_id: root.root_id,
            device_function_path: vec![0x08, 0x13],
        };

        let evidence = build_pci_device_resource_evidence(&root, &device, &path, &generation)
            .expect("fixed device memory resource evidence");
        assert_eq!(evidence.device_id, device.device_id);
        assert_eq!(evidence.path_length, 2);
        assert_eq!(evidence.device_function_path[..2], [0x08, 0x13]);
        assert_eq!(evidence.memory_windows.len(), 1);
        assert_eq!(evidence.memory_windows[0].base, 0x8000_1000);
        assert_eq!(evidence.memory_windows[0].length, 0x1000);
    }

    #[test]
    fn device_resource_evidence_rejects_stale_paths_degraded_and_unfixed_crs() {
        let root = root_with_resources();
        let generation = osc_generation();
        let mut device = device(55, 0x0f, false);
        device.resources_evaluated = true;
        device.address_spaces = vec![rust_acpi_naos::PciRootResourceWindow {
            resource_type: 0,
            descriptor_revision: 1,
            general_flags: 0x0d,
            type_specific_flags: 0,
            granularity: 0x0fff,
            minimum: 0x8000_1000,
            maximum: 0x8000_1fff,
            translation_offset: 0,
            length: 0x1000,
            type_specific_attributes: 0,
        }];
        let path = rust_acpi_naos::PciDeviceFunctionPath {
            device_id: device.device_id,
            root_id: root.root_id,
            device_function_path: vec![0x08],
        };

        let mut stale_path = path.clone();
        stale_path.root_id += 1;
        assert!(matches!(
            build_pci_device_resource_evidence(&root, &device, &stale_path, &generation),
            Err(DeviceResourceEvidenceError::InvalidPciPath)
        ));

        device.degraded = true;
        assert!(matches!(
            build_pci_device_resource_evidence(&root, &device, &path, &generation),
            Err(DeviceResourceEvidenceError::ResourcesUnavailable)
        ));
        device.degraded = false;
        device.address_spaces[0].general_flags = 0x09;
        assert!(matches!(
            build_pci_device_resource_evidence(&root, &device, &path, &generation),
            Err(DeviceResourceEvidenceError::InvalidWindows)
        ));
    }

    #[test]
    fn pci_osc_completion_requires_matching_query_and_commit_support_without_errors() {
        let negotiation = rust_acpi_naos::PciOscNegotiation {
            firmware_version: "test-firmware-v1".to_owned(),
            aml_namespace_generation: 3,
            compatibility_profile_hash: 4,
            requested_support: IMPLEMENTED_PCI_ROOT_SUPPORT_MASK,
            requested_control: 0,
            query_errors: 0,
            query_support: IMPLEMENTED_PCI_ROOT_SUPPORT_MASK,
            query_granted_control: 0,
            masked_control: 0,
            commit_errors: 0,
            commit_support: IMPLEMENTED_PCI_ROOT_SUPPORT_MASK,
            granted_control: 0,
            denied_control: 0,
        };

        assert!(pci_osc_negotiation_is_complete(&negotiation));

        let mut narrowed_support = negotiation.clone();
        narrowed_support.query_support = 0;
        narrowed_support.commit_support = 0;
        assert!(pci_osc_negotiation_is_complete(&narrowed_support));

        let mut unrequested_support = negotiation.clone();
        unrequested_support.query_support |= 1 << 31;
        assert!(!pci_osc_negotiation_is_complete(&unrequested_support));

        let mut commit_support_mismatch = negotiation.clone();
        commit_support_mismatch.commit_support = 0;
        assert!(!pci_osc_negotiation_is_complete(&commit_support_mismatch));

        let mut query_error = negotiation.clone();
        query_error.query_errors = 1;
        assert!(!pci_osc_negotiation_is_complete(&query_error));

        let mut commit_error = negotiation.clone();
        commit_error.commit_errors = 1;
        assert!(!pci_osc_negotiation_is_complete(&commit_error));

        let mut unexpected_grant = negotiation.clone();
        unexpected_grant.query_granted_control = 1;
        assert!(!pci_osc_negotiation_is_complete(&unexpected_grant));
    }

    fn device(id: u64, status: u64, degraded: bool) -> DeviceSnapshot {
        DeviceSnapshot {
            device_id: id,
            namespace_generation: 9,
            path: format!("\\_SB_.D{id:03}"),
            identifiers: Vec::new(),
            identifiers_evaluated: true,
            status,
            status_evaluated: true,
            initialized: false,
            initialization_evaluated: false,
            degraded,
            resources: Vec::new(),
            address_spaces: Vec::new(),
            resources_evaluated: false,
            routing_entries: 0,
            routing: Vec::new(),
            routing_evaluated: false,
            routing_complete: false,
        }
    }

    fn state(devices: Vec<DeviceSnapshot>) -> TopologyState {
        TopologyState {
            generation: PlatformGenerationSnapshot {
                firmware_version: "test-firmware-v1".to_owned(),
                value: idl::PlatformGeneration {
                    aml_namespace_generation: 9,
                    platform_topology_generation: 3,
                    pci_fabric_generation: 4,
                    health: 0,
                    compatibility_profile_hash: 5,
                },
            },
            roots: Vec::new(),
            resource_windows: Vec::new(),
            features: Vec::new(),
            devices: Arc::new(devices),
            pci_device_paths: Vec::new(),
        }
    }

    #[test]
    fn list_devices_carries_root_relative_pci_address_paths_without_namespace_paths() {
        let mut state = state(vec![device(10, 0xf, false), device(20, 0xf, false)]);
        state.roots.push(idl::PlatformPciRoot {
            root_id: 7,
            segment: 0,
            start_bus: 0,
            end_bus: 31,
            aml_namespace_generation: 9,
            platform_topology_generation: 3,
            pci_fabric_generation: 4,
            device_id: 30,
            resource_window_start: 0,
            resource_window_count: 0,
            compatibility_profile_hash: 5,
        });
        state.pci_device_paths = vec![rust_acpi_naos::PciDeviceFunctionPath {
            device_id: 10,
            root_id: 7,
            device_function_path: vec![1, 0x23, 0x44],
        }];
        let request = idl::list_devices_request {
            firmware_version: state.generation.firmware_version.as_str(),
            platform_topology_generation: 3,
            cursor: 0,
            limit: 4,
            filter_flags: 0,
        };
        let request_wire = encode_list_request(&request);
        let response_wire = dispatch(idl::METHOD_LIST_DEVICES, &request_wire, &state).unwrap();
        let response = idl::decode_list_devices_response(&response_wire).unwrap();

        assert_eq!(response.devices[0].pci_root_id, 7);
        assert_eq!(response.devices[0].pci_path_count, 3);
        assert_eq!(response.pci_device_function_path, vec![1, 0x23, 0x44]);
        assert_eq!(
            &response.pci_device_function_path[response.devices[0].pci_path_start as usize
                ..(response.devices[0].pci_path_start + response.devices[0].pci_path_count)
                    as usize],
            [1, 0x23, 0x44]
        );
        assert_eq!(response.devices[1].pci_root_id, 0);
        assert_eq!(response.devices[1].pci_path_count, 0);
    }

    #[test]
    fn unavailable_generation_is_degraded_and_cannot_claim_an_aml_namespace() {
        let generation = unavailable_generation("unavailable-firmware-v1".to_owned(), 3, 4);

        assert_eq!(generation.firmware_version, "unavailable-firmware-v1");
        assert_eq!(generation.aml_namespace_generation, 0);
        assert_eq!(generation.platform_topology_generation, 3);
        assert_eq!(generation.pci_fabric_generation, 4);
        assert_eq!(generation.health, HEALTH_DEGRADED | HEALTH_DISCOVERY_FAILED);
        assert_eq!(generation.compatibility_profile_hash, 0);
    }

    #[test]
    fn rebooted_topology_rejects_a_stale_firmware_version() {
        let mut current = state(Vec::new());
        current.generation.firmware_version = "current-firmware-v2".to_owned();
        let old_boot_request = encode_list_request(&idl::list_devices_request {
            firmware_version: "old-firmware-v1",
            platform_topology_generation: current.generation.platform_topology_generation,
            cursor: 0,
            limit: 1,
            filter_flags: 0,
        });
        let current_boot_request = encode_list_request(&idl::list_devices_request {
            firmware_version: "current-firmware-v2",
            platform_topology_generation: current.generation.platform_topology_generation,
            cursor: 0,
            limit: 1,
            filter_flags: 0,
        });

        assert_eq!(
            dispatch(idl::METHOD_LIST_DEVICES, &old_boot_request, &current),
            Err(-116)
        );
        assert!(dispatch(idl::METHOD_LIST_DEVICES, &current_boot_request, &current).is_ok());
    }

    #[test]
    fn sci_consumer_failure_publishes_a_new_informationally_degraded_generation() {
        let mut state = state(Vec::new());
        let (lifecycle, mut invalidated) = TopologyInvalidation::channel();
        state.roots.push(idl::PlatformPciRoot {
            root_id: 6,
            segment: 0,
            start_bus: 0,
            end_bus: 31,
            aml_namespace_generation: 9,
            platform_topology_generation: 3,
            pci_fabric_generation: 4,
            device_id: 7,
            resource_window_start: 0,
            resource_window_count: 0,
            compatibility_profile_hash: 5,
        });
        state.features.push(idl::PciRootFeatureSet {
            root_id: 6,
            requested_support: 0,
            requested_control: 0,
            query_support: 0,
            query_granted_control: 0,
            commit_support: 0,
            granted_control: 0,
            denied_control: 0,
            masked_control: 0,
            query_errors: 0,
            commit_errors: 0,
            aml_namespace_generation: 9,
            platform_topology_generation: 3,
            compatibility_profile_hash: 5,
            pci_fabric_generation: 4,
        });

        assert!(mark_sci_consumer_failed(&mut state, 4, &lifecycle));
        assert_eq!(state.generation.platform_topology_generation, 4);
        assert_eq!(state.generation.health, HEALTH_DEGRADED);
        assert_eq!(state.roots[0].platform_topology_generation, 4);
        assert_eq!(state.features[0].platform_topology_generation, 4);
        assert_eq!(*invalidated.borrow_and_update(), Some(4));
        assert_eq!(ensure_current_generation("stale", 3, &state), Err(-116));
        assert_eq!(
            ensure_current_generation("test-firmware-v1", 4, &state),
            Ok(())
        );
    }

    #[test]
    fn pci_feature_negotiation_returns_cached_query_and_commit_evidence_only_for_its_request() {
        let mut state = state(Vec::new());
        state.roots.push(idl::PlatformPciRoot {
            root_id: 6,
            segment: 0,
            start_bus: 0,
            end_bus: 31,
            aml_namespace_generation: 9,
            platform_topology_generation: 3,
            pci_fabric_generation: 4,
            device_id: 7,
            resource_window_start: 0,
            resource_window_count: 0,
            compatibility_profile_hash: 5,
        });
        state.features.push(idl::PciRootFeatureSet {
            root_id: 6,
            requested_support: idl::PciRootFeatureSupport::segment_groups as u32,
            requested_control: 0,
            query_support: idl::PciRootFeatureSupport::segment_groups as u32,
            query_granted_control: 0,
            commit_support: idl::PciRootFeatureSupport::segment_groups as u32,
            granted_control: 0,
            denied_control: 0,
            masked_control: 0,
            query_errors: 0,
            commit_errors: 0,
            aml_namespace_generation: 9,
            platform_topology_generation: 3,
            compatibility_profile_hash: 5,
            pci_fabric_generation: 4,
        });

        let request = idl::negotiate_pci_features_request {
            root_id: 6,
            firmware_version: state.generation.firmware_version.as_str(),
            platform_topology_generation: 3,
            requested_support: idl::PciRootFeatureSupport::segment_groups as u32,
            requested_control: 0,
        };
        let mut request_wire = [0_u8; 64];
        let request_length =
            idl::encode_negotiate_pci_features_request(&request, &mut request_wire).unwrap();
        let response_wire = dispatch(
            idl::METHOD_NEGOTIATE_PCI_FEATURES,
            &request_wire[..request_length],
            &state,
        )
        .unwrap();
        let response = idl::decode_negotiate_pci_features_response(&response_wire).unwrap();
        let supported_os_features = idl::PciRootFeatureSupport::segment_groups as u32;
        assert_eq!(response.value.query_support, supported_os_features);
        assert_eq!(response.value.commit_support, supported_os_features);
        assert_eq!(response.value.granted_control, 0);

        let unadvertised_support_request = idl::negotiate_pci_features_request {
            requested_support: 0,
            ..request
        };
        let request_length = idl::encode_negotiate_pci_features_request(
            &unadvertised_support_request,
            &mut request_wire,
        )
        .unwrap();
        assert_eq!(
            dispatch(
                idl::METHOD_NEGOTIATE_PCI_FEATURES,
                &request_wire[..request_length],
                &state,
            ),
            Err(-95)
        );

        let unsupported_request = idl::negotiate_pci_features_request {
            requested_control: 1,
            ..request
        };
        let request_length =
            idl::encode_negotiate_pci_features_request(&unsupported_request, &mut request_wire)
                .unwrap();
        assert_eq!(
            dispatch(
                idl::METHOD_NEGOTIATE_PCI_FEATURES,
                &request_wire[..request_length],
                &state,
            ),
            Err(-95)
        );
    }

    #[test]
    fn sci_failure_rejects_nonadvancing_generation_without_mutation() {
        let mut state = state(Vec::new());
        let (lifecycle, invalidated) = TopologyInvalidation::channel();
        assert!(!mark_sci_consumer_failed(&mut state, 3, &lifecycle));
        assert_eq!(state.generation.platform_topology_generation, 3);
        assert_eq!(state.generation.health, 0);
        assert!(!invalidated.has_changed().unwrap());
    }

    #[tokio::test]
    async fn topology_invalidation_cancels_the_active_server_future() {
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

        let (lifecycle, mut invalidated) = TopologyInvalidation::channel();
        let was_dropped = Arc::new(AtomicBool::new(false));
        let drop_flag = DropFlag(was_dropped.clone());
        let server = async move {
            let _drop_flag = drop_flag;
            core::future::pending::<i32>().await
        };
        tokio::spawn(async move {
            tokio::task::yield_now().await;
            lifecycle.invalidate(5);
        });

        assert_eq!(
            serve_until_invalidation(server, &mut invalidated).await,
            TopologyServeOutcome::Invalidated(5)
        );
        assert!(was_dropped.load(Ordering::Acquire));
    }

    fn encode_list_request(request: &idl::list_devices_request) -> Vec<u8> {
        let mut wire = vec![0_u8; 128];
        let written = idl::encode_list_devices_request(request, &mut wire).unwrap();
        wire.truncate(written);
        wire
    }

    #[test]
    fn device_page_filters_and_advances_by_stable_source_cursor() {
        let devices = vec![
            device(1, 0x0f, false),
            device(2, 0, false),
            device(3, 0x0f, true),
        ];
        let (page, next) = select_device_page(&devices, 0, 2, FILTER_PRESENT_ONLY).unwrap();
        assert_eq!(page, vec![0, 2]);
        assert_eq!(next, 0);

        let status_devices = vec![
            device(1, 0x0f, false),
            device(2, 0x01, false),
            device(3, 0, false),
        ];
        let (page, next) = select_device_page(
            &status_devices,
            0,
            MAX_DEVICES_PER_PAGE,
            FILTER_PRESENT_ONLY | FILTER_FUNCTIONAL_ONLY,
        )
        .unwrap();
        assert_eq!(page, vec![0]);
        assert_eq!(next, 0);

        let (page, next) = select_device_page(&devices, 0, 1, FILTER_DEGRADED_ONLY).unwrap();
        assert_eq!(page, vec![2]);
        assert_eq!(next, 0);

        let (page, next) = select_device_page(&devices, 1, 1, 0).unwrap();
        assert_eq!(page, vec![1]);
        assert_eq!(next, 2);
    }

    #[test]
    fn device_page_rejects_invalid_filter_limit_and_cursor() {
        let devices = vec![device(1, 0x0f, false)];
        assert_eq!(
            select_device_page(&devices, 0, 0, 0),
            Err(PageError::InvalidLimit)
        );
        assert_eq!(
            select_device_page(&devices, 0, MAX_DEVICES_PER_PAGE + 1, 0),
            Err(PageError::InvalidLimit)
        );
        assert_eq!(
            select_device_page(&devices, 0, 1, !DEVICE_FILTER_MASK),
            Err(PageError::InvalidFilter)
        );
        assert_eq!(
            select_device_page(&devices, 2, 1, 0),
            Err(PageError::CursorOutOfRange)
        );
    }

    #[test]
    fn list_devices_encodes_flattened_identifiers_and_resources_with_stable_cursors() {
        let mut first = device(11, 0x0f, false);
        first.identifiers = vec![
            DeviceIdentifier {
                kind: DeviceIdentifierKind::Hid,
                value: DeviceIdentifierValue::String("PNP0A08".to_string()),
            },
            DeviceIdentifier {
                kind: DeviceIdentifierKind::Cid,
                value: DeviceIdentifierValue::Integer(0x030a_d041),
            },
            DeviceIdentifier {
                kind: DeviceIdentifierKind::Uid,
                value: DeviceIdentifierValue::Integer(7),
            },
        ];
        first.resources = vec![ResourceSnapshot {
            kind: 2,
            base: 0x8000_0000,
            length: 0x1000,
            flags: 3,
        }];
        first.resources_evaluated = true;
        let devices = vec![first, device(12, 0, false), device(13, 0x0f, true)];
        let state = state(devices);

        let first_request = encode_list_request(&idl::list_devices_request {
            firmware_version: state.generation.firmware_version.as_str(),
            platform_topology_generation: 3,
            cursor: 0,
            limit: 1,
            filter_flags: FILTER_PRESENT_ONLY,
        });
        let first_wire = dispatch(idl::METHOD_LIST_DEVICES, &first_request, &state).unwrap();
        let first_page = idl::decode_list_devices_response(&first_wire).unwrap();
        assert_eq!(first_page.devices.len(), 1);
        assert_eq!(first_page.devices[0].device_id, 11);
        assert_eq!(first_page.devices[0].identifier_count, 3);
        assert_eq!(first_page.devices[0].resource_count, 1);
        assert_eq!(first_page.next_cursor, 1);
        assert_eq!(first_page.identifiers.len(), 3);
        assert_eq!(first_page.identifiers[0].kind, 1);
        assert_eq!(first_page.identifiers[0].text_start, 0);
        assert_eq!(first_page.identifiers[0].text_length, 7);
        assert_eq!(&first_page.identifier_text[..7], b"PNP0A08");
        assert_eq!(first_page.identifiers[1].value_kind, 2);
        assert_eq!(first_page.identifiers[1].integer, 0x030a_d041);
        assert_eq!(first_page.identifiers[2].kind, 3);
        assert_eq!(first_page.identifiers[2].value_kind, 2);
        assert_eq!(first_page.identifiers[2].integer, 7);
        assert_eq!(first_page.resources[0].base, 0x8000_0000);

        let second_request = encode_list_request(&idl::list_devices_request {
            firmware_version: state.generation.firmware_version.as_str(),
            platform_topology_generation: 3,
            cursor: first_page.next_cursor,
            limit: 1,
            filter_flags: FILTER_PRESENT_ONLY,
        });
        let second_wire = dispatch(idl::METHOD_LIST_DEVICES, &second_request, &state).unwrap();
        let second_page = idl::decode_list_devices_response(&second_wire).unwrap();
        assert_eq!(second_page.devices[0].device_id, 13);
        assert_eq!(second_page.next_cursor, 0);
    }

    #[test]
    fn get_resources_is_generation_scoped_and_reports_evaluation_state() {
        let mut item = device(55, 0, true);
        item.resources = vec![ResourceSnapshot {
            kind: 4,
            base: 0x3f8,
            length: 8,
            flags: 1,
        }];
        item.resources_evaluated = true;
        let state = state(vec![item]);
        let request = idl::get_resources_request {
            device_id: 55,
            firmware_version: state.generation.firmware_version.as_str(),
            platform_topology_generation: 3,
        };
        let mut request_wire = [0_u8; 64];
        let written = idl::encode_get_resources_request(&request, &mut request_wire).unwrap();
        let response_wire =
            dispatch(idl::METHOD_GET_RESOURCES, &request_wire[..written], &state).unwrap();
        let response = idl::decode_get_resources_response(&response_wire).unwrap();
        assert_eq!(response.resources.len(), 1);
        assert_eq!(response.resources[0].base, 0x3f8);
        assert_eq!(response.evaluated, 1);
        assert_eq!(response.degraded, 1);

        let stale = idl::get_resources_request {
            platform_topology_generation: 4,
            ..request
        };
        let stale_size = idl::encode_get_resources_request(&stale, &mut request_wire).unwrap();
        assert_eq!(
            dispatch(
                idl::METHOD_GET_RESOURCES,
                &request_wire[..stale_size],
                &state
            ),
            Err(-116)
        );

        let unknown = idl::get_resources_request {
            device_id: 56,
            ..request
        };
        let unknown_size = idl::encode_get_resources_request(&unknown, &mut request_wire).unwrap();
        assert_eq!(
            dispatch(
                idl::METHOD_GET_RESOURCES,
                &request_wire[..unknown_size],
                &state
            ),
            Err(-2)
        );
    }

    #[test]
    fn get_interrupt_routing_pages_typed_routes_without_aml_paths() {
        let mut item = device(77, 0xf, false);
        item.routing = vec![
            PciRoutingEntry {
                address: 0x0002_ffff,
                pin: 0,
                source: PciRoutingSource::DirectGsi(23),
            },
            PciRoutingEntry {
                address: 0x0003_ffff,
                pin: 3,
                source: PciRoutingSource::LinkDevice {
                    device_id: 0xabc,
                    source_index: 1,
                },
            },
        ];
        item.routing_entries = item.routing.len() as u32;
        item.routing_evaluated = true;
        item.routing_complete = true;
        let state = state(vec![item]);

        let request = idl::get_interrupt_routing_request {
            device_id: 77,
            firmware_version: state.generation.firmware_version.as_str(),
            platform_topology_generation: 3,
            cursor: 0,
            limit: 1,
        };
        let mut request_wire = [0_u8; 128];
        let written =
            idl::encode_get_interrupt_routing_request(&request, &mut request_wire).unwrap();
        let response_wire = dispatch(
            idl::METHOD_GET_INTERRUPT_ROUTING,
            &request_wire[..written],
            &state,
        )
        .unwrap();
        let first = idl::decode_get_interrupt_routing_response(&response_wire).unwrap();
        assert_eq!(first.evaluated, 1);
        assert_eq!(first.complete, 1);
        assert_eq!(first.next_cursor, 1);
        assert_eq!(first.routes.len(), 1);
        assert_eq!(first.routes[0].address, 0x0002_ffff);
        assert_eq!(
            first.routes[0].source_kind,
            idl::PciRoutingSourceKind::direct_gsi
        );
        assert_eq!(first.routes[0].source_device_id, 0);
        assert_eq!(first.routes[0].source_index, 23);

        let next_request = idl::get_interrupt_routing_request {
            cursor: first.next_cursor,
            ..request
        };
        let written =
            idl::encode_get_interrupt_routing_request(&next_request, &mut request_wire).unwrap();
        let response_wire = dispatch(
            idl::METHOD_GET_INTERRUPT_ROUTING,
            &request_wire[..written],
            &state,
        )
        .unwrap();
        let second = idl::decode_get_interrupt_routing_response(&response_wire).unwrap();
        assert_eq!(second.next_cursor, 0);
        assert_eq!(second.routes.len(), 1);
        assert_eq!(
            second.routes[0].source_kind,
            idl::PciRoutingSourceKind::link_device
        );
        assert_eq!(second.routes[0].source_device_id, 0xabc);
        assert_eq!(second.routes[0].source_index, 1);

        let stale = idl::get_interrupt_routing_request {
            platform_topology_generation: 4,
            ..request
        };
        let written = idl::encode_get_interrupt_routing_request(&stale, &mut request_wire).unwrap();
        assert_eq!(
            dispatch(
                idl::METHOD_GET_INTERRUPT_ROUTING,
                &request_wire[..written],
                &state,
            ),
            Err(-116)
        );
    }
}
