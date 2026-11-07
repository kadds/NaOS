use crate::block_catalog::{BlockFactoryCatalog, BlockFactoryProvider};
use crate::driver_binding::{
    DriverBinding, DriverCompatibleId, DriverFunctionIdentifiers, finish_driver_preparation,
    group_driver_plan_by_image, plan_driver_bindings_for_functions,
};
use crate::topology::{
    BridgeBusRange, GenerationLineage, MAX_PLATFORM_DEVICE_PAGE, MAX_PLATFORM_DEVICE_SNAPSHOTS,
    PLATFORM_DEVICE_FILTER_PRESENT_AND_FUNCTIONAL, PlatformCompatibleIdentifier,
    PlatformGenerationSnapshot, PlatformPciDeviceFunctionPath, PlatformPciRootSnapshot,
    append_platform_device_paths, assign_bridge_parents, associate_platform_device_paths,
    health_allows_pci_discovery, intersect_bus_window, pci_requester_id, race_topology_lifecycle,
    read_ecam_dword, valid_device_page_cursor, validate_pci_root_window_table,
};
use naos_idl::interrupt_source;
use naos_idl::pci_block_device_catalog;
use naos_idl::pci_discovery_fabric;
use naos_idl::pci_dma_domain;
use naos_idl::pci_dma_fault_source;
use naos_idl::pci_fabric_admin;
use naos_idl::pci_function;
use naos_idl::platform_pci_topology;
use naos_idl::process as process_idl;
use naos_idl::{CallError, OwnedHandle, ProtocolClientEndpoint, ResourceTransferBuilder};
use pci_types::{ConfigRegionAccess, HeaderType, PciAddress, PciHeader, PciPciBridgeHeader};
use servicekit::Context;
use servicekit::memory::{MappedMemory, MemoryObject};
use servicekit::server::{ServeHandler, ServerError};
use std::collections::BTreeSet;

#[cfg(feature = "pci-msi-route-smoke")]
mod smoke;
#[cfg(feature = "pci-msi-route-smoke")]
pub(super) async fn run_driver(context: Context) -> i64 {
    smoke::run_driver(context).await
}

const PCI_DISCOVERY_URI: &str = "naos://kernel/hw/pci-discovery";
const PLATFORM_PCI_TOPOLOGY_URI: &str = "naos://system/platform/pci-topology";
const VIRTIO_BLK_DRIVER_IMAGE_URI: &str = "naos://service/boot/module/pdrv-vblk";
const PCI_DRIVER_PROFILE_READ_CONFIG: u32 = 1 << 0;
const PCI_DRIVER_PROFILE_DMA_DOMAIN: u32 = 1 << 1;
const PCI_DRIVER_PROFILE_ROUTE_INTERRUPT: u32 = 1 << 2;
const VIRTIO_BLK_DRIVER_CATALOG: [DriverBinding; 2] = [
    DriverBinding {
        compatible_id: DriverCompatibleId::Pci {
            vendor_id: 0x1af4,
            device_id: 0x1001,
        },
        image_uri: VIRTIO_BLK_DRIVER_IMAGE_URI,
        executable_path: "/virtioblkd",
        profile_rights: PCI_DRIVER_PROFILE_READ_CONFIG
            | PCI_DRIVER_PROFILE_DMA_DOMAIN
            | PCI_DRIVER_PROFILE_ROUTE_INTERRUPT,
        profile_bar_mask: 0,
    },
    DriverBinding {
        compatible_id: DriverCompatibleId::Pci {
            vendor_id: 0x1af4,
            device_id: 0x1042,
        },
        image_uri: VIRTIO_BLK_DRIVER_IMAGE_URI,
        executable_path: "/virtioblkd",
        profile_rights: PCI_DRIVER_PROFILE_READ_CONFIG
            | PCI_DRIVER_PROFILE_DMA_DOMAIN
            | PCI_DRIVER_PROFILE_ROUTE_INTERRUPT,
        profile_bar_mask: 0,
    },
];

struct PlatformPciTopologyClient(ProtocolClientEndpoint);
struct PciDiscoveryFabricClient(ProtocolClientEndpoint);

async fn wait_for_call(handle: naos_sys::Handle, operation: &str) -> Result<(), String> {
    match servicekit::wait_ready_async(&[handle], None).await {
        Ok(event) if event.readable => Ok(()),
        Ok(event) => Err(format!(
            "{operation} completed without readable result (closed={} error={})",
            event.read_closed || event.write_closed,
            event.error
        )),
        Err(status) => Err(format!("{operation} readiness failed: {status}")),
    }
}

struct PciFunctionNode {
    address: PciAddress,
    vendor_id: u16,
    device_id: u16,
    header_type: HeaderType,
    parent: Option<PciAddress>,
    children: Vec<PciAddress>,
    platform_device_id: Option<u64>,
    compatible_identifiers: Vec<PlatformCompatibleIdentifier>,
}

struct BoundPciRoot {
    root_id: u64,
    device_id: u64,
    segment: u32,
    start_bus: u32,
    end_bus: u32,
    firmware_version: String,
    aml_namespace_generation: u64,
    platform_topology_generation: u64,
    pci_fabric_generation: u64,
    platform_root: PlatformPciRootSnapshot,
    resource_windows: Vec<platform_pci_topology::PlatformPciResourceWindow>,
    _admin: ProtocolClientEndpoint,
    functions: Vec<PciFunctionNode>,
}

struct PciFabricTree {
    generation: PlatformGenerationSnapshot,
    roots: Vec<BoundPciRoot>,
}

struct EcamAccess<'a> {
    bytes: &'a [u8],
    segment: u16,
    start_bus: u8,
    end_bus: u8,
}

impl ConfigRegionAccess for EcamAccess<'_> {
    unsafe fn read(&self, address: PciAddress, offset: u16) -> u32 {
        if address.segment() != self.segment
            || address.bus() < self.start_bus
            || address.bus() > self.end_bus
            || offset % 4 != 0
        {
            return u32::MAX;
        }
        let Some(bus_offset) = usize::from(address.bus() - self.start_bus).checked_shl(20) else {
            return u32::MAX;
        };
        let Some(device_offset) = usize::from(address.device()).checked_shl(15) else {
            return u32::MAX;
        };
        let Some(function_offset) = usize::from(address.function()).checked_shl(12) else {
            return u32::MAX;
        };
        let Some(byte_offset) = bus_offset
            .checked_add(device_offset)
            .and_then(|value| value.checked_add(function_offset))
            .and_then(|value| value.checked_add(usize::from(offset)))
        else {
            return u32::MAX;
        };
        read_ecam_dword(self.bytes, byte_offset)
    }

    unsafe fn write(&self, _address: PciAddress, _offset: u16, _value: u32) {
        // PciDiscoveryFabric is read-only. pci_types requires this method in
        // its generic access trait, but the discovery adapter never calls it.
    }
}

