use crate::volatile::{ScalarAccessError, load_volatile, store_volatile};
use acpi::aml::op_region::{OpRegion, RegionSpace};
use naos_idl::acpi_resource_authority;
use naos_idl::{OwnedHandle, ProtocolClientEndpoint, ResourceTransferBuilder};
use rust_acpi_naos::native::NativeRegionBackend;
use rust_acpi_naos::region::{
    AccessMode, CacheType, LeaseKind, OperationRegion, RegionAccessToken, RegionAccessor,
    RegionError, RegionSpace as NaosRegionSpace, ResourceLease, SystemIoResource,
};
use servicekit::memory::{MappedMemory, MemoryObject};
use std::collections::{BTreeMap, HashMap};
use std::mem::ManuallyDrop;
use std::sync::{Arc, Mutex};

const ACPI_RESOURCE_AUTHORITY_URI: &str = "naos://kernel/hw/acpi-resource-authority";
const MAX_PCI_CONFIG_PATHS: usize = 16_384;

/// Invoke the owner-only mint method without transferring or duplicating the
/// unique root attestation. ManuallyDrop is required because the temporary
/// endpoint view aliases the still-owned handle and must never close it.
pub fn mint_pci_device_resource_attestation(
    root_attestation: &OwnedHandle,
    evidence: naos_idl::pci_root_resource_attestation::mint_device_resources_request,
) -> Result<OwnedHandle, String> {
    if !root_attestation.is_valid() {
        return Err("PCI root resource attestation handle is invalid".to_string());
    }
    // SAFETY: this view is used synchronously and is wrapped in ManuallyDrop;
    // root_attestation remains the sole owner and outlives the whole call.
    let endpoint =
        ManuallyDrop::new(unsafe { ProtocolClientEndpoint::from_raw(root_attestation.get()) });
    let mut request_wire = vec![
        0_u8;
        naos_idl::pci_root_resource_attestation::MINT_DEVICE_RESOURCES_REQUEST_HEADER_BYTES
            + rust_acpi_naos::MAX_PCI_DEVICE_RESOURCE_WINDOWS * 20
    ];
    let mut invocation = naos_idl::pci_root_resource_attestation::submit_mint_device_resources(
        &endpoint,
        &evidence,
        ResourceTransferBuilder::new(),
        &mut request_wire,
        0,
    )
    .map_err(|error| format!("mint PCI device resource attestation submit: {error:?}"))?;
    match crate::region_wait::wait_for_kernel_call(invocation.get(), servicekit::wait_ready) {
        crate::region_wait::KernelCallWait::Completed => {}
        outcome => {
            return Err(format!(
                "mint PCI device resource attestation wait: {outcome:?}"
            ));
        }
    }
    let mut response_wire = [0_u8; 128];
    let (response, mut resources) =
        naos_idl::pci_root_resource_attestation::take_mint_device_resources(
            &mut invocation,
            &mut response_wire,
        )
        .map_err(|error| format!("mint PCI device resource attestation rejected: {error:?}"))?;
    resources
        .take(response.authority)
        .ok_or_else(|| "kernel omitted the PCI device resource attestation capability".to_string())
}

#[derive(Clone, Copy)]
struct PciConfigSelector {
    scope_id: u64,
    root_id: u64,
    segment: u32,
    root_start_bus: u32,
    root_end_bus: u32,
    path_length: u32,
    device_function_path: [u8; 16],
}

#[derive(Clone)]
struct Identity {
    firmware_version: String,
}

struct RegionState {
    lease_id: u64,
    space: u32,
    base: u64,
    length: u64,
    parent_device_id: u64,
    mode: AccessMode,
    namespace_generation: u64,
    resource_generation: u64,
    logical_offset: u64,
    mapping: Option<MappedMemory>,
}

// MappedMemory owns a process VMA. The AML executor thread owns this backend
// for the full interpreter lifetime; the mutex also serializes callbacks and
// preserves the map's address/lifetime while entries are borrowed.
unsafe impl Send for RegionState {}

pub struct KernelNativeRegionBackend {
    authority: Mutex<ProtocolClientEndpoint>,
    identity: Identity,
    allow_writes: bool,
    regions: Mutex<HashMap<u64, RegionState>>,
    pci_config_paths: Mutex<HashMap<String, PciConfigSelector>>,
    system_io_resources: Mutex<Option<Vec<SystemIoResource>>>,
}

impl KernelNativeRegionBackend {
    pub fn connect(
        service_directory: naos_sys::Handle,
        firmware_version: &str,
    ) -> Result<Arc<Self>, String> {
        let authority =
            servicekit::resolve_resource(service_directory, ACPI_RESOURCE_AUTHORITY_URI)
                .map_err(|error| format!("AcpiResourceAuthority resolve: {error:?}"))?;
        let authority = unsafe { ProtocolClientEndpoint::from_raw(authority.into_raw()) };
        Ok(Arc::new(Self {
            authority: Mutex::new(authority),
            identity: Identity {
                firmware_version: firmware_version.to_owned(),
            },
            // The first controlled execution profile is read-only. AML
            // methods that attempt a hardware write fail closed and are
            // reported as degraded; enabling writes is a separate policy
            // decision, not an accidental consequence of mapping.
            allow_writes: false,
            regions: Mutex::new(HashMap::new()),
            pci_config_paths: Mutex::new(HashMap::new()),
            system_io_resources: Mutex::new(None),
        }))
    }

