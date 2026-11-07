use crate::dedicated_worker::{DedicatedWorker, WorkerError};
use crate::pci_osc_path::FixedPciOscPath;
use crate::region::KernelNativeRegionBackend;
use naos_idl::OwnedHandle;
use rust_acpi_naos::native::NativeRegionBackend;
use rust_acpi_naos::region::SystemIoResource;
use rust_acpi_naos::{
    AcpiExecutor, CompatibilityProfile, DeviceSnapshot, DeviceSnapshotPolicy, ExecutionBudget,
    FirmwareIdentity, GpeDispatchResult, GpeHandlerKind, LoadError, NamespaceSummary,
    PciConfigRegionPath, PciDeviceFunctionPath, PciOscNegotiation, PciRootBridgeDescriptor,
    PciRootDiscovery, SleepTypes,
};
use std::sync::Arc;

struct OwnedMintPciRootResourceAttestationRequest {
    pci_config_scope_id: u64,
    root_id: u64,
    firmware_version: String,
    aml_namespace_generation: u64,
    platform_topology_generation: u64,
    pci_fabric_generation: u64,
    compatibility_profile_hash: u64,
    segment: u32,
    start_bus: u32,
    end_bus: u32,
    windows: Vec<naos_idl::acpi_resource_authority::PciRootResourceWindow>,
}

impl From<naos_idl::acpi_resource_authority::mint_pci_root_resource_attestation_request<'_>>
    for OwnedMintPciRootResourceAttestationRequest
{
    fn from(
        request: naos_idl::acpi_resource_authority::mint_pci_root_resource_attestation_request<'_>,
    ) -> Self {
        Self {
            pci_config_scope_id: request.pci_config_scope_id,
            root_id: request.root_id,
            firmware_version: request.firmware_version.to_owned(),
            aml_namespace_generation: request.aml_namespace_generation,
            platform_topology_generation: request.platform_topology_generation,
            pci_fabric_generation: request.pci_fabric_generation,
            compatibility_profile_hash: request.compatibility_profile_hash,
            segment: request.segment,
            start_bus: request.start_bus,
            end_bus: request.end_bus,
            windows: request.windows,
        }
    }
}

enum Request {
    Summary,
    CompatibilityProfileHash,
    S5SleepTypes,
    PciConfigRootCandidates,
    PciConfigRegionPaths(Arc<Vec<PciRootBridgeDescriptor>>),
    PciDeviceFunctionPaths(Arc<Vec<DeviceSnapshot>>, Arc<Vec<PciRootBridgeDescriptor>>),
    PciRoots,
    PreflightSystemIoResources,
    ActivateNativeRegions,
    DeactivateNativeRegions,
    SnapshotDevices(usize, DeviceSnapshotPolicy),
    BeginSciWorkUnit,
    EndSciWorkUnit,
    ResolveGpe(u32),
    ExecuteGpe(u32, GpeHandlerKind),
    NegotiatePciFeatures(FixedPciOscPath, u32, u32),
    ValidatePciRootFeatures(
        String,
        naos_idl::acpi_resource_authority::PciRootFeatureEvidence,
    ),
    MintPciRootResourceAttestation(OwnedMintPciRootResourceAttestationRequest),
}

enum Reply {
    Summary(NamespaceSummary),
    CompatibilityProfileHash(u64),
    S5SleepTypes(SleepTypes),
    PciConfigRootCandidates(Vec<PciRootBridgeDescriptor>),
    PciConfigRegionPaths(Vec<PciConfigRegionPath>),
    PciDeviceFunctionPaths(Vec<PciDeviceFunctionPath>),
    PciRoots(PciRootDiscovery),
    SystemIoResources(Vec<SystemIoResource>),
    Activated,
    Deactivated,
    Devices(Vec<DeviceSnapshot>),
    SciWorkUnitStarted,
    SciWorkUnitEnded,
    GpeResolved(GpeDispatchResult),
    GpeExecuted,
    Negotiated(PciOscNegotiation),
    PciRootFeaturesValidated,
    RootResourceAttestation(OwnedHandle),
}