async fn bind_root(
    endpoint: &PciDiscoveryFabricClient,
    index: u64,
    root: &PlatformPciRootSnapshot,
    start_bus: u32,
    end_bus: u32,
    attestation: OwnedHandle,
) -> Result<ProtocolClientEndpoint, String> {
    let mut resources = ResourceTransferBuilder::new();
    let attestation_slot = resources
        .push_move(attestation)
        .map_err(|error| format!("bind_root attestation resource: {error:?}"))?;
    let request = pci_discovery_fabric::bind_root_request {
        index,
        platform_root_id: root.root_id,
        firmware_version: root.firmware_version.as_str(),
        aml_namespace_generation: root.aml_namespace_generation,
        platform_topology_generation: root.platform_topology_generation,
        pci_fabric_generation: root.pci_fabric_generation,
        segment: root.segment,
        start_bus,
        end_bus,
        attestation: attestation_slot,
    };
    let mut request_wire = vec![
        0_u8;
        pci_discovery_fabric::BIND_ROOT_REQUEST_HEADER_BYTES
            + request.firmware_version.len()
    ];
    let mut invocation = pci_discovery_fabric::submit_bind_root(
        &endpoint.0,
        &request,
        resources,
        &mut request_wire,
        0,
    )
    .map_err(|error| format!("bind_root submit: {error:?}"))?;
    wait_for_call(invocation.get(), "bind_root").await?;
    let mut response_wire = [0_u8; 32];
    let (response, mut resources) =
        pci_discovery_fabric::take_bind_root(&mut invocation, &mut response_wire)
            .map_err(|error| format!("bind_root take: {error:?}"))?;
    let handle = resources
        .take(response.admin)
        .ok_or_else(|| "bind_root omitted PciFabricAdmin capability".to_string())?;
    log::info!(
        "PciFabricAdmin bound root_id={:016x} segment={} buses={}..{}",
        root.root_id,
        root.segment,
        start_bus,
        end_bus
    );
    // SAFETY: NaoIDL validated the returned resource's protocol scope and
    // adopted ownership; moving the raw handle into the generic client is the
    // documented ProtocolClientEndpoint conversion.
    Ok(unsafe { ProtocolClientEndpoint::from_raw(handle.into_raw()) })
}

async fn take_root_resource_attestation(
    endpoint: &PlatformPciTopologyClient,
    root: &PlatformPciRootSnapshot,
) -> Result<Option<OwnedHandle>, String> {
    let request = platform_pci_topology::take_root_resource_attestation_request {
        root_id: root.root_id,
        firmware_version: root.firmware_version.as_str(),
        platform_topology_generation: root.platform_topology_generation,
    };
    let mut request_wire = vec![
        0_u8;
        platform_pci_topology::TAKE_ROOT_RESOURCE_ATTESTATION_REQUEST_HEADER_BYTES
            + request.firmware_version.len()
    ];
    let mut invocation = platform_pci_topology::submit_take_root_resource_attestation(
        &endpoint.0,
        &request,
        ResourceTransferBuilder::new(),
        &mut request_wire,
        0,
    )
    .map_err(|error| format!("root resource attestation submit: {error:?}"))?;
    wait_for_call(invocation.get(), "root resource attestation").await?;
    let mut response_wire = [0_u8; 128];
    let (response, mut resources) = match platform_pci_topology::take_take_root_resource_attestation(
        &mut invocation,
        &mut response_wire,
    ) {
        Ok(result) => result,
        Err(CallError::Outcome {
            protocol_error: -2, ..
        }) => return Ok(None),
        Err(error) => {
            return Err(format!("root resource attestation take: {error:?}"));
        }
    };
    let handle = resources
        .take(response.attestation)
        .ok_or_else(|| "PlatformPciTopology omitted root resource attestation".to_string())?;
    Ok(Some(handle))
}

async fn take_device_resource_attestation(
    endpoint: &PlatformPciTopologyClient,
    root: &PlatformPciRootSnapshot,
    device_id: u64,
) -> Result<Option<OwnedHandle>, String> {
    let request = platform_pci_topology::take_device_resource_attestation_request {
        device_id,
        root_id: root.root_id,
        firmware_version: root.firmware_version.as_str(),
        platform_topology_generation: root.platform_topology_generation,
    };
    let mut request_wire = vec![
        0_u8;
        platform_pci_topology::TAKE_DEVICE_RESOURCE_ATTESTATION_REQUEST_HEADER_BYTES
            + request.firmware_version.len()
    ];
    let mut invocation = platform_pci_topology::submit_take_device_resource_attestation(
        &endpoint.0,
        &request,
        ResourceTransferBuilder::new(),
        &mut request_wire,
        0,
    )
    .map_err(|error| format!("device resource attestation submit: {error:?}"))?;
    wait_for_call(invocation.get(), "device resource attestation").await?;
    let mut response_wire = [0_u8; 128];
    let (response, mut resources) =
        match platform_pci_topology::take_take_device_resource_attestation(
            &mut invocation,
            &mut response_wire,
        ) {
            Ok(result) => result,
            Err(CallError::Outcome {
                protocol_error: -2, ..
            }) => return Ok(None),
            Err(error) => return Err(format!("device resource attestation take: {error:?}")),
        };
    let handle = resources
        .take(response.attestation)
        .ok_or_else(|| "PlatformPciTopology omitted device resource attestation".to_string())?;
    Ok(Some(handle))
}

async fn bind_device_resources(
    function: &ProtocolClientEndpoint,
    device_id: u64,
    attestation: OwnedHandle,
) -> Result<(), String> {
    let mut resources = ResourceTransferBuilder::new();
    let authority = resources
        .push_move(attestation)
        .map_err(|error| format!("bind_device_resources resource: {error:?}"))?;
    let request = pci_function::bind_device_resources_request {
        device_id,
        authority,
    };
    let mut request_wire = [0_u8; pci_function::BIND_DEVICE_RESOURCES_REQUEST_HEADER_BYTES];
    let mut invocation = pci_function::submit_bind_device_resources(
        function,
        &request,
        resources,
        &mut request_wire,
        0,
    )
    .map_err(|error| format!("bind_device_resources submit: {error:?}"))?;
    wait_for_call(invocation.get(), "bind_device_resources").await?;
    let mut response_wire = [0_u8; 32];
    pci_function::take_bind_device_resources(&mut invocation, &mut response_wire)
        .map_err(|error| format!("bind_device_resources rejected: {error:?}"))?;
    Ok(())
}

async fn open_function(
    endpoint: &ProtocolClientEndpoint,
    root: &PlatformPciRootSnapshot,
    address: PciAddress,
) -> Result<ProtocolClientEndpoint, String> {
    let request = pci_fabric_admin::open_function_request {
        bus: u32::from(address.bus()),
        device: u32::from(address.device()),
        function: u32::from(address.function()),
    };
    let mut request_wire = [0_u8; pci_fabric_admin::OPEN_FUNCTION_REQUEST_HEADER_BYTES];
    let mut invocation = pci_fabric_admin::submit_open_function(
        endpoint,
        &request,
        ResourceTransferBuilder::new(),
        &mut request_wire,
        0,
    )
    .map_err(|error| format!("open_function submit: {error:?}"))?;
    wait_for_call(invocation.get(), "open_function").await?;
    let mut response_wire = [0_u8; 96];
    let (response, mut resources) =
        pci_fabric_admin::take_open_function(&mut invocation, &mut response_wire)
            .map_err(|error| format!("open_function take: {error:?}"))?;
    if response.platform_root_id != root.root_id
        || response.segment != root.segment
        || response.bus != u32::from(address.bus())
        || response.device != u32::from(address.device())
        || response.function != u32::from(address.function())
        || response.firmware_version != root.firmware_version
        || response.aml_namespace_generation != root.aml_namespace_generation
        || response.platform_topology_generation != root.platform_topology_generation
        || response.pci_fabric_generation != root.pci_fabric_generation
    {
        return Err("kernel returned a PciFunction outside the bound root identity".into());
    }
    let handle = resources
        .take(response.capability)
        .ok_or_else(|| "open_function omitted PciFunction capability".to_string())?;
    // SAFETY: NaoIDL validated this kernel-issued resource against the
    // PciFunction protocol scope before returning it.
    Ok(unsafe { ProtocolClientEndpoint::from_raw(handle.into_raw()) })
}