    pub fn install_system_io_resources(
        &self,
        resources: Vec<SystemIoResource>,
    ) -> Result<usize, String> {
        if resources.len() > 4096
            || resources.iter().any(|resource| {
                !rust_acpi_naos::region::system_io_region_is_authorized(
                    &resource.parent_device_path,
                    resource.base,
                    resource.length,
                    core::slice::from_ref(resource),
                )
            })
        {
            return Err("invalid or oversized SystemIO `_CRS` allowlist".to_string());
        }
        let mut installed = self
            .system_io_resources
            .lock()
            .map_err(|_| "SystemIO `_CRS` allowlist lock poisoned".to_string())?;
        if installed.is_some() {
            return Err("SystemIO `_CRS` allowlist is already installed".to_string());
        }
        let count = resources.len();
        *installed = Some(resources);
        Ok(count)
    }

    pub fn bind_execution_thread(&self, execution_thread_id: u64) -> Result<(), String> {
        if execution_thread_id == 0 {
            return Err("AML execution thread ID is invalid".to_string());
        }
        let endpoint = self
            .authority
            .lock()
            .map_err(|_| "AcpiResourceAuthority lock poisoned".to_string())?;
        let request = acpi_resource_authority::bind_execution_thread_request {
            request: acpi_resource_authority::AcpiResourceThreadBindRequest {
                execution_thread_id,
            },
            firmware_version: self.identity.firmware_version.as_str(),
        };
        let mut request_wire = vec![
            0_u8;
            acpi_resource_authority::BIND_EXECUTION_THREAD_REQUEST_HEADER_BYTES
                + request.firmware_version.len()
        ];
        let mut invocation = acpi_resource_authority::submit_bind_execution_thread(
            &endpoint,
            &request,
            ResourceTransferBuilder::new(),
            &mut request_wire,
            0,
        )
        .map_err(|error| format!("bind AML execution thread submit: {error:?}"))?;
        Self::wait_for_executor_call(invocation.get())
            .map_err(|error| format!("bind AML execution thread call failed: {error:?}"))?;
        let mut response_wire = [0_u8; 32];
        acpi_resource_authority::take_bind_execution_thread(&mut invocation, &mut response_wire)
            .map_err(|error| format!("bind AML execution thread rejected: {error:?}"))?;
        Ok(())
    }

    pub fn bind_platform_generation(
        &self,
        aml_namespace_generation: u64,
        platform_topology_generation: u64,
    ) -> Result<(), String> {
        if aml_namespace_generation == 0 || platform_topology_generation == 0 {
            return Err("platform generation lineage contains a zero generation".to_string());
        }
        let endpoint = self
            .authority
            .lock()
            .map_err(|_| "AcpiResourceAuthority lock poisoned".to_string())?;
        let request = acpi_resource_authority::bind_platform_generation_request {
            binding: acpi_resource_authority::AcpiPlatformGenerationBinding {
                aml_namespace_generation,
                platform_topology_generation,
            },
            firmware_version: self.identity.firmware_version.as_str(),
        };
        let mut request_wire = vec![
            0_u8;
            acpi_resource_authority::BIND_PLATFORM_GENERATION_REQUEST_HEADER_BYTES
                + request.firmware_version.len()
        ];
        let mut invocation = acpi_resource_authority::submit_bind_platform_generation(
            &endpoint,
            &request,
            ResourceTransferBuilder::new(),
            &mut request_wire,
            0,
        )
        .map_err(|error| format!("bind platform generation submit: {error:?}"))?;
        Self::wait_for_executor_call(invocation.get())
            .map_err(|error| format!("bind platform generation call failed: {error:?}"))?;
        let mut response_wire = [0_u8; 32];
        acpi_resource_authority::take_bind_platform_generation(&mut invocation, &mut response_wire)
            .map_err(|error| format!("bind platform generation rejected: {error:?}"))?;
        Ok(())
    }

    pub fn bind_pci_config_root(
        &self,
        namespace_generation: u64,
        platform_topology_generation: u64,
        compatibility_profile_hash: u64,
        root: &rust_acpi_naos::PciRootBridgeDescriptor,
    ) -> Result<u64, String> {
        let endpoint = self
            .authority
            .lock()
            .map_err(|_| "AcpiResourceAuthority lock poisoned".to_string())?;
        let request = acpi_resource_authority::bind_pci_config_root_request {
            request: acpi_resource_authority::AcpiPciConfigRootRequest {
                namespace_generation,
                root_id: root.root_id,
                segment: u32::from(root.segment),
                start_bus: u32::from(root.start_bus),
                end_bus: u32::from(root.end_bus),
                platform_topology_generation,
                compatibility_profile_hash,
            },
            firmware_version: self.identity.firmware_version.as_str(),
        };
        let mut request_wire = vec![
            0_u8;
            acpi_resource_authority::BIND_PCI_CONFIG_ROOT_REQUEST_HEADER_BYTES
                + request.firmware_version.len()
        ];
        let mut invocation = acpi_resource_authority::submit_bind_pci_config_root(
            &endpoint,
            &request,
            ResourceTransferBuilder::new(),
            &mut request_wire,
            0,
        )
        .map_err(|error| format!("bind PCI_Config root request: {error:?}"))?;
        Self::wait_for_executor_call(invocation.get())
            .map_err(|error| format!("bind PCI_Config root call failed: {error:?}"))?;
        let mut response_wire = [0_u8; 128];
        let response =
            acpi_resource_authority::take_bind_pci_config_root(&mut invocation, &mut response_wire)
                .map_err(|error| {
                    format!(
                        "bind PCI_Config root rejected: {:?}",
                        Self::map_invocation_error(error)
                    )
                })?;
        Ok(response.response.scope_id)
    }