pub(crate) struct ExecutorWorker {
    worker: DedicatedWorker<Request, Result<Reply, String>>,
    native_regions_may_be_active: bool,
}

impl ExecutorWorker {
    pub(crate) fn spawn(
        dsdt: Vec<u8>,
        ssdts: Vec<Vec<u8>>,
        profile: CompatibilityProfile,
        budget: ExecutionBudget,
        identity: FirmwareIdentity,
        hardware_side_effect_profile: rust_acpi_naos::HardwareSideEffectProfile,
        backend: Arc<KernelNativeRegionBackend>,
    ) -> Result<Self, String> {
        let binding_backend = backend.clone();
        let executor_backend = backend.clone();
        let mut active_sci_work_unit = None;
        let worker = DedicatedWorker::spawn_with_setup(
            "platformd-acpi-aml",
            || {
                let thread_id = unsafe { naos_sys::_s_current_tid() };
                u64::try_from(thread_id)
                    .ok()
                    .filter(|thread_id| *thread_id != 0)
                    .ok_or_else(|| "AML worker could not identify its kernel thread".to_string())
            },
            move |thread_id| {
                binding_backend
                    .bind_execution_thread(thread_id)
                    .map_err(|error| format!("AML executor thread binding failed: {error}"))
            },
            move || {
                #[cfg(all(
                    feature = "acpi-global-lock-wake-smoke",
                    not(any(
                        feature = "acpi-global-lock-owner-exit-smoke",
                        feature = "acpi-global-lock-release-failure-smoke"
                    ))
                ))]
                match backend.test_acquire_global_lock_timeout(50_000) {
                    Err(rust_acpi_naos::region::RegionError::GlobalLockTimeout) => {
                        log::info!("ACPI TEST: kernel Global Lock finite-timeout outcome verified");
                    }
                    result => {
                        return Err(format!(
                            "ACPI TEST: expected finite Global Lock timeout, got {result:?}"
                        ));
                    }
                }
                #[cfg(feature = "acpi-global-lock-owner-exit-smoke")]
                {
                    let message = b"ACPI TEST: owner-exit Global Lock acquire started\0";
                    unsafe { naos_sys::_s_log(message.as_ptr()) };
                    println!("ACPI TEST: owner-exit Global Lock acquire started");
                    // Keep the test wait finite so a kernel admission error
                    // fails the smoke instead of hanging platformd.
                    match backend.test_acquire_global_lock_timeout(1_000_000) {
                        Ok(()) => {
                            let message =
                                b"ACPI TEST: platformd acquired FACS Global Lock before process owner exit\0";
                            unsafe { naos_sys::_s_log(message.as_ptr()) };
                            println!("ACPI TEST: platformd acquired FACS Global Lock before process owner exit");
                        }
                        Err(error) => {
                            let message = b"ACPI TEST: owner-exit Global Lock acquire failed\0";
                            unsafe { naos_sys::_s_log(message.as_ptr()) };
                            println!("ACPI TEST: owner-exit Global Lock acquire failed: {error:?}");
                            return Err(format!("ACPI TEST: owner-exit lock acquire failed: {error:?}"));
                        }
                    }
                    unsafe { naos_sys::_s_exit(73) };
                }
                #[cfg(feature = "acpi-global-lock-release-failure-smoke")]
                {
                    match backend.test_acquire_global_lock_timeout(u64::MAX) {
                        Ok(()) => {
                            let message =
                                b"ACPI TEST: platformd acquired FACS Global Lock before injected release failure\0";
                            unsafe { naos_sys::_s_log(message.as_ptr()) };
                            println!("ACPI TEST: platformd acquired FACS Global Lock before injected release failure");
                        }
                        Err(error) => {
                            return Err(format!("ACPI TEST: acquire before injected release failure failed: {error:?}"));
                        }
                    }
                    match backend.test_release_global_lock() {
                        Err(rust_acpi_naos::region::RegionError::GlobalLockIoError) => {
                            let message =
                                b"ACPI TEST: kernel returned GlobalLockIoError after injected release failure\0";
                            unsafe { naos_sys::_s_log(message.as_ptr()) };
                            println!("ACPI TEST: kernel returned GlobalLockIoError after injected release failure");
                        }
                        result => {
                            return Err(format!("ACPI TEST: expected injected release error, got {result:?}"));
                        }
                    }
                    match backend.test_acquire_global_lock_timeout(u64::MAX) {
                        Err(rust_acpi_naos::region::RegionError::GlobalLockUnsupported) => {
                            let message = b"ACPI TEST: Global Lock stayed fail-closed after release failure\0";
                            unsafe { naos_sys::_s_log(message.as_ptr()) };
                            println!("ACPI TEST: Global Lock stayed fail-closed after release failure");
                        }
                        result => {
                            return Err(format!("ACPI TEST: release failure did not quarantine Global Lock: {result:?}"));
                        }
                    }
                }
                let backend: Arc<dyn NativeRegionBackend> = backend;
                rust_acpi_naos::load_executor_with_owned_backend(
                    &dsdt,
                    &ssdts,
                    profile,
                    budget,
                    identity,
                    hardware_side_effect_profile,
                    backend,
                )
                .map_err(|error: LoadError| format!("controlled AML namespace rejected: {error:?}"))
            },
            move |state: &mut Result<AcpiExecutor, String>, request| {
                let executor = match state {
                    Ok(executor) => executor,
                    Err(error) => return Err(error.clone()),
                };
                let reply = match request {
                    Request::Summary => Reply::Summary(executor.summary()),
                    Request::CompatibilityProfileHash => {
                        Reply::CompatibilityProfileHash(executor.compatibility_profile_hash())
                    }
                    Request::S5SleepTypes => executor
                        .get_s5_sleep_types()
                        .map(Reply::S5SleepTypes)
                        .map_err(map_load_error)?,
                    Request::PciConfigRootCandidates => executor
                        .get_pci_config_root_candidates()
                        .map(Reply::PciConfigRootCandidates)
                        .map_err(map_load_error)?,
                    Request::PciConfigRegionPaths(roots) => executor
                        .pci_config_region_paths(&roots)
                        .map(Reply::PciConfigRegionPaths)
                        .map_err(map_load_error)?,
                    Request::PciDeviceFunctionPaths(devices, roots) => executor
                        .pci_device_function_paths(&devices, &roots)
                        .map(Reply::PciDeviceFunctionPaths)
                        .map_err(map_load_error)?,
                    Request::PciRoots => executor
                        .get_pci_roots()
                        .map(Reply::PciRoots)
                        .map_err(map_load_error)?,
                    Request::PreflightSystemIoResources => executor
                        .preflight_system_io_resources()
                        .map(Reply::SystemIoResources)
                        .map_err(map_load_error)?,
                    Request::ActivateNativeRegions => {
                        executor.activate_native_regions().map_err(map_load_error)?;
                        Reply::Activated
                    }
                    Request::DeactivateNativeRegions => {
                        executor
                            .deactivate_native_regions()
                            .map_err(map_load_error)?;
                        Reply::Deactivated
                    }
                    Request::SnapshotDevices(limit, policy) => executor
                        .snapshot_devices(limit, policy)
                        .map(Reply::Devices)
                        .map_err(map_load_error)?,
                    Request::BeginSciWorkUnit => {
                        // A canceled drain may leave only the worker-owned
                        // guard behind. Replace it before starting the next
                        // sequence so old work cannot poison the new budget.
                        drop(active_sci_work_unit.take());
                        active_sci_work_unit =
                            Some(executor.begin_sci_work_unit().map_err(map_load_error)?);
                        Reply::SciWorkUnitStarted
                    }
                    Request::EndSciWorkUnit => {
                        if let Some(work_unit) = active_sci_work_unit.take() {
                            work_unit.finish().map_err(map_load_error)?;
                        }
                        Reply::SciWorkUnitEnded
                    }
                    Request::ResolveGpe(gpe_number) => executor
                        .resolve_gpe_handler(gpe_number)
                        .map(Reply::GpeResolved)
                        .map_err(map_load_error)?,
                    Request::ExecuteGpe(gpe_number, kind) => executor
                        .execute_gpe_handler(gpe_number, kind)
                        .map(|()| Reply::GpeExecuted)
                        .map_err(map_load_error)?,
                    Request::NegotiatePciFeatures(path, support, control) => executor
                        .negotiate_pci_features(path.as_str(), support, control)
                        .map(Reply::Negotiated)
                        .map_err(map_load_error)?,
                    Request::ValidatePciRootFeatures(firmware_version, evidence) => {
                        executor_backend
                            .validate_pci_root_features(&evidence, &firmware_version)?;
                        Reply::PciRootFeaturesValidated
                    }
                    Request::MintPciRootResourceAttestation(request) => {
                        let evidence =
                            naos_idl::acpi_resource_authority::mint_pci_root_resource_attestation_request {
                                pci_config_scope_id: request.pci_config_scope_id,
                                root_id: request.root_id,
                                firmware_version: request.firmware_version.as_str(),
                                aml_namespace_generation: request.aml_namespace_generation,
                                platform_topology_generation: request.platform_topology_generation,
                                pci_fabric_generation: request.pci_fabric_generation,
                                compatibility_profile_hash: request.compatibility_profile_hash,
                                segment: request.segment,
                                start_bus: request.start_bus,
                                end_bus: request.end_bus,
                                windows: request.windows,
                            };
                        executor_backend
                            .mint_pci_root_resource_attestation(evidence)
                            .map(Reply::RootResourceAttestation)?
                    }
                };
                Ok(reply)
            },
        )
        .map_err(|error| format!("AML executor thread spawn failed: {error}"))?;
        let worker = Self {
            worker,
            native_regions_may_be_active: false,
        };
        // Force the worker-side table load to complete before publishing the
        // executor to the rest of platformd, and preserve its actual error.
        worker.summary()?;
        Ok(worker)
    }

    fn call(&self, request: Request) -> Result<Reply, String> {
        self.worker.call(request).map_err(|error| match error {
            WorkerError::Disconnected => "AML executor thread disconnected".to_string(),
            WorkerError::Busy => "AML executor request queue is full".to_string(),
        })?
    }

    pub(crate) fn summary(&self) -> Result<NamespaceSummary, String> {
        match self.call(Request::Summary)? {
            Reply::Summary(summary) => Ok(summary),
            _ => Err("AML executor returned an invalid summary response".to_string()),
        }
    }

    pub(crate) fn compatibility_profile_hash(&self) -> Result<u64, String> {
        match self.call(Request::CompatibilityProfileHash)? {
            Reply::CompatibilityProfileHash(hash) => Ok(hash),
            _ => Err("AML executor returned an invalid profile response".to_string()),
        }
    }

    pub(crate) async fn get_s5_sleep_types_async(&self) -> Result<SleepTypes, String> {
        let reply = self
            .worker
            .call_async(Request::S5SleepTypes)
            .await
            .map_err(|error| match error {
                WorkerError::Disconnected => "AML executor thread disconnected".to_string(),
                WorkerError::Busy => "AML executor request queue is full".to_string(),
            })??;
        match reply {
            Reply::S5SleepTypes(types) => Ok(types),
            _ => Err("AML executor returned invalid S5 sleep types".to_string()),
        }
    }

    pub(crate) fn get_pci_config_root_candidates(
        &self,
    ) -> Result<Vec<PciRootBridgeDescriptor>, String> {
        match self.call(Request::PciConfigRootCandidates)? {
            Reply::PciConfigRootCandidates(roots) => Ok(roots),
            _ => Err("AML executor returned invalid PCI root candidates".to_string()),
        }
    }

    pub(crate) fn pci_config_region_paths(
        &self,
        roots: Arc<Vec<PciRootBridgeDescriptor>>,
    ) -> Result<Vec<PciConfigRegionPath>, String> {
        match self.call(Request::PciConfigRegionPaths(roots))? {
            Reply::PciConfigRegionPaths(paths) => Ok(paths),
            _ => Err("AML executor returned invalid PCI_Config paths".to_string()),
        }
    }

    pub(crate) fn pci_device_function_paths(
        &self,
        devices: Arc<Vec<DeviceSnapshot>>,
        roots: Arc<Vec<PciRootBridgeDescriptor>>,
    ) -> Result<Vec<PciDeviceFunctionPath>, String> {
        match self.call(Request::PciDeviceFunctionPaths(devices, roots))? {
            Reply::PciDeviceFunctionPaths(paths) => Ok(paths),
            _ => Err("AML executor returned invalid PCI device paths".to_string()),
        }
    }

    pub(crate) fn get_pci_roots(&self) -> Result<PciRootDiscovery, String> {
        match self.call(Request::PciRoots)? {
            Reply::PciRoots(discovery) => Ok(discovery),
            _ => Err("AML executor returned invalid PCI root discovery".to_string()),
        }
    }

    pub(crate) fn preflight_system_io_resources(&self) -> Result<Vec<SystemIoResource>, String> {
        match self.call(Request::PreflightSystemIoResources)? {
            Reply::SystemIoResources(resources) => Ok(resources),
            _ => Err("AML executor returned invalid SystemIO `_CRS` resources".to_string()),
        }
    }

    pub(crate) fn activate_native_regions(&mut self) -> Result<(), String> {
        // Treat an interrupted/failed activation as possibly active. The
        // interpreter owns the partial-registration rollback, but the caller
        // must still explicitly confirm teardown before serving unavailable.
        self.native_regions_may_be_active = true;
        match self.call(Request::ActivateNativeRegions) {
            Ok(Reply::Activated) => Ok(()),
            Ok(_) => Err("AML executor returned invalid region activation response".to_string()),
            Err(error) => Err(error),
        }
    }

    pub(crate) fn shutdown(mut self) -> Result<(), String> {
        if self.native_regions_may_be_active {
            self.deactivate_native_regions()?;
        }
        Ok(())
    }

    pub(crate) fn deactivate_native_regions(&mut self) -> Result<(), String> {
        match self.call(Request::DeactivateNativeRegions)? {
            Reply::Deactivated => {
                self.native_regions_may_be_active = false;
                Ok(())
            }
            _ => Err("AML executor returned invalid region deactivation response".to_string()),
        }
    }

    pub(crate) fn snapshot_devices(
        &self,
        limit: usize,
        policy: DeviceSnapshotPolicy,
    ) -> Result<Vec<DeviceSnapshot>, String> {
        match self.call(Request::SnapshotDevices(limit, policy))? {
            Reply::Devices(devices) => Ok(devices),
            _ => Err("AML executor returned invalid device snapshot".to_string()),
        }
    }

    pub(crate) async fn begin_sci_work_unit(&self) -> Result<(), String> {
        let reply = self
            .worker
            .call_async(Request::BeginSciWorkUnit)
            .await
            .map_err(|error| match error {
                WorkerError::Disconnected => "AML executor thread disconnected".to_string(),
                WorkerError::Busy => "AML executor request queue is full".to_string(),
            })??;
        match reply {
            Reply::SciWorkUnitStarted => Ok(()),
            _ => Err("AML executor returned invalid SCI work-unit begin response".to_string()),
        }
    }

    pub(crate) async fn end_sci_work_unit(&self) -> Result<(), String> {
        let reply = self
            .worker
            .call_async(Request::EndSciWorkUnit)
            .await
            .map_err(|error| match error {
                WorkerError::Disconnected => "AML executor thread disconnected".to_string(),
                WorkerError::Busy => "AML executor request queue is full".to_string(),
            })??;
        match reply {
            Reply::SciWorkUnitEnded => Ok(()),
            _ => Err("AML executor returned invalid SCI work-unit end response".to_string()),
        }
    }

    pub(crate) async fn resolve_gpe_handler(
        &self,
        gpe_number: u32,
    ) -> Result<GpeDispatchResult, String> {
        let reply = self
            .worker
            .call_async(Request::ResolveGpe(gpe_number))
            .await
            .map_err(|error| match error {
                WorkerError::Disconnected => "AML executor thread disconnected".to_string(),
                WorkerError::Busy => "AML executor request queue is full".to_string(),
            })??;
        match reply {
            Reply::GpeResolved(result) => Ok(result),
            _ => Err("AML executor returned an invalid GPE resolution response".to_string()),
        }
    }

    pub(crate) async fn execute_gpe_handler(
        &self,
        gpe_number: u32,
        kind: GpeHandlerKind,
    ) -> Result<(), String> {
        let reply = self
            .worker
            .call_async(Request::ExecuteGpe(gpe_number, kind))
            .await
            .map_err(|error| match error {
                WorkerError::Disconnected => "AML executor thread disconnected".to_string(),
                WorkerError::Busy => "AML executor request queue is full".to_string(),
            })??;
        match reply {
            Reply::GpeExecuted => Ok(()),
            _ => Err("AML executor returned an invalid GPE execution response".to_string()),
        }
    }

    pub(crate) fn negotiate_pci_features(
        &self,
        root_path: &str,
        requested_support: u32,
        requested_control: u32,
    ) -> Result<PciOscNegotiation, String> {
        let Some(root_path) = FixedPciOscPath::try_from_path(root_path) else {
            return Err("invalid or oversized PCI _OSC root path".to_string());
        };
        match self.call(Request::NegotiatePciFeatures(
            root_path,
            requested_support,
            requested_control,
        ))? {
            Reply::Negotiated(negotiation) => Ok(negotiation),
            _ => Err("AML executor returned invalid _OSC response".to_string()),
        }
    }

    pub(crate) fn validate_pci_root_features(
        &self,
        evidence: naos_idl::acpi_resource_authority::PciRootFeatureEvidence,
        firmware_version: &str,
    ) -> Result<(), String> {
        match self.call(Request::ValidatePciRootFeatures(
            firmware_version.to_owned(),
            evidence,
        ))? {
            Reply::PciRootFeaturesValidated => Ok(()),
            _ => Err(
                "AML executor returned invalid PCI root feature validation response".to_string(),
            ),
        }
    }

    pub(crate) fn mint_pci_root_resource_attestation(
        &self,
        evidence: naos_idl::acpi_resource_authority::mint_pci_root_resource_attestation_request<'_>,
    ) -> Result<OwnedHandle, String> {
        match self.call(Request::MintPciRootResourceAttestation(evidence.into()))? {
            Reply::RootResourceAttestation(authority) => Ok(authority),
            _ => {
                Err("AML executor returned invalid root resource attestation response".to_string())
            }
        }
    }
}

impl Drop for ExecutorWorker {
    fn drop(&mut self) {
        if !self.native_regions_may_be_active {
            return;
        }
        if let Err(error) = self.deactivate_native_regions() {
            log::warn!("AML native region deactivation failed: {error}");
        }
    }
}

fn map_load_error(error: LoadError) -> String {
    format!("{error:?}")
}