async fn open_bound_function(
    topology_endpoint: &PlatformPciTopologyClient,
    root: &BoundPciRoot,
    function: &PciFunctionNode,
) -> Result<ProtocolClientEndpoint, String> {
    let capability = open_function(&root._admin, &root.platform_root, function.address).await?;
    if let Some(device_id) = function.platform_device_id {
        if let Some(attestation) =
            take_device_resource_attestation(topology_endpoint, &root.platform_root, device_id)
                .await?
        {
            if let Err(error) = bind_device_resources(&capability, device_id, attestation).await {
                log::warn!(
                    "PCI device resource attestation did not bind device_id={:016x} address={}: {error}",
                    device_id,
                    function.address
                );
            } else {
                log::info!(
                    "PCI device resource attestation bound address={} device_id={:016x}",
                    function.address,
                    device_id
                );
            }
        }
    }
    Ok(capability)
}

struct PreparedPciDriver {
    address: PciAddress,
    process: servicekit::boot::PreparedEarlyService,
    service_directory: OwnedHandle,
    manager_directory: OwnedHandle,
    wait_capability: OwnedHandle,
}

struct StartedPciDriver {
    address: PciAddress,
    pid: u64,
    process: ProtocolClientEndpoint,
    manager_directory: OwnedHandle,
}

struct PciDriverExitEvent {
    address: PciAddress,
    pid: u64,
    factory_id: Option<u64>,
    outcome: Result<i64, String>,
}

async fn grant_pci_driver(
    function: &PciFunctionNode,
    function_capability: &ProtocolClientEndpoint,
    driver: &DriverBinding,
    image: &[u8],
) -> Result<PreparedPciDriver, String> {
    grant_pci_driver_with_bar_mask(
        function,
        function_capability,
        driver,
        image,
        driver.profile_bar_mask,
    )
    .await
}

async fn grant_pci_driver_with_bar_mask(
    function: &PciFunctionNode,
    function_capability: &ProtocolClientEndpoint,
    driver: &DriverBinding,
    image: &[u8],
    bar_mask: u32,
) -> Result<PreparedPciDriver, String> {
    match driver.compatible_id {
        DriverCompatibleId::Text(identifier) => log::info!(
            "PCI driver match address={} compatible_id={identifier} image={}",
            function.address,
            driver.executable_path.trim_start_matches('/')
        ),
        DriverCompatibleId::Integer(identifier) => log::info!(
            "PCI driver match address={} compatible_id={identifier:#x} image={}",
            function.address,
            driver.executable_path.trim_start_matches('/')
        ),
        DriverCompatibleId::Pci {
            vendor_id,
            device_id,
        } => log::info!(
            "PCI driver match address={} vendor={vendor_id:04x} device={device_id:04x} image={}",
            function.address,
            driver.executable_path.trim_start_matches('/')
        ),
    }
    let prepared = servicekit::boot::prepare_early_service(image, driver.executable_path)
        .map_err(|error| format!("prepare PCI driver process: {error:?}"))?;
    let target = prepared
        .process_capability()
        .map_err(|error| format!("duplicate PCI driver process capability: {error:?}"))?;
    let mut resources = ResourceTransferBuilder::new();
    let process_slot = resources
        .push_move(target)
        .map_err(|error| format!("grant_driver process resource: {error:?}"))?;
    let request = pci_function::grant_driver_request {
        profile: pci_function::PciDriverFunctionProfile {
            rights: driver.profile_rights,
            bar_mask,
        },
        driver_process: process_slot,
    };
    let mut request_wire = [0_u8; pci_function::GRANT_DRIVER_REQUEST_HEADER_BYTES];
    let mut invocation = pci_function::submit_grant_driver(
        function_capability,
        &request,
        resources,
        &mut request_wire,
        0,
    )
    .map_err(|error| format!("PciFunction.grant_driver submit: {error:?}"))?;
    wait_for_call(invocation.get(), "PciFunction.grant_driver").await?;
    let mut response_wire = [0_u8; pci_function::GRANT_DRIVER_RESPONSE_HEADER_BYTES];
    let (response, mut resources) =
        pci_function::take_grant_driver(&mut invocation, &mut response_wire)
            .map_err(|error| format!("PciFunction.grant_driver take: {error:?}"))?;
    let service_directory = resources
        .take(response.service_directory)
        .ok_or_else(|| "grant_driver omitted the private ServiceDirectory".to_string())?;
    let manager_directory = service_directory
        .duplicate(0)
        .map_err(|error| format!("duplicate private driver ServiceDirectory: {error:?}"))?;
    log::info!("PCI driver PciFunction grant returned private ServiceDirectory");

    let wait_capability = prepared
        .process_capability()
        .map_err(|error| format!("duplicate granted driver wait capability: {error:?}"))?;

    Ok(PreparedPciDriver {
        address: function.address,
        process: prepared,
        service_directory,
        manager_directory,
        wait_capability,
    })
}

async fn start_pci_driver(
    context: &Context,
    prepared: PreparedPciDriver,
) -> Result<StartedPciDriver, String> {
    let PreparedPciDriver {
        address,
        process,
        service_directory,
        manager_directory,
        wait_capability,
    } = prepared;
    let pid = process
        .start_confirmed(context, service_directory)
        .await
        .map_err(|error| format!("start granted PCI driver {address}: {error:?}"))?;
    if let Err(error) = verify_process_start_is_idempotent(&wait_capability, address, pid).await {
        log::warn!(
            "PCI driver Process.start idempotence probe failed after confirmed start address={address} pid={pid}: {error}"
        );
    }
    let process = unsafe { ProtocolClientEndpoint::from_raw(wait_capability.into_raw()) };
    Ok(StartedPciDriver {
        address,
        pid,
        process,
        manager_directory,
    })
}