    pub fn validate_pci_root_features(
        &self,
        evidence: &acpi_resource_authority::PciRootFeatureEvidence,
        firmware_version: &str,
    ) -> Result<(), String> {
        let endpoint = self
            .authority
            .lock()
            .map_err(|_| "AcpiResourceAuthority lock poisoned".to_string())?;
        let request = acpi_resource_authority::validate_pci_root_features_request {
            evidence: *evidence,
            firmware_version,
        };
        let mut request_wire = vec![
            0_u8;
            acpi_resource_authority::VALIDATE_PCI_ROOT_FEATURES_REQUEST_HEADER_BYTES
                + request.firmware_version.len()
        ];
        let mut invocation = acpi_resource_authority::submit_validate_pci_root_features(
            &endpoint,
            &request,
            ResourceTransferBuilder::new(),
            &mut request_wire,
            0,
        )
        .map_err(|error| format!("validate PCI root feature evidence submit: {error:?}"))?;
        Self::wait_for_executor_call(invocation.get())
            .map_err(|error| format!("validate PCI root feature evidence wait: {error:?}"))?;
        let mut response_wire = [0_u8; 32];
        acpi_resource_authority::take_validate_pci_root_features(
            &mut invocation,
            &mut response_wire,
        )
        .map(|_| ())
        .map_err(|error| {
            format!(
                "validate PCI root feature evidence rejected: {:?}",
                Self::map_invocation_error(error)
            )
        })
    }

    pub fn mint_pci_root_resource_attestation(
        &self,
        evidence: acpi_resource_authority::mint_pci_root_resource_attestation_request,
    ) -> Result<OwnedHandle, String> {
        let endpoint = self
            .authority
            .lock()
            .map_err(|_| "AcpiResourceAuthority lock poisoned".to_string())?;
        let mut request_wire = vec![
            0_u8;
            acpi_resource_authority::MINT_PCI_ROOT_RESOURCE_ATTESTATION_REQUEST_HEADER_BYTES
                + rust_acpi_naos::MAX_PCI_ROOT_RESOURCE_WINDOWS * 64
                + evidence.firmware_version.len()
        ];
        let mut invocation = acpi_resource_authority::submit_mint_pci_root_resource_attestation(
            &endpoint,
            &evidence,
            ResourceTransferBuilder::new(),
            &mut request_wire,
            0,
        )
        .map_err(|error| format!("mint PCI root resource attestation submit: {error:?}"))?;
        Self::wait_for_executor_call(invocation.get())
            .map_err(|error| format!("mint PCI root resource attestation wait: {error:?}"))?;
        let mut response_wire = [0_u8; 128];
        let (response, mut resources) =
            acpi_resource_authority::take_mint_pci_root_resource_attestation(
                &mut invocation,
                &mut response_wire,
            )
            .map_err(|error| {
                format!(
                    "mint PCI root resource attestation rejected: {:?}",
                    Self::map_invocation_error(error)
                )
            })?;
        resources.take(response.authority).ok_or_else(|| {
            "kernel omitted the PCI root resource attestation capability".to_string()
        })
    }

    pub fn install_pci_config_paths(
        &self,
        paths: Vec<rust_acpi_naos::PciConfigRegionPath>,
        root_scopes: &BTreeMap<u64, u64>,
    ) -> Result<usize, String> {
        if paths.len() > MAX_PCI_CONFIG_PATHS {
            return Err("PCI_Config path table exceeds namespace bound".to_string());
        }
        let mut configured = HashMap::new();
        configured
            .try_reserve(paths.len())
            .map_err(|_| "PCI_Config path table allocation failed".to_string())?;
        for path in paths {
            let Some(scope_id) = root_scopes.get(&path.root_id).copied() else {
                continue;
            };
            if path.device_function_path.len() > 16 {
                return Err("PCI_Config `_ADR` path exceeds kernel bound".to_string());
            }
            let mut selector = PciConfigSelector {
                scope_id,
                root_id: path.root_id,
                segment: u32::from(path.segment),
                root_start_bus: u32::from(path.root_start_bus),
                root_end_bus: u32::from(path.root_end_bus),
                path_length: path.device_function_path.len() as u32,
                device_function_path: [0; 16],
            };
            selector.device_function_path[..path.device_function_path.len()]
                .copy_from_slice(&path.device_function_path);
            let key = path.parent_device_path;
            if configured.insert(key, selector).is_some_and(|previous| {
                previous.scope_id != selector.scope_id
                    || previous.root_id != selector.root_id
                    || previous.segment != selector.segment
                    || previous.path_length != selector.path_length
                    || previous.device_function_path != selector.device_function_path
            }) {
                return Err("conflicting PCI_Config selector for one AML parent".to_string());
            }
        }
        let count = configured.len();
        *self
            .pci_config_paths
            .lock()
            .map_err(|_| "PCI_Config path table lock poisoned".to_string())? = configured;
        Ok(count)
    }