async fn start_virtio_block_drivers(
    context: &Context,
    topology_endpoint: &PlatformPciTopologyClient,
    tree: &PciFabricTree,
    exit_sender: &tokio::sync::mpsc::UnboundedSender<PciDriverExitEvent>,
) -> Vec<BlockFactoryProvider> {
    let functions: Vec<_> = tree
        .roots
        .iter()
        .flat_map(|root| root.functions.iter().map(move |function| (root, function)))
        .collect();
    let identifiers: Vec<_> = functions
        .iter()
        .map(|(_, function)| DriverFunctionIdentifiers {
            compatible_ids: function.compatible_identifiers.as_slice(),
            vendor_id: function.vendor_id,
            device_id: function.device_id,
        })
        .collect();
    let plan = match plan_driver_bindings_for_functions(&identifiers, &VIRTIO_BLK_DRIVER_CATALOG) {
        Ok(plan) => plan,
        Err(error) => {
            log::error!("Virtio block driver catalog preflight failed: {error:?}");
            return Vec::new();
        }
    };
    if plan.is_empty() {
        log::info!("Virtio block driver manager found no supported PCI function");
        return Vec::new();
    }
    let expected_prepared_count = plan.len();

    let mut prepared = Vec::new();
    for group in group_driver_plan_by_image(plan) {
        let image = match servicekit::boot::read_module(context, group.image_uri) {
            Ok(image) => image,
            Err(error) => {
                log::error!(
                    "read PCI driver image {} failed: {error:?}",
                    group.image_uri
                );
                continue;
            }
        };
        for assignment in group.assignments {
            let (root, function) = functions[assignment.function_index];
            let function_capability =
                match open_bound_function(topology_endpoint, root, function).await {
                    Ok(capability) => capability,
                    Err(error) => {
                        log::error!(
                            "open PCI function capability failed address={}: {error}",
                            function.address
                        );
                        continue;
                    }
                };
            let capabilities = match crate::virtio_pci::discover_function_capabilities(
                &function_capability,
            )
            .await
            {
                Ok(capabilities) => capabilities,
                Err(error) => {
                    log::warn!(
                        "skip Virtio block candidate address={} invalid PCI capabilities: {error:?}",
                        function.address
                    );
                    continue;
                }
            };
            match grant_pci_driver_with_bar_mask(
                function,
                &function_capability,
                assignment.driver,
                &image,
                capabilities.bar_mask(),
            )
            .await
            {
                Ok(driver) => prepared.push(driver),
                Err(error) => log::error!(
                    "Virtio block function grant failed address={}: {error}",
                    function.address
                ),
            }
        }
    }

    let prepared = match finish_driver_preparation(expected_prepared_count, prepared) {
        Ok(prepared) => prepared,
        Err(actual_count) => {
            log::error!(
                "Virtio block driver preflight incomplete expected={} prepared={}; no child started",
                expected_prepared_count,
                actual_count
            );
            return Vec::new();
        }
    };

    let mut started = Vec::with_capacity(prepared.len());
    for prepared in prepared {
        match start_pci_driver(context, prepared).await {
            Ok(driver) => started.push(driver),
            Err(error) => log::error!("Virtio block driver start failed: {error}"),
        }
    }

    let mut providers = Vec::new();
    let mut next_factory_id = 1_u64;
    for driver in started {
        let StartedPciDriver {
            address,
            pid,
            process,
            manager_directory,
        } = driver;
        let mut registered_factory_id = None;
        let endpoint = servicekit::connect_until(
            manager_directory.get(),
            "naos://service/pci/block-device-factory",
            naos_idl::block_device_factory::PROTOCOL_UUID,
            naos_idl::block_device_factory::PROTOCOL_RIGHTS,
            naos_idl::block_device_factory::PROTOCOL_REVISION,
            naos_idl::block_device_factory::PROTOCOL_FEATURES,
            core::time::Duration::from_secs(5),
        )
        .await;
        match endpoint {
            Ok(endpoint) => {
                let info = naos_idl::handle_info(endpoint.get());
                let valid = info.as_ref().is_ok_and(|info| {
                    info.binding == servicekit::sys::BINDING_CLIENT_END
                        && info.scope == naos_idl::block_device_factory::PROTOCOL_SCOPE
                        && info.protocol_uuid.bytes == naos_idl::block_device_factory::PROTOCOL_UUID
                        && info.revision == naos_idl::block_device_factory::PROTOCOL_REVISION
                        && info.features == naos_idl::block_device_factory::PROTOCOL_FEATURES
                        && info.protocol_rights & naos_idl::block_device_factory::PROTOCOL_RIGHTS
                            == naos_idl::block_device_factory::PROTOCOL_RIGHTS
                });
                drop(endpoint);
                if !valid {
                    match info {
                        Ok(_) => log::error!(
                            "Virtio block driver published a factory with invalid protocol metadata address={address}"
                        ),
                        Err(error) => log::error!(
                            "Virtio block factory metadata could not be read address={address}: {error:?}"
                        ),
                    }
                } else if let Some((_, function)) = functions
                    .iter()
                    .find(|(_, function)| function.address == address)
                {
                    let directory = manager_directory.duplicate(0);
                    let next = next_factory_id.checked_add(1);
                    match (directory, next) {
                        (Ok(directory), Some(next_id)) => {
                            let identity = pci_block_device_catalog::PciBlockFactoryIdentity {
                                factory_id: next_factory_id,
                                segment: u32::from(address.segment()),
                                bus: u32::from(address.bus()),
                                device: u32::from(address.device()),
                                function: u32::from(address.function()),
                                vendor_id: function.vendor_id,
                                pci_device_id: function.device_id,
                            };
                            providers.push(BlockFactoryProvider {
                                identity,
                                directory,
                            });
                            registered_factory_id = Some(identity.factory_id);
                            next_factory_id = next_id;
                            log::info!(
                                "Virtio block provider registered address={} factory_id={}",
                                address,
                                identity.factory_id
                            );
                        }
                        (Err(error), _) => log::error!(
                            "duplicate Virtio block provider directory address={address}: {error}"
                        ),
                        (_, None) => log::error!("Virtio block factory identity space exhausted"),
                    }
                } else {
                    log::error!("Virtio block driver BDF disappeared from its bound PCI snapshot");
                }
            }
            Err(error) => log::error!(
                "Virtio block driver did not publish its factory address={address}: {error:?}"
            ),
        }
        spawn_pci_driver_exit_monitor(
            address,
            pid,
            process,
            registered_factory_id,
            exit_sender.clone(),
        );
    }
    providers
}

fn spawn_pci_driver_exit_monitor(
    address: PciAddress,
    pid: u64,
    process: ProtocolClientEndpoint,
    factory_id: Option<u64>,
    exit_sender: tokio::sync::mpsc::UnboundedSender<PciDriverExitEvent>,
) {
    tokio::spawn(async move {
        let outcome = wait_for_pci_driver_exit(address, &process).await;
        if exit_sender
            .send(PciDriverExitEvent {
                address,
                pid,
                factory_id,
                outcome,
            })
            .is_err()
        {
            log::debug!("PCIED stopped before processing driver exit address={address} pid={pid}");
        }
    });
}

async fn wait_for_pci_driver_exit(
    address: PciAddress,
    process: &ProtocolClientEndpoint,
) -> Result<i64, String> {
    let request = process_idl::wait_request { flags: 0 };
    let mut request_wire = [0_u8; process_idl::WAIT_REQUEST_HEADER_BYTES];
    let mut invocation = process_idl::submit_wait(
        process,
        &request,
        ResourceTransferBuilder::new(),
        &mut request_wire,
        0,
    )
    .map_err(|error| format!("wait for PCI driver {address} submit: {error:?}"))?;
    wait_for_call(invocation.get(), "wait for PCI driver exit").await?;
    let mut response_wire = [0_u8; process_idl::WAIT_RESPONSE_HEADER_BYTES];
    let response = process_idl::take_wait(&mut invocation, &mut response_wire)
        .map_err(|error| format!("wait for PCI driver {address} take: {error:?}"))?;
    Ok(response.status)
}

async fn verify_process_start_is_idempotent(
    process: &OwnedHandle,
    address: PciAddress,
    pid: u64,
) -> Result<(), String> {
    let duplicate = process
        .duplicate(0)
        .map_err(|error| format!("duplicate Process.start verifier for {address}: {error}"))?;
    let endpoint = unsafe { ProtocolClientEndpoint::from_raw(duplicate.into_raw()) };
    let mut request_wire = [0u8; 64];
    let mut invocation = process_idl::submit_start(
        &endpoint,
        &process_idl::start_request {},
        ResourceTransferBuilder::new(),
        &mut request_wire,
        0,
    )
    .map_err(|error| format!("repeat Process.start submit for {address}: {error:?}"))?;
    wait_for_call(invocation.get(), "repeat Process.start").await?;
    let mut response_wire = [0u8; 64];
    process_idl::take_start(&mut invocation, &mut response_wire)
        .map_err(|error| format!("repeat Process.start take for {address}: {error:?}"))?;
    log::info!("PCI child Process.start retry confirmed idempotent address={address} pid={pid}");
    Ok(())
}

async fn scan_region(
    endpoint: &ProtocolClientEndpoint,
    root: &PlatformPciRootSnapshot,
    start_bus: u32,
    end_bus: u32,
    platform_device_paths: &[PlatformPciDeviceFunctionPath],
    function_count: &mut usize,
) -> Result<Vec<PciFunctionNode>, String> {
    if start_bus > end_bus || end_bus > u32::from(u8::MAX) {
        return Err("invalid ECAM bus intersection".into());
    }
    let mut request_wire = [];
    let mut invocation = pci_fabric_admin::submit_open_region(
        endpoint,
        &pci_fabric_admin::open_region_request {},
        ResourceTransferBuilder::new(),
        &mut request_wire,
        0,
    )
    .map_err(|error| format!("open_region submit: {error:?}"))?;
    wait_for_call(invocation.get(), "open_region").await?;
    let mut response_wire = [0_u8; 128];
    let (response, mut resources) =
        pci_fabric_admin::take_open_region(&mut invocation, &mut response_wire)
            .map_err(|error| format!("open_region take: {error:?}"))?;
    let expected_bytes = (u64::from(end_bus) - u64::from(start_bus) + 1) << 20;
    if response.segment != root.segment
        || response.start_bus != start_bus
        || response.end_bus != end_bus
        || response.bytes != expected_bytes
        || response.firmware_version != root.firmware_version
        || response.pci_fabric_generation != root.pci_fabric_generation
    {
        return Err("kernel returned an inconsistent bound ECAM region".into());
    }
    let handle = resources
        .take(response.ecam)
        .ok_or_else(|| "open_region omitted ECAM resource".to_string())?;
    let object = MemoryObject::from_owned(handle);
    let mapping = object
        .map_all(servicekit::sys::MEMORY_MAP_READ)
        .map_err(|error| format!("map ECAM: {error:?}"))?;
    let start_bus = u8::try_from(response.start_bus)
        .map_err(|_| "ECAM region start bus does not fit in a PCI bus number")?;
    let end_bus = u8::try_from(response.end_bus)
        .map_err(|_| "ECAM region end bus does not fit in a PCI bus number")?;
    let segment = u16::try_from(response.segment)
        .map_err(|_| "ECAM segment does not fit in a PCI segment number")?;
    let access = EcamAccess {
        bytes: mapping.as_slice(),
        segment,
        start_bus,
        end_bus,
    };
    let mut functions = Vec::new();
    let mut bridge_ranges = Vec::new();
    for bus in start_bus..=end_bus {
        for device in 0..32_u8 {
            for function in 0..8_u8 {
                let address = PciAddress::new(segment, bus, device, function);
                let header = PciHeader::new(address);
                let (vendor, device_id) = header.id(&access);
                if vendor == 0xffff {
                    if function == 0 {
                        break;
                    }
                    continue;
                }
                if !crate::topology::admit_pci_function(function_count) {
                    return Err(format!(
                        "PCI function inventory exceeds the fixed fabric limit of {}",
                        crate::topology::MAX_PCI_FUNCTIONS_PER_FABRIC
                    ));
                }
                let header_type = header.header_type(&access);
                let bridge_range = match header_type {
                    HeaderType::PciPciBridge => {
                        let bridge =
                            PciPciBridgeHeader::from_header(PciHeader::new(address), &access)
                                .ok_or_else(|| {
                                    "PCI bridge header changed during read-only scan".to_string()
                                })?;
                        Some(BridgeBusRange {
                            address,
                            primary: bridge.primary_bus_number(&access),
                            secondary: bridge.secondary_bus_number(&access),
                            subordinate: bridge.subordinate_bus_number(&access),
                        })
                    }
                    HeaderType::CardBusBridge => {
                        let bus_numbers = unsafe { access.read(address, 0x18) };
                        Some(BridgeBusRange {
                            address,
                            primary: bus_numbers as u8,
                            secondary: (bus_numbers >> 8) as u8,
                            subordinate: (bus_numbers >> 16) as u8,
                        })
                    }
                    _ => None,
                };
                if let Some(bridge_range) = bridge_range {
                    bridge_ranges.push(bridge_range);
                }
                functions.push(PciFunctionNode {
                    address,
                    vendor_id: vendor,
                    device_id,
                    header_type,
                    parent: None,
                    children: Vec::new(),
                    platform_device_id: None,
                    compatible_identifiers: Vec::new(),
                });
                log::info!(
                    "pci found address={} vendor={:04x} device={:04x} header={:?}",
                    address,
                    vendor,
                    device_id,
                    header_type
                );
                if function == 0 && !header.has_multiple_functions(&access) {
                    break;
                }
            }
        }
    }

    let addresses: Vec<_> = functions.iter().map(|function| function.address).collect();
    let parents = assign_bridge_parents(start_bus, end_bus, &addresses, &bridge_ranges)
        .map_err(|error| format!("PCI bridge hierarchy validation failed: {error:?}"))?;
    let root_start_bus = u8::try_from(root.start_bus)
        .map_err(|_| "PlatformPciTopology root start bus does not fit in a PCI bus number")?;
    let platform_device_ids = associate_platform_device_paths(
        root.root_id,
        root_start_bus,
        &addresses,
        &parents,
        platform_device_paths,
    )
    .map_err(|error| format!("ACPI _ADR to PCI function association failed: {error:?}"))?;
    for (index, (parent, platform_device_id)) in
        parents.into_iter().zip(platform_device_ids).enumerate()
    {
        functions[index].platform_device_id = platform_device_id;
        if let Some(device_id) = platform_device_id {
            let platform_device = platform_device_paths
                .iter()
                .find(|platform_device| platform_device.device_id == device_id)
                .ok_or_else(|| {
                    "associated platform device omitted its compatible identifiers".to_string()
                })?;
            functions[index].compatible_identifiers =
                platform_device.compatible_identifiers.clone();
        }
        let Some(parent) = parent else {
            continue;
        };
        let parent_index = functions
            .iter()
            .position(|function| function.address == parent)
            .ok_or_else(|| "PCI bridge parent disappeared from scan result".to_string())?;
        let address = functions[index].address;
        functions[index].parent = Some(parent);
        functions[parent_index].children.push(address);
    }

    log::info!(
        "pci function inventory built root_id={:016x} functions={}",
        root.root_id,
        functions.len()
    );
    Ok(functions)
}

async fn connect_platform_topology(
    service_directory: naos_sys::Handle,
) -> Result<PlatformPciTopologyClient, String> {
    log::info!("waiting for PlatformPciTopology listener");
    let descriptor = platform_pci_topology::protocol_descriptor();
    let endpoint = servicekit::connect_until(
        service_directory,
        PLATFORM_PCI_TOPOLOGY_URI,
        descriptor.uuid.bytes,
        descriptor.protocol_rights,
        descriptor.revision,
        descriptor.features,
        core::time::Duration::from_secs(30),
    )
    .await
    .map_err(|error| format!("PlatformPciTopology connect: {error:?}"))?;
    log::info!("connected to PlatformPciTopology listener");
    Ok(PlatformPciTopologyClient(endpoint))
}

async fn acquire_platform_generation(
    endpoint: &PlatformPciTopologyClient,
) -> Result<PlatformGenerationSnapshot, String> {
    let mut generation_invocation = platform_pci_topology::submit_generation(
        &endpoint.0,
        &platform_pci_topology::generation_request {},
        ResourceTransferBuilder::new(),
        &mut [0_u8; platform_pci_topology::GENERATION_REQUEST_HEADER_BYTES],
        0,
    )
    .map_err(|error| format!("PlatformPciTopology generation submit: {error:?}"))?;
    wait_for_call(
        generation_invocation.get(),
        "PlatformPciTopology generation",
    )
    .await?;
    let mut generation_wire = [0_u8; 128];
    let generation =
        platform_pci_topology::take_generation(&mut generation_invocation, &mut generation_wire)
            .map_err(|error| format!("PlatformPciTopology generation take: {error:?}"))?;
    if generation.firmware_version.is_empty()
        || generation.value.aml_namespace_generation == 0
        || generation.value.platform_topology_generation == 0
        || generation.value.pci_fabric_generation == 0
        || generation.value.compatibility_profile_hash == 0
    {
        return Err("PlatformPciTopology returned invalid generation identity".into());
    }
    Ok(PlatformGenerationSnapshot {
        firmware_version: generation.firmware_version.to_owned(),
        value: generation.value,
    })
}

async fn acquire_platform_roots(
    endpoint: &PlatformPciTopologyClient,
    generation: &PlatformGenerationSnapshot,
) -> Result<
    (
        Vec<PlatformPciRootSnapshot>,
        Vec<platform_pci_topology::PlatformPciResourceWindow>,
    ),
    String,