    fn endpoint(&self) -> Result<std::sync::MutexGuard<'_, ProtocolClientEndpoint>, RegionError> {
        self.authority
            .lock()
            .map_err(|_| RegionError::NoMatchingLease)
    }

    fn wait_for_executor_call(handle: naos_sys::Handle) -> Result<(), RegionError> {
        Self::wait_for_executor_call_with_timeout(
            handle,
            Some(crate::region_wait::REGION_INVOCATION_TIMEOUT_US),
        )
    }

    fn wait_for_executor_call_with_timeout(
        handle: naos_sys::Handle,
        timeout_us: Option<u64>,
    ) -> Result<(), RegionError> {
        match crate::region_wait::wait_for_kernel_call_with_timeout(
            handle,
            timeout_us,
            servicekit::wait_ready,
        ) {
            crate::region_wait::KernelCallWait::Completed => Ok(()),
            crate::region_wait::KernelCallWait::TimedOut => {
                Err(RegionError::InvocationOutcomeUnknown)
            }
            crate::region_wait::KernelCallWait::PeerClosed => {
                Err(RegionError::InvocationEndpointClosed)
            }
            crate::region_wait::KernelCallWait::ReadinessError => {
                Err(RegionError::InvocationReadinessError)
            }
            crate::region_wait::KernelCallWait::UnexpectedEvent => {
                Err(RegionError::InvocationUnexpectedReadiness)
            }
            crate::region_wait::KernelCallWait::Failed(status) => {
                Err(RegionError::InvocationWaitStatus(status))
            }
        }
    }

    fn map_invocation_error(error: naos_idl::CallError) -> RegionError {
        match crate::region_wait::classify_call_error(error) {
            crate::region_wait::KernelCallFailure::Outcome {
                execution,
                reason,
                protocol_error,
            } => RegionError::InvocationOutcome {
                execution,
                reason,
                protocol_error,
            },
            crate::region_wait::KernelCallFailure::Status(status) => {
                RegionError::InvocationStatus(status)
            }
            crate::region_wait::KernelCallFailure::InvalidInvocation => {
                RegionError::InvalidInvocation
            }
            crate::region_wait::KernelCallFailure::InvalidResult => {
                RegionError::InvalidInvocationResult
            }
        }
    }

    fn parent_device_id(parent_device_path: &str) -> u64 {
        let mut hash = 0xcbf29ce484222325_u64;
        for byte in parent_device_path.as_bytes() {
            hash ^= u64::from(*byte);
            hash = hash.wrapping_mul(0x100000001b3);
        }
        if hash == 0 { 1 } else { hash }
    }

    fn space(space: RegionSpace) -> Result<u32, RegionError> {
        match space {
            RegionSpace::SystemMemory => Ok(0),
            RegionSpace::SystemIO => Ok(1),
            RegionSpace::PciConfig => Ok(2),
            _ => Err(RegionError::UnsupportedSpace),
        }
    }

    fn naos_space(space: u32) -> Result<NaosRegionSpace, RegionError> {
        match space {
            0 => Ok(NaosRegionSpace::SystemMemory),
            1 => Ok(NaosRegionSpace::SystemIo),
            2 => Ok(NaosRegionSpace::PciConfig),
            _ => Err(RegionError::UnsupportedSpace),
        }
    }

    fn mode(mode: AccessMode) -> u32 {
        match mode {
            AccessMode::Read => 0,
            AccessMode::ReadWrite => 1,
        }
    }

    fn request(
        &self,
        region: &OpRegion,
        parent_device_path: &str,
        namespace_generation: u64,
        mode: AccessMode,
    ) -> Result<acpi_resource_authority::AcpiRegionRequest, RegionError> {
        let mut request = acpi_resource_authority::AcpiRegionRequest {
            namespace_generation,
            parent_device_id: Self::parent_device_id(parent_device_path),
            base: region.base,
            length: region.length,
            space: Self::space(region.space)?,
            mode: Self::mode(mode),
            pci_config_scope_id: 0,
            pci_root_id: 0,
            pci_segment: 0,
            pci_root_start_bus: 0,
            pci_root_end_bus: 0,
            pci_path_length: 0,
            pci_device_function_path: [0; 16],
        };
        if request.space == 2 {
            let selector = self
                .pci_config_paths
                .lock()
                .map_err(|_| RegionError::NoMatchingLease)?
                .get(parent_device_path)
                .copied()
                .ok_or(RegionError::NoMatchingLease)?;
            request.pci_config_scope_id = selector.scope_id;
            request.pci_root_id = selector.root_id;
            request.pci_segment = selector.segment;
            request.pci_root_start_bus = selector.root_start_bus;
            request.pci_root_end_bus = selector.root_end_bus;
            request.pci_path_length = selector.path_length;
            request.pci_device_function_path = selector.device_function_path;
        }
        Ok(request)
    }

    fn authorize_kernel(
        &self,
        region: &OpRegion,
        parent_device_path: &str,
        namespace_generation: u64,
        mode: AccessMode,
    ) -> Result<acpi_resource_authority::AcpiRegionGrant, RegionError> {
        let endpoint = self.endpoint()?;
        let request = acpi_resource_authority::authorize_region_request {
            request: self.request(region, parent_device_path, namespace_generation, mode)?,
            firmware_version: self.identity.firmware_version.as_str(),
        };
        let mut request_wire = vec![
            0_u8;
            acpi_resource_authority::AUTHORIZE_REGION_REQUEST_HEADER_BYTES
                + request.firmware_version.len()
        ];
        let mut invocation = acpi_resource_authority::submit_authorize_region(
            &endpoint,
            &request,
            ResourceTransferBuilder::new(),
            &mut request_wire,
            0,
        )
        .map_err(Self::map_invocation_error)?;
        Self::wait_for_executor_call(invocation.get())?;
        let mut response_wire = [0_u8; 256];
        match acpi_resource_authority::take_authorize_region(&mut invocation, &mut response_wire) {
            Ok(response) => Ok(response.grant),
            Err(error) => {
                log::warn!("ACPI native region authorization rejected: {error:?}");
                Err(Self::map_invocation_error(error))
            }
        }
    }

    fn map_kernel(
        &self,
        region: &OpRegion,
        parent_device_path: &str,
        namespace_generation: u64,
        mode: AccessMode,
        lease_id: u64,
    ) -> Result<(acpi_resource_authority::AcpiRegionGrant, MappedMemory), RegionError> {
        let endpoint = self.endpoint()?;
        let request = acpi_resource_authority::map_region_request {
            request: self.request(region, parent_device_path, namespace_generation, mode)?,
            lease_id,
            firmware_version: self.identity.firmware_version.as_str(),
        };
        let mut request_wire = vec![
            0_u8;
            acpi_resource_authority::MAP_REGION_REQUEST_HEADER_BYTES
                + request.firmware_version.len()
        ];
        let mut invocation = acpi_resource_authority::submit_map_region(
            &endpoint,
            &request,
            ResourceTransferBuilder::new(),
            &mut request_wire,
            0,
        )
        .map_err(Self::map_invocation_error)?;
        Self::wait_for_executor_call(invocation.get())?;
        let mut response_wire = [0_u8; 256];
        let (response, mut resources) =
            acpi_resource_authority::take_map_region(&mut invocation, &mut response_wire)
                .map_err(Self::map_invocation_error)?;
        let mapping_handle = resources
            .take(response.mapping)
            .ok_or(RegionError::NoMatchingLease)?;
        let object = MemoryObject::from_owned(mapping_handle);
        let bytes =
            usize::try_from(response.grant.mapping_bytes).map_err(|_| RegionError::Overflow)?;
        if bytes == 0 {
            return Err(RegionError::NoMatchingLease);
        }
        let flags = match mode {
            AccessMode::Read => {
                servicekit::sys::MEMORY_MAP_READ | servicekit::sys::MEMORY_MAP_SHARED
            }
            AccessMode::ReadWrite => {
                servicekit::sys::MEMORY_MAP_READ
                    | servicekit::sys::MEMORY_MAP_WRITE
                    | servicekit::sys::MEMORY_MAP_SHARED
            }
        };
        let mapping = object
            .map(0, bytes, flags)
            .map_err(|_| RegionError::NoMatchingLease)?;
        Ok((response.grant, mapping))
    }

    fn token_lease(
        &self,
        region: &OpRegion,
        parent_device_path: &str,
        grant: &acpi_resource_authority::AcpiRegionGrant,
        mode: AccessMode,
    ) -> Result<RegionAccessToken, RegionError> {
        let space = Self::space(region.space)?;
        let naos_space = Self::naos_space(space)?;
        let parent_device_id = Self::parent_device_id(parent_device_path);
        if grant.parent_device_id != parent_device_id {
            return Err(RegionError::NoMatchingLease);
        }
        let page_base = if grant.mapping_bytes == 0 {
            if space == 1 { 0 } else { region.base & !4095 }
        } else {
            region
                .base
                .checked_sub(grant.logical_offset)
                .ok_or(RegionError::Overflow)?
        };
        let page_end = if grant.mapping_bytes == 0 {
            if space == 1 {
                region
                    .base
                    .checked_add(region.length)
                    .and_then(|end| end.checked_add(4095))
                    .ok_or(RegionError::Overflow)?
                    & !4095
            } else {
                let end = region
                    .base
                    .checked_add(region.length)
                    .ok_or(RegionError::Overflow)?;
                end.checked_add(4095).ok_or(RegionError::Overflow)? & !4095
            }
        } else {
            page_base
                .checked_add(grant.mapping_bytes)
                .ok_or(RegionError::Overflow)?
        };
        let kind = match space {
            0 => LeaseKind::DeviceMmio,
            1 => LeaseKind::IoPort,
            2 => LeaseKind::PciConfig,
            _ => return Err(RegionError::NoMatchingLease),
        };
        let lease = ResourceLease::new(
            grant.lease_id,
            parent_device_id,
            region.base,
            region.length,
            page_base,
            page_end,
            kind,
            CacheType::Uncacheable,
            grant.resource_generation,
            mode,
        )?;
        let operation = OperationRegion {
            space: naos_space,
            base: region.base,
            length: region.length,
            parent_device: parent_device_id,
            generation: grant.resource_generation,
        };
        rust_acpi_naos::region::authorize_region(operation, &[lease], mode)
    }

    fn direct_read(
        &self,
        state: &RegionState,
        address: u64,
        width: u8,
    ) -> Result<u64, RegionError> {
        if state.space == 1 {
            return unsafe { read_io(address, width) };
        }
        let mapping = state.mapping.as_ref().ok_or(RegionError::NoMatchingLease)?;
        let relative = address
            .checked_sub(state.base)
            .ok_or(RegionError::OutOfBounds)?;
        let offset = state
            .logical_offset
            .checked_add(relative)
            .ok_or(RegionError::Overflow)?;
        let end = offset
            .checked_add(u64::from(width))
            .ok_or(RegionError::Overflow)?;
        if end > mapping.as_slice().len() as u64 {
            return Err(RegionError::OutOfBounds);
        }
        unsafe { load_volatile(mapping.as_slice().as_ptr().add(offset as usize), width) }
            .map_err(map_volatile_access_error)
    }

    fn direct_write(
        &self,
        state: &mut RegionState,
        address: u64,
        width: u8,
        value: u64,
    ) -> Result<(), RegionError> {
        if state.space == 1 {
            return unsafe { write_io(address, width, value) };
        }
        let mapping = state.mapping.as_mut().ok_or(RegionError::NoMatchingLease)?;
        let relative = address
            .checked_sub(state.base)
            .ok_or(RegionError::OutOfBounds)?;
        let offset = state
            .logical_offset
            .checked_add(relative)
            .ok_or(RegionError::Overflow)?;
        let end = offset
            .checked_add(u64::from(width))
            .ok_or(RegionError::Overflow)?;
        if end > mapping.as_slice().len() as u64 {
            return Err(RegionError::OutOfBounds);
        }
        unsafe {
            store_volatile(
                mapping.as_mut_slice().as_mut_ptr().add(offset as usize),
                width,
                value,
            )
        }
        .map_err(map_volatile_access_error)?;
        Ok(())
    }

    fn release_kernel(&self, state: &RegionState) -> Result<(), RegionError> {
        self.release_kernel_lease(state.namespace_generation, state.lease_id)
    }

    fn release_kernel_lease(
        &self,
        namespace_generation: u64,
        lease_id: u64,
    ) -> Result<(), RegionError> {
        let endpoint = self.endpoint()?;
        let request = acpi_resource_authority::release_region_request {
            firmware_version: self.identity.firmware_version.as_str(),
            namespace_generation,
            lease_id,
        };
        let mut request_wire = vec![
            0_u8;
            acpi_resource_authority::RELEASE_REGION_REQUEST_HEADER_BYTES
                + request.firmware_version.len()
        ];
        let mut invocation = acpi_resource_authority::submit_release_region(
            &endpoint,
            &request,
            ResourceTransferBuilder::new(),
            &mut request_wire,
            0,
        )
        .map_err(Self::map_invocation_error)?;
        Self::wait_for_executor_call(invocation.get())?;
        let mut response_wire = [0_u8; 128];
        acpi_resource_authority::take_release_region(&mut invocation, &mut response_wire)
            .map(|_| ())
            .map_err(Self::map_invocation_error)
    }

    fn acquire_global_lock_kernel(&self, timeout_us: u64) -> Result<(), RegionError> {
        let endpoint = self
            .endpoint()
            .map_err(|_| RegionError::GlobalLockIoError)?;
        let request = acpi_resource_authority::acquire_global_lock_request {
            request: acpi_resource_authority::AcpiGlobalLockRequest { timeout_us },
            firmware_version: self.identity.firmware_version.as_str(),
        };
        let mut request_wire = vec![
            0_u8;
            acpi_resource_authority::ACQUIRE_GLOBAL_LOCK_REQUEST_HEADER_BYTES
                + request.firmware_version.len()
        ];
        let mut invocation = acpi_resource_authority::submit_acquire_global_lock(
            &endpoint,
            &request,
            ResourceTransferBuilder::new(),
            &mut request_wire,
            0,
        )
        .map_err(|_| RegionError::GlobalLockIoError)?;
        Self::wait_for_executor_call_with_timeout(
            invocation.get(),
            crate::region_wait::global_lock_kernel_wait_timeout(timeout_us),
        )
        .map_err(|_| RegionError::GlobalLockIoError)?;
        let mut response_wire = [0_u8; 64];
        let response =
            acpi_resource_authority::take_acquire_global_lock(&mut invocation, &mut response_wire)
                .map_err(|_| RegionError::GlobalLockIoError)?;
        match response.response.outcome {
            0 => Ok(()),
            1 => Err(RegionError::GlobalLockTimeout),
            2 => Err(RegionError::GlobalLockUnsupported),
            _ => Err(RegionError::GlobalLockIoError),
        }
    }

    #[cfg(feature = "acpi-global-lock-wake-smoke")]
    pub(crate) fn test_acquire_global_lock_timeout(
        &self,
        timeout_us: u64,
    ) -> Result<(), RegionError> {
        self.acquire_global_lock_kernel(timeout_us)
    }

    #[cfg(feature = "acpi-global-lock-release-failure-smoke")]
    pub(crate) fn test_release_global_lock(&self) -> Result<(), RegionError> {
        self.release_global_lock_kernel()
    }

    fn release_global_lock_kernel(&self) -> Result<(), RegionError> {
        let endpoint = self
            .endpoint()
            .map_err(|_| RegionError::GlobalLockIoError)?;
        let request = acpi_resource_authority::release_global_lock_request {
            firmware_version: self.identity.firmware_version.as_str(),
        };
        let mut request_wire = vec![
            0_u8;
            acpi_resource_authority::RELEASE_GLOBAL_LOCK_REQUEST_HEADER_BYTES
                + request.firmware_version.len()
        ];
        let mut invocation = acpi_resource_authority::submit_release_global_lock(
            &endpoint,
            &request,
            ResourceTransferBuilder::new(),
            &mut request_wire,
            0,
        )
        .map_err(|_| RegionError::GlobalLockIoError)?;
        Self::wait_for_executor_call(invocation.get())
            .map_err(|_| RegionError::GlobalLockIoError)?;
        let mut response_wire = [0_u8; 64];
        let response =
            acpi_resource_authority::take_release_global_lock(&mut invocation, &mut response_wire)
                .map_err(|_| RegionError::GlobalLockIoError)?;
        match response.response.outcome {
            0 => Ok(()),
            2 => Err(RegionError::GlobalLockUnsupported),
            _ => Err(RegionError::GlobalLockIoError),
        }
    }
}