> {
    let request = platform_pci_topology::get_pci_roots_request {
        firmware_version: generation.firmware_version.as_str(),
        platform_topology_generation: generation.platform_topology_generation,
    };
    let mut request_wire = vec![
        0_u8;
        platform_pci_topology::GET_PCI_ROOTS_REQUEST_HEADER_BYTES
            + request.firmware_version.len()
    ];
    let mut invocation = platform_pci_topology::submit_get_pci_roots(
        &endpoint.0,
        &request,
        ResourceTransferBuilder::new(),
        &mut request_wire,
        0,
    )
    .map_err(|error| format!("PlatformPciTopology roots submit: {error:?}"))?;
    wait_for_call(invocation.get(), "PlatformPciTopology roots").await?;
    let mut response_wire = [0_u8; 65_536];
    let response = platform_pci_topology::take_get_pci_roots(&mut invocation, &mut response_wire)
        .map_err(|error| format!("PlatformPciTopology roots take: {error:?}"))?;
    if response.firmware_version != generation.firmware_version {
        return Err("PlatformPciTopology returned a stale firmware version".into());
    }
    let firmware_version = response.firmware_version.to_owned();
    let roots: Vec<_> = response
        .roots
        .into_iter()
        .map(|value| PlatformPciRootSnapshot {
            firmware_version: firmware_version.clone(),
            value,
        })
        .collect();
    let resource_windows = response.resource_windows;
    if roots.iter().any(|root| {
        root.segment > u32::from(u16::MAX)
            || root.start_bus > root.end_bus
            || root.end_bus > u32::from(u8::MAX)
            || root.aml_namespace_generation != generation.aml_namespace_generation
            || root.platform_topology_generation != generation.platform_topology_generation
            || root.pci_fabric_generation != generation.pci_fabric_generation
            || root.compatibility_profile_hash != generation.compatibility_profile_hash
    }) || {
        let root_values: Vec<_> = roots.iter().map(|root| root.value.clone()).collect();
        !validate_pci_root_window_table(
            &root_values,
            &resource_windows,
            generation.compatibility_profile_hash,
        )
    } {
        return Err("PlatformPciTopology returned an invalid or stale PCI root".into());
    }
    Ok((roots, resource_windows))
}

async fn acquire_platform_device_paths(
    endpoint: &PlatformPciTopologyClient,
    generation: &PlatformGenerationSnapshot,
    roots: &[PlatformPciRootSnapshot],
) -> Result<Vec<PlatformPciDeviceFunctionPath>, String> {
    let root_ids: BTreeSet<_> = roots.iter().map(|root| root.root_id).collect();
    if root_ids.len() != roots.len() || root_ids.contains(&0) {
        return Err("PlatformPciTopology returned duplicate or zero PCI root identities".into());
    }

    let mut cursor = 0_u64;
    let mut seen_device_ids = BTreeSet::new();
    let mut paths = Vec::new();
    loop {
        let request = platform_pci_topology::list_devices_request {
            firmware_version: generation.firmware_version.as_str(),
            platform_topology_generation: generation.platform_topology_generation,
            cursor,
            limit: MAX_PLATFORM_DEVICE_PAGE as u32,
            filter_flags: PLATFORM_DEVICE_FILTER_PRESENT_AND_FUNCTIONAL,
        };
        let mut request_wire = vec![
            0_u8;
            platform_pci_topology::LIST_DEVICES_REQUEST_HEADER_BYTES
                + request.firmware_version.len()
        ];
        let mut invocation = platform_pci_topology::submit_list_devices(
            &endpoint.0,
            &request,
            ResourceTransferBuilder::new(),
            &mut request_wire,
            0,
        )
        .map_err(|error| format!("PlatformPciTopology list_devices submit: {error:?}"))?;
        wait_for_call(invocation.get(), "PlatformPciTopology list_devices").await?;
        let mut response_wire = [0_u8; 65_536];
        let response =
            platform_pci_topology::take_list_devices(&mut invocation, &mut response_wire)
                .map_err(|error| format!("PlatformPciTopology list_devices take: {error:?}"))?;
        let returned_device_count = response.devices.len();
        append_platform_device_paths(
            &response,
            &generation,
            &root_ids,
            &mut seen_device_ids,
            &mut paths,
        )
        .map_err(|error| {
            format!("PlatformPciTopology returned invalid PCI device paths: {error:?}")
        })?;
        if seen_device_ids.len() > MAX_PLATFORM_DEVICE_SNAPSHOTS {
            return Err("PlatformPciTopology exceeded the ACPI device snapshot limit".into());
        }

        if !valid_device_page_cursor(cursor, response.next_cursor, returned_device_count) {
            return Err(
                "PlatformPciTopology returned a non-progressing or invalid device cursor".into(),
            );
        }
        if response.next_cursor == 0 {
            break;
        }
        cursor = response.next_cursor;
    }
    log::info!(
        "PlatformPciTopology ACPI PCI device paths snapshots={} associated={}",
        seen_device_ids.len(),
        paths.len()
    );
    Ok(paths)
}

async fn negotiate_read_only_root_features(
    endpoint: &PlatformPciTopologyClient,
    root: &PlatformPciRootSnapshot,
) -> Result<Option<platform_pci_topology::PciRootFeatureSet>, String> {
    let request = platform_pci_topology::negotiate_pci_features_request {
        root_id: root.root_id,
        firmware_version: root.firmware_version.as_str(),
        platform_topology_generation: root.platform_topology_generation,
        requested_support: crate::topology::IMPLEMENTED_PCI_ROOT_SUPPORT_MASK,
        requested_control: 0,
    };
    let mut request_wire = vec![
        0_u8;
        platform_pci_topology::NEGOTIATE_PCI_FEATURES_REQUEST_HEADER_BYTES
            + request.firmware_version.len()
    ];
    let mut invocation = platform_pci_topology::submit_negotiate_pci_features(
        &endpoint.0,
        &request,
        ResourceTransferBuilder::new(),
        &mut request_wire,
        0,
    )
    .map_err(|error| format!("negotiate_pci_features submit: {error:?}"))?;
    wait_for_call(
        invocation.get(),
        "PlatformPciTopology negotiate_pci_features",
    )
    .await?;
    let mut response_wire = [0_u8; 256];
    let response = match platform_pci_topology::take_negotiate_pci_features(
        &mut invocation,
        &mut response_wire,
    ) {
        Ok(response) => response,
        Err(CallError::Outcome {
            protocol_error: -2, ..
        }) => return Ok(None),
        Err(error) => {
            return Err(format!(
                "negotiate_pci_features take failed requested_support={:08x} requested_control=00000000: {error:?}",
                crate::topology::IMPLEMENTED_PCI_ROOT_SUPPORT_MASK
            ));
        }
    };
    if !crate::topology::validate_pci_root_feature_set(
        root,
        &response.value,
        response.firmware_version,
        &root.firmware_version,
    ) {
        if crate::topology::pci_root_feature_set_is_bound_and_read_only(
            root,
            &response.value,
            response.firmware_version,
            &root.firmware_version,
        ) {
            return Ok(None);
        }
        return Err(format!(
            "PlatformPciTopology returned invalid or unauthorized feature evidence root_id={:016x}",
            root.root_id
        ));
    }
    Ok(Some(response.value))
}

async fn discover_tree(
    topology_endpoint: &PlatformPciTopologyClient,
    discovery_endpoint: &PciDiscoveryFabricClient,
    platform_generation: PlatformGenerationSnapshot,
    platform_roots: &[PlatformPciRootSnapshot],
    platform_resource_windows: &[platform_pci_topology::PlatformPciResourceWindow],
) -> Result<(PciFabricTree, usize), String> {
    let platform_device_paths =
        acquire_platform_device_paths(topology_endpoint, &platform_generation, platform_roots)
            .await?;
    for root in platform_roots {
        let feature = negotiate_read_only_root_features(topology_endpoint, root).await?;
        if let Some(feature) = feature {
            log::info!(
                "PCI root _OSC cached root_id={:016x} requested_support={:08x} query_support={:08x} commit_support={:08x} granted_control={:08x}",
                feature.root_id,
                feature.requested_support,
                feature.query_support,
                feature.commit_support,
                feature.granted_control
            );
        } else if platform_generation.health & crate::topology::HEALTH_DEGRADED != 0 {
            log::warn!(
                "PCI root _OSC result unavailable root_id={:016x}; continuing read-only discovery with no native control authority",
                root.root_id
            );
        } else {
            return Err(format!(
                "healthy PlatformPciTopology omitted PCI root _OSC result root_id={:016x}",
                root.root_id
            ));
        }
    }

    let request = pci_discovery_fabric::list_regions_request {};
    let mut request_wire = [0_u8; pci_discovery_fabric::LIST_REGIONS_REQUEST_HEADER_BYTES];
    let mut invocation = pci_discovery_fabric::submit_list_regions(
        &discovery_endpoint.0,
        &request,
        ResourceTransferBuilder::new(),
        &mut request_wire,
        0,
    )
    .map_err(|error| format!("list_regions submit failed: {error:?}"))?;
    wait_for_call(invocation.get(), "PciDiscoveryFabric list_regions").await?;
    let mut response_wire = [0_u8; 4096];
    let response = pci_discovery_fabric::take_list_regions(&mut invocation, &mut response_wire)
        .map_err(|error| format!("list_regions failed: {error:?}"))?;
    let region_count = response.regions.len();
    if response.firmware_version != platform_generation.firmware_version
        || response
            .regions
            .iter()
            .any(|region| region.pci_fabric_generation != platform_generation.pci_fabric_generation)
    {
        return Err("PciDiscoveryFabric returned inconsistent generation identity".into());
    }

    let mut roots = Vec::new();
    let mut function_count = 0;
    for (index, region) in response.regions.iter().enumerate() {
        if region.pci_fabric_generation != platform_generation.pci_fabric_generation {
            return Err("PciDiscoveryFabric returned a stale ECAM generation".into());
        }
        let mut bound_root = false;
        for root in platform_roots {
            if root.segment != region.segment {
                continue;
            }
            let Some((start_bus, end_bus)) = intersect_bus_window(
                region.start_bus,
                region.end_bus,
                root.start_bus,
                root.end_bus,
            ) else {
                continue;
            };
            if root.firmware_version != platform_generation.firmware_version
                || root.aml_namespace_generation != platform_generation.aml_namespace_generation
                || root.platform_topology_generation
                    != platform_generation.platform_topology_generation
                || root.pci_fabric_generation != platform_generation.pci_fabric_generation
                || root.compatibility_profile_hash != platform_generation.compatibility_profile_hash
            {
                return Err("PlatformPciTopology root identity changed after validation".into());
            }
            let Some(attestation) = take_root_resource_attestation(topology_endpoint, root).await?
            else {
                log::warn!(
                    "PCI root skipped because platformd supplied no kernel root-resource attestation root_id={:016x}",
                    root.root_id
                );
                continue;
            };
            bound_root = true;
            let admin = bind_root(
                discovery_endpoint,
                index as u64,
                root,
                start_bus,
                end_bus,
                attestation,
            )
            .await?;
            let functions = scan_region(
                &admin,
                root,
                start_bus,
                end_bus,
                &platform_device_paths,
                &mut function_count,
            )
            .await?;
            let window_start = root.resource_window_start as usize;
            let window_end = window_start + root.resource_window_count as usize;
            roots.push(BoundPciRoot {
                root_id: root.root_id,
                device_id: root.device_id,
                segment: root.segment,
                start_bus,
                end_bus,
                firmware_version: root.firmware_version.clone(),
                aml_namespace_generation: root.aml_namespace_generation,
                platform_topology_generation: root.platform_topology_generation,
                pci_fabric_generation: root.pci_fabric_generation,
                platform_root: root.clone(),
                resource_windows: platform_resource_windows[window_start..window_end].to_vec(),
                _admin: admin,
                functions,
            });
        }
        if !bound_root {
            log::warn!(
                "PCI discovery region {} has no PlatformPciTopology root intersection; skipping",
                index
            );
        }
    }
    Ok((
        PciFabricTree {
            generation: platform_generation,
            roots,
        },
        region_count,
    ))
}