impl NativeRegionBackend for KernelNativeRegionBackend {
    fn acquire_global_lock(&self, timeout_us: u64) -> Result<(), RegionError> {
        self.acquire_global_lock_kernel(timeout_us)
    }

    fn release_global_lock(&self) -> Result<(), RegionError> {
        self.release_global_lock_kernel()
    }

    fn execution_thread_id(&self) -> Option<u64> {
        u64::try_from(unsafe { naos_sys::_s_current_tid() })
            .ok()
            .filter(|thread_id| *thread_id != 0)
    }

    fn resource_generation(
        &self,
        region: &OpRegion,
        parent_device_path: &str,
        _namespace_generation: u64,
        mode: AccessMode,
    ) -> Result<u64, RegionError> {
        let parent = Self::parent_device_id(parent_device_path);
        let space = Self::space(region.space)?;
        if let Ok(regions) = self.regions.lock() {
            if let Some(state) = regions.values().find(|state| {
                state.space == space
                    && state.base == region.base
                    && state.length == region.length
                    && state.parent_device_id == parent
                    && state.mode == mode
            }) {
                return Ok(state.resource_generation);
            }
        }
        Ok(1)
    }

    fn authorize(
        &self,
        region: &OpRegion,
        parent_device_path: &str,
        namespace_generation: u64,
        mode: AccessMode,
    ) -> Result<RegionAccessToken, RegionError> {
        if mode == AccessMode::ReadWrite && !self.allow_writes {
            return Err(RegionError::RightsDenied);
        }
        let mut regions = self
            .regions
            .lock()
            .map_err(|_| RegionError::NoMatchingLease)?;
        regions
            .try_reserve(1)
            .map_err(|_| RegionError::HeapBudgetExceeded)?;
        if region.space == RegionSpace::SystemIO {
            let resources = self
                .system_io_resources
                .lock()
                .map_err(|_| RegionError::NoMatchingLease)?;
            if !resources.as_ref().is_some_and(|resources| {
                rust_acpi_naos::region::system_io_region_is_authorized(
                    parent_device_path,
                    region.base,
                    region.length,
                    resources,
                )
            }) {
                return Err(RegionError::RightsDenied);
            }
        }
        let grant_result =
            self.authorize_kernel(region, parent_device_path, namespace_generation, mode);
        let grant = match grant_result {
            Ok(grant) => grant,
            Err(error) => {
                log::warn!(
                    "ACPI native region grant rejected space={:?} base={:#x} length={:#x} mode={:?} parent={}: {error:?}",
                    region.space,
                    region.base,
                    region.length,
                    mode,
                    region.parent_device_path
                );
                return Err(error);
            }
        };
        let mut mapping = None;
        let mut final_grant = grant;
        if matches!(final_grant.space, 0 | 2) {
            match self.map_kernel(
                region,
                parent_device_path,
                namespace_generation,
                mode,
                final_grant.lease_id,
            ) {
                Ok((mapped_grant, mapped)) => {
                    final_grant = mapped_grant;
                    mapping = Some(mapped);
                }
                Err(error) => {
                    if self
                        .release_kernel_lease(namespace_generation, final_grant.lease_id)
                        .is_err()
                    {
                        log::warn!(
                            "ACPI direct region map failed and lease rollback also failed: lease={:#x}",
                            final_grant.lease_id
                        );
                    }
                    return Err(error);
                }
            }
        }
        let token = match self.token_lease(region, parent_device_path, &final_grant, mode) {
            Ok(token) => token,
            Err(error) => {
                if let Some(mapped) = mapping.as_mut() {
                    if mapped.unmap().is_err() {
                        log::warn!(
                            "ACPI mapped region token validation failed and unmap failed: lease={:#x}",
                            final_grant.lease_id
                        );
                    }
                } else if self
                    .release_kernel_lease(namespace_generation, final_grant.lease_id)
                    .is_err()
                {
                    log::warn!(
                        "ACPI region token validation failed and lease rollback also failed: lease={:#x}",
                        final_grant.lease_id
                    );
                }
                return Err(error);
            }
        };
        regions.insert(
            final_grant.lease_id,
            RegionState {
                lease_id: final_grant.lease_id,
                space: final_grant.space,
                base: region.base,
                length: region.length,
                parent_device_id: Self::parent_device_id(parent_device_path),
                mode,
                namespace_generation,
                resource_generation: final_grant.resource_generation,
                logical_offset: final_grant.logical_offset,
                mapping,
            },
        );
        Ok(token)
    }

    fn release(&self, lease_id: u64) -> Result<(), RegionError> {
        let mut regions = self
            .regions
            .lock()
            .map_err(|_| RegionError::NoMatchingLease)?;
        let state = regions
            .get_mut(&lease_id)
            .ok_or(RegionError::NoMatchingLease)?;

        // Keep the lease and mapping alive if unmap fails. Removing the token
        // first would report success while a stale VMA could still access the
        // physical range.
        if matches!(state.space, 0 | 2) {
            state
                .mapping
                .as_mut()
                .ok_or(RegionError::NoMatchingLease)?
                .unmap()
                .map_err(|_| RegionError::NoMatchingLease)?;
            let state = regions
                .remove(&lease_id)
                .ok_or(RegionError::NoMatchingLease)?;
            drop(regions);
            drop(state);
            return Ok(());
        }

        let state = regions
            .remove(&lease_id)
            .ok_or(RegionError::NoMatchingLease)?;
        drop(regions);
        if let Err(error) = self.release_kernel(&state) {
            if let Ok(mut regions) = self.regions.lock() {
                regions.insert(lease_id, state);
            }
            return Err(error);
        }
        Ok(())
    }
}

impl RegionAccessor for KernelNativeRegionBackend {
    fn lease_generation(&self, lease_id: u64) -> Result<u64, RegionError> {
        self.regions
            .lock()
            .map_err(|_| RegionError::NoMatchingLease)?
            .get(&lease_id)
            .map(|state| state.resource_generation)
            .ok_or(RegionError::NoMatchingLease)
    }