pub async fn run(context: Context) -> i64 {
    #[cfg(feature = "pci-msi-route-smoke")]
    {
        let scoped_view = match servicekit::create_service_directory_view(
            context.service_directory_handle(),
            PCI_DISCOVERY_URI,
        ) {
            Ok(view) => view,
            Err(error) => {
                log::error!("create scoped PCI ServiceDirectory view failed: {error:?}");
                return 1;
            }
        };
        match servicekit::resolve_resource(scoped_view.get(), PCI_DISCOVERY_URI) {
            Ok(resource) => drop(resource),
            Err(error) => {
                log::error!("scoped PCI ServiceDirectory resolve failed: {error:?}");
                return 1;
            }
        }
        log::info!("scoped PCI ServiceDirectory view resolved PciDiscoveryFabric");

        match servicekit::create_service_directory_view(
            scoped_view.get(),
            PLATFORM_PCI_TOPOLOGY_URI,
        ) {
            Err(CallError::Status(status)) if status == servicekit::sys::STATUS_ACCESS_DENIED => {}
            Err(CallError::Outcome {
                protocol_error: -13 | 13,
                ..
            }) => {}
            Ok(widened_view) => {
                drop(widened_view);
                log::error!("single-URI PCI ServiceDirectory view allowed a nested view");
                return 1;
            }
            Err(error) => {
                log::error!(
                    "nested PCI ServiceDirectory view returned unexpected error: {error:?}"
                );
                return 1;
            }
        }
        log::info!("single-URI PCI ServiceDirectory view rejected nested view creation");

        match servicekit::resolve_resource(scoped_view.get(), PLATFORM_PCI_TOPOLOGY_URI) {
            Err(CallError::Outcome {
                protocol_error: -13 | 13,
                ..
            }) => {}
            Ok(resource) => {
                drop(resource);
                log::error!("single-URI PCI ServiceDirectory view resolved an unrelated locator");
                return 1;
            }
            Err(error) => {
                log::error!(
                    "unrelated PCI ServiceDirectory resolve returned unexpected error: {error:?}"
                );
                return 1;
            }
        }
        log::info!("single-URI PCI ServiceDirectory view denied unrelated locator");
    }

    let resource =
        match servicekit::resolve_resource(context.service_directory_handle(), PCI_DISCOVERY_URI) {
            Ok(resource) => resource,
            Err(error) => {
                log::error!("PciDiscoveryFabric resolve failed: {error:?}");
                return 1;
            }
        };
    let endpoint =
        PciDiscoveryFabricClient(unsafe { ProtocolClientEndpoint::from_raw(resource.into_raw()) });
    let topology_endpoint = match connect_platform_topology(context.service_directory_handle())
        .await
    {
        Ok(endpoint) => endpoint,
        Err(error) => {
            log::warn!("PlatformPciTopology unavailable; PCI discovery remains unbound: {error}");
            return 1;
        }
    };
    let topology_handles = [topology_endpoint.0.get()];
    let mut topology_readiness = servicekit::ReadinessSet::new();
    if let Err(status) = topology_readiness.refresh(&topology_handles) {
        log::error!("PlatformPciTopology lifecycle watch setup failed: {status}");
        return 1;
    }

    let platform_generation = match acquire_platform_generation(&topology_endpoint).await {
        Ok(generation) => generation,
        Err(error) => {
            log::error!("PCI discovery preflight failed: {error}");
            return 1;
        }
    };
    if !health_allows_pci_discovery(platform_generation.health) {
        log::warn!(
            "PlatformPciTopology health={:#x} forbids PCI discovery; keeping an empty block catalog online",
            platform_generation.health
        );
        let mut catalog = match BlockFactoryCatalog::new(
            platform_generation.firmware_version.clone(),
            platform_generation.pci_fabric_generation,
            Vec::new(),
        ) {
            Ok(catalog) => catalog,
            Err(error) => {
                log::error!("degraded PCI block catalog preflight failed: {error}");
                return 1;
            }
        };
        let mut server = match BlockFactoryCatalog::publish(context).await {
            Ok(server) => server,
            Err(error) => {
                log::error!("degraded PCI block catalog publish failed: {error:?}");
                return 1;
            }
        };
        log::info!(
            "empty PCI block catalog online firmware_version={} pci_fabric_generation={:016x}",
            platform_generation.firmware_version,
            platform_generation.pci_fabric_generation
        );
        loop {
            tokio::select! {
                request = server.next() => match request {
                    Ok(request) => catalog.handle(&mut server, request).await,
                    Err(ServerError::PeerClosed | ServerError::Protocol) => continue,
                    Err(error) => {
                        log::error!("degraded PCI block catalog server failed: {error:?}");
                        return 1;
                    }
                },
                result = topology_readiness.wait(None) => {
                    log::error!("PlatformPciTopology lifecycle ended while PCI was degraded: {result:?}");
                    return 1;
                }
            }
        }
    }

    let discovery = async {
        let (platform_roots, platform_resource_windows) =
            acquire_platform_roots(&topology_endpoint, &platform_generation).await?;
        discover_tree(
            &topology_endpoint,
            &endpoint,
            platform_generation,
            &platform_roots,
            &platform_resource_windows,
        )
        .await
    };
    let topology_closed = async {
        match topology_readiness.wait(None).await {
            Ok(event) => log::error!(
                "PlatformPciTopology closed during discovery read_closed={} write_closed={} error={}",
                event.read_closed,
                event.write_closed,
                event.error
            ),
            Err(error) => log::error!("PlatformPciTopology lifecycle wait failed: {error}"),
        }
    };
    let (tree, region_count) = match race_topology_lifecycle(discovery, topology_closed).await {
        Err(()) => return 1,
        Ok(Err(error)) => {
            log::error!("PCI discovery failed: {error}");
            return 1;
        }
        Ok(Ok(value)) => value,
    };
    #[cfg(feature = "pci-msi-route-smoke")]
    if let Err(error) = smoke::exercise_qemu_edu_route(&context, &topology_endpoint, &tree).await {
        log::error!("QEMU EDU MSI route smoke failed: {error}");
        return 1;
    }
    #[cfg(feature = "pci-msi-route-smoke")]
    if let Err(error) = smoke::exercise_qemu_rocker_msix_route(&topology_endpoint, &tree).await {
        log::error!("QEMU Rocker MSI-X route smoke failed: {error}");
        return 1;
    }
    let devices = tree
        .roots
        .iter()
        .map(|root| root.functions.len() as u64)
        .sum::<u64>();
    let associated_functions = tree
        .roots
        .iter()
        .flat_map(|root| &root.functions)
        .filter(|function| function.platform_device_id.is_some())
        .count();
    log::info!("pci ACPI function associations matched={associated_functions}/{devices}");
    for root in &tree.roots {
        log::info!(
            "pci root bound id={:016x} device_id={:016x} segment={} bus={}..{} functions={} resource_windows={} firmware_version={} aml_namespace_generation={:016x} platform_topology_generation={:016x} pci_fabric_generation={:016x}",
            root.root_id,
            root.device_id,
            root.segment,
            root.start_bus,
            root.end_bus,
            root.functions.len(),
            root.resource_windows.len(),
            root.firmware_version,
            root.aml_namespace_generation,
            root.platform_topology_generation,
            root.pci_fabric_generation
        );
        for function in &root.functions {
            log::debug!(
                "pci tree function address={} parent={:?} children={:?} vendor={:04x} device={:04x} header={:?} platform_device_id={:?} compatible_identifiers={:?}",
                function.address,
                function.parent,
                function.children,
                function.vendor_id,
                function.device_id,
                function.header_type,
                function.platform_device_id,
                function.compatible_identifiers
            );
        }
    }
    log::info!(
        "ready regions={} roots={} devices={} read_only=true platform_topology_generation={:016x}",
        region_count,
        tree.roots.len(),
        devices,
        tree.generation.platform_topology_generation
    );
    log::info!("PlatformPciTopology lifecycle watch active");
    let (driver_exit_sender, mut driver_exit_receiver) = tokio::sync::mpsc::unbounded_channel();
    let block_providers =
        start_virtio_block_drivers(&context, &topology_endpoint, &tree, &driver_exit_sender).await;
    let mut block_catalog = match BlockFactoryCatalog::new(
        tree.generation.firmware_version.clone(),
        tree.generation.pci_fabric_generation,
        block_providers,
    ) {
        Ok(catalog) => catalog,
        Err(error) => {
            log::error!("PCI block catalog preflight failed: {error}");
            return 1;
        }
    };
    let mut block_catalog_server = match BlockFactoryCatalog::publish(context).await {
        Ok(server) => server,
        Err(error) => {
            log::error!("PCI block catalog publish failed: {error:?}");
            return 1;
        }
    };
    log::info!(
        "PCI block catalog online entries={} firmware_version={} pci_fabric_generation={:016x}",
        block_catalog.entry_count(),
        tree.generation.firmware_version,
        tree.generation.pci_fabric_generation
    );
    loop {
        tokio::select! {
            request = block_catalog_server.next() => match request {
                Ok(request) => block_catalog.handle(&mut block_catalog_server, request).await,
                Err(ServerError::PeerClosed | ServerError::Protocol) => continue,
                Err(error) => {
                    log::error!("PCI block catalog server failed: {error:?}");
                    return 1;
                }
            },
            event = driver_exit_receiver.recv() => match event {
                Some(event) => {
                    match event.outcome {
                        Ok(0) => log::warn!(
                            "PCI driver exited normally while its function remained bound address={} pid={}",
                            event.address,
                            event.pid
                        ),
                        Ok(status) => log::error!(
                            "PCI driver exited with status={} address={} pid={}",
                            status,
                            event.address,
                            event.pid
                        ),
                        Err(error) => log::error!(
                            "PCI driver exit observation failed address={} pid={}: {}",
                            event.address,
                            event.pid,
                            error
                        ),
                    }
                    if let Some(factory_id) = event.factory_id {
                        if block_catalog.remove_factory(factory_id) {
                            log::warn!(
                                "removed PCI block provider after driver exit address={} factory_id={}",
                                event.address,
                                factory_id
                            );
                        }
                    }
                }
                None => {
                    log::error!("PCI driver exit monitor channel unexpectedly closed");
                    return 1;
                }
            },
            result = topology_readiness.wait(None) => match result {
                Ok(event) => {
                    log::error!(
                        "PlatformPciTopology readiness ended; dropping bound PCI capabilities read_closed={} write_closed={} error={}",
                        event.read_closed,
                        event.write_closed,
                        event.error
                    );
                    // Returning drops the process-local PciFabricAdmin and
                    // PciFunction capabilities. A restarted platformd requires a
                    // fresh pcied process and cannot reuse this topology lineage.
                    return 1;
                }
                Err(error) => {
                    log::error!("PlatformPciTopology lifecycle watch failed: {error}");
                    return 1;
                }
            }
        }
    }
}