    fn read(&self, lease_id: u64, address: u64, width: u8) -> Result<u64, RegionError> {
        let regions = self
            .regions
            .lock()
            .map_err(|_| RegionError::NoMatchingLease)?;
        let state = regions.get(&lease_id).ok_or(RegionError::NoMatchingLease)?;
        self.direct_read(state, address, width)
    }

    fn write(&self, lease_id: u64, address: u64, width: u8, value: u64) -> Result<(), RegionError> {
        let mut regions = self
            .regions
            .lock()
            .map_err(|_| RegionError::NoMatchingLease)?;
        let state = regions
            .get_mut(&lease_id)
            .ok_or(RegionError::NoMatchingLease)?;
        self.direct_write(state, address, width, value)
    }
}

fn map_volatile_access_error(error: ScalarAccessError) -> RegionError {
    match error {
        ScalarAccessError::InvalidWidth => RegionError::InvalidWidth,
        ScalarAccessError::Alignment => RegionError::Alignment,
    }
}

#[cfg(target_arch = "x86_64")]
unsafe fn read_io(address: u64, width: u8) -> Result<u64, RegionError> {
    if address > 0xffff {
        return Err(RegionError::OutOfBounds);
    }
    let port = address as u16;
    Ok(match width {
        1 => {
            let mut value: u8;
            unsafe { core::arch::asm!("in al, dx", out("al") value, in("dx") port) };
            u64::from(value)
        }
        2 => {
            let mut value: u16;
            unsafe { core::arch::asm!("in ax, dx", out("ax") value, in("dx") port) };
            u64::from(value)
        }
        4 => {
            let mut value: u32;
            unsafe { core::arch::asm!("in eax, dx", out("eax") value, in("dx") port) };
            u64::from(value)
        }
        _ => return Err(RegionError::InvalidWidth),
    })
}

#[cfg(target_arch = "x86_64")]
unsafe fn write_io(address: u64, width: u8, value: u64) -> Result<(), RegionError> {
    if address > 0xffff {
        return Err(RegionError::OutOfBounds);
    }
    let port = address as u16;
    match width {
        1 => unsafe { core::arch::asm!("out dx, al", in("al") value as u8, in("dx") port) },
        2 => unsafe { core::arch::asm!("out dx, ax", in("ax") value as u16, in("dx") port) },
        4 => unsafe { core::arch::asm!("out dx, eax", in("eax") value as u32, in("dx") port) },
        _ => return Err(RegionError::InvalidWidth),
    }
    Ok(())
}
