use crate::catalog;
use crate::executor_worker::ExecutorWorker;
use crate::platform_topology::{self, TopologyState};
use crate::power_transition::{
    CommitError, TransitionBackend, TransitionFailure, TransitionKind, TransitionOutcome,
    TransitionRequest, TransitionTicket, perform_transition,
};
use crate::region;
use crate::sci_wait::{
    GpeBank, GpeEoiTiming, GpeEventIter, PM1_GLOBAL_LOCK_STATUS_MASK, PM1_TIMER_STATUS_MASK,
    SciGenerationLineage, SciWaitAction, SciWaitOutcome, classify_sci_wait, gpe_eoi_timing,
    pm1_has_unhandled_status, pm1_safe_ack_status,
};
use crate::topology_health::{PlatformHealthInputs, compute_platform_health};
use naos_idl::acpi_fixed_hardware;
use naos_idl::firmware_root;
use naos_idl::platform_pci_topology;
use naos_idl::transport::{RpcMethodReply, RpcOutcome};
use naos_idl::{OwnedHandle, ProtocolClientEndpoint, ResourceSlot, ResourceTransferBuilder};
use rust_acpi_naos::GpeDispatchResult;
use servicekit::Context;
use servicekit::memory::MemoryObject;
use servicekit::server::{EndpointResource, Request, ServeHandler, Server};
use std::collections::BTreeMap;
use std::ops::Deref;
use std::sync::Arc;
use std::time::Duration;

const FIRMWARE_ROOT_URI: &str = "naos://kernel/firmware/root";
const ACPI_FIXED_HARDWARE_URI: &str = "naos://kernel/hw/acpi-fixed";
const PLATFORM_PCI_TOPOLOGY_URI: &str = "naos://system/platform/pci-topology";
const PLATFORM_POWER_CONTROL_URI: &str = "naos://system/platform/power/control";
const MAX_GPE_EVENTS_PER_SCI: usize = 128;
const SCI_WORK_UNIT_TIMEOUT: Duration = Duration::from_secs(10);

#[derive(Clone)]
struct FixedHardwareSnapshot {
    firmware_version: String,
    value: acpi_fixed_hardware::AcpiFixedHardwareInfo,
}

impl Deref for FixedHardwareSnapshot {
    type Target = acpi_fixed_hardware::AcpiFixedHardwareInfo;

    fn deref(&self) -> &Self::Target {
        &self.value
    }
}

fn validate_power_control_request(
    request: &naos_idl::transport::RpcRequestOwned,
) -> Result<(), i64> {
    naos_idl::platform_power_control::validate_request_transport_resources(request)?;
    if request.bulk.is_empty() && request.target.is_none() {
        Ok(())
    } else {
        Err(-22)
    }
}

async fn serve_unavailable_topology(
    context: &Context,
    generation: platform_topology::PlatformGenerationSnapshot,
    reason: String,
) -> i64 {
    let state = Arc::new(tokio::sync::RwLock::new(TopologyState {
        generation,
        roots: Vec::new(),
        resource_windows: Vec::new(),
        features: Vec::new(),
        devices: Arc::new(Vec::new()),
        pci_device_paths: Vec::new(),
    }));
    let mut server = match Server::publish(
        &context.service_directory(),
        PLATFORM_PCI_TOPOLOGY_URI,
        &platform_pci_topology::protocol_descriptor(),
        platform_pci_topology::validate_request_transport_resources,
    )
    .await
    {
        Ok(server) => server,
        Err(error) => {
            log::error!(
                "unavailable PlatformPciTopology publish failed: {error:?}; cause={reason}"
            );
            return 1;
        }
    };
    tokio::spawn(async move {
        let mut handler = TopologyHandler {
            state,
            firmware_version: String::new(),
            root_resource_attestations: Arc::new(tokio::sync::Mutex::new(BTreeMap::new())),
            device_resource_attestations: Arc::new(tokio::sync::Mutex::new(BTreeMap::new())),
        };
        let status = server.serve(&mut handler).await;
        log::error!("unavailable PlatformPciTopology server stopped status={status}");
    });
    log::error!("PlatformPciTopology published fail-closed health state: {reason}");
    loop {
        tokio::time::sleep(core::time::Duration::from_secs(3600)).await;
    }
}

async fn serve_firmware_unavailable(
    context: &Context,
    identity: rust_acpi_naos::FirmwareIdentity,
    pci_fabric_generation: u64,
    reason: String,
) -> i64 {
    let platform_topology_generation =
        match rust_acpi_naos::next_instance_generation(identity.process_id) {
            Ok(generation) => generation,
            Err(error) => {
                log::error!("unable to allocate unavailable topology generation: {error:?}");
                return 1;
            }
        };
    serve_unavailable_topology(
        context,
        platform_topology::unavailable_generation(
            identity.firmware_version.clone(),
            platform_topology_generation,
            pci_fabric_generation,
        ),
        reason,
    )
    .await
}

async fn stop_executor_and_serve_unavailable(
    context: &Context,
    executor: ExecutorWorker,
    identity: rust_acpi_naos::FirmwareIdentity,
    pci_fabric_generation: u64,
    reason: String,
) -> i64 {
    if let Err(error) = executor.shutdown() {
        log::error!(
            "AML executor shutdown failed; terminating platformd for kernel owner cleanup: {error}"
        );
        return 1;
    }
    serve_firmware_unavailable(context, identity, pci_fabric_generation, reason).await
}

async fn mark_sci_consumer_failed(
    endpoint: &ProtocolClientEndpoint,
    state: &Arc<tokio::sync::RwLock<TopologyState>>,
    identity: rust_acpi_naos::FirmwareIdentity,
    invalidation: &platform_topology::TopologyInvalidation,
) -> Result<u64, String> {
    let request = acpi_fixed_hardware::report_sci_failure_request {
        firmware_version: identity.firmware_version.as_str(),
    };
    let mut request_wire = vec![
        0_u8;
        acpi_fixed_hardware::REPORT_SCI_FAILURE_REQUEST_HEADER_BYTES
            + request.firmware_version.len()
    ];
    let mut invocation = acpi_fixed_hardware::submit_report_sci_failure(
        endpoint,
        &request,
        ResourceTransferBuilder::new(),
        &mut request_wire,
        0,
    )
    .map_err(|error| format!("AcpiFixedHardware report_sci_failure submit: {error:?}"))?;
    if !completion_ready(invocation.get()).await {
        return Err("AcpiFixedHardware report_sci_failure did not complete".into());
    }
    let mut response_wire = [0_u8; 32];
    let response =
        acpi_fixed_hardware::take_report_sci_failure(&mut invocation, &mut response_wire)
            .map_err(|error| format!("AcpiFixedHardware report_sci_failure take: {error:?}"))?;
    if response.sci_state != 2 {
        return Err(format!(
            "AcpiFixedHardware did not persist SCI degradation state={}",
            response.sci_state
        ));
    }

    mark_initial_sci_failure(state).await;
    let generation = rust_acpi_naos::next_instance_generation(identity.process_id)
        .map_err(|error| format!("topology generation allocation failed: {error:?}"))?;
    let mut topology = state.write().await;
    if !platform_topology::mark_sci_consumer_failed(&mut topology, generation, invalidation) {
        return Err(format!(
            "nonadvancing topology generation {generation:016x}"
        ));
    }
    Ok(generation)
}

async fn mark_initial_sci_failure(state: &Arc<tokio::sync::RwLock<TopologyState>>) -> u32 {
    let mut topology = state.write().await;
    topology.generation.health |= crate::topology_health::HEALTH_DEGRADED;
    topology.generation.health
}

struct TopologyHandler {
    state: Arc<tokio::sync::RwLock<TopologyState>>,
    firmware_version: String,
    root_resource_attestations: Arc<tokio::sync::Mutex<BTreeMap<u64, OwnedHandle>>>,
    device_resource_attestations: Arc<tokio::sync::Mutex<BTreeMap<u64, OwnedHandle>>>,
}

impl platform_pci_topology::PlatformPciTopologyServiceKitHandler<()> for TopologyHandler {
    type Resource = EndpointResource;

    async fn generation<'s>(
        &'s mut self,
        _context: &mut (),
        _request: platform_pci_topology::generation_request,
    ) -> Result<
        RpcMethodReply<platform_pci_topology::generation_response<'s>, Self::Resource>,
        RpcOutcome,
    > {
        let state = self.state.read().await;
        self.firmware_version
            .clone_from(&state.generation.firmware_version);
        Ok(RpcMethodReply::new(platform_topology::generation_response(
            &state,
            self.firmware_version.as_str(),
        )))
    }

    async fn get_pci_roots<'s>(
        &'s mut self,
        _context: &mut (),
        request: platform_pci_topology::get_pci_roots_request<'_>,
    ) -> Result<
        RpcMethodReply<platform_pci_topology::get_pci_roots_response<'s>, Self::Resource>,
        RpcOutcome,
    > {
        let state = self.state.read().await;
        self.firmware_version
            .clone_from(&state.generation.firmware_version);
        platform_topology::get_pci_roots_response(&request, &state, self.firmware_version.as_str())
            .map(RpcMethodReply::new)
            .map_err(service_outcome)
    }

    async fn negotiate_pci_features<'s>(
        &'s mut self,
        _context: &mut (),
        request: platform_pci_topology::negotiate_pci_features_request<'_>,
    ) -> Result<
        RpcMethodReply<platform_pci_topology::negotiate_pci_features_response<'s>, Self::Resource>,
        RpcOutcome,
    > {
        log::info!(
            "PlatformPciTopology _OSC cache request root_id={:016x} requested_support={:08x} requested_control={:08x} firmware_version={} topology_generation={:016x}",
            request.root_id,
            request.requested_support,
            request.requested_control,
            request.firmware_version,
            request.platform_topology_generation
        );
        let state = self.state.read().await;
        self.firmware_version
            .clone_from(&state.generation.firmware_version);
        platform_topology::negotiate_pci_features_response(
            &request,
            &state,
            self.firmware_version.as_str(),
        )
        .map(RpcMethodReply::new)
        .map_err(service_outcome)
    }

    async fn list_devices<'s>(
        &'s mut self,
        _context: &mut (),
        request: platform_pci_topology::list_devices_request<'_>,
    ) -> Result<
        RpcMethodReply<platform_pci_topology::list_devices_response<'s>, Self::Resource>,
        RpcOutcome,
    > {
        let state = self.state.read().await;
        self.firmware_version
            .clone_from(&state.generation.firmware_version);
        platform_topology::list_devices_response(&request, &state, self.firmware_version.as_str())
            .map(RpcMethodReply::new)
            .map_err(service_outcome)
    }

    async fn get_resources<'s>(
        &'s mut self,
        _context: &mut (),
        request: platform_pci_topology::get_resources_request<'_>,
    ) -> Result<
        RpcMethodReply<platform_pci_topology::get_resources_response, Self::Resource>,
        RpcOutcome,
    > {
        let state = self.state.read().await;
        platform_topology::get_resources_response(&request, &state)
            .map(RpcMethodReply::new)
            .map_err(service_outcome)
    }

    async fn get_interrupt_routing<'s>(
        &'s mut self,
        _context: &mut (),
        request: platform_pci_topology::get_interrupt_routing_request<'_>,
    ) -> Result<
        RpcMethodReply<platform_pci_topology::get_interrupt_routing_response, Self::Resource>,
        RpcOutcome,
    > {
        let state = self.state.read().await;
        platform_topology::get_interrupt_routing_response(&request, &state)
            .map(RpcMethodReply::new)
            .map_err(service_outcome)
    }

    async fn take_root_resource_attestation<'s>(
        &'s mut self,
        _context: &mut (),
        request: platform_pci_topology::take_root_resource_attestation_request<'_>,
    ) -> Result<
        RpcMethodReply<
            platform_pci_topology::take_root_resource_attestation_response,
            Self::Resource,
        >,
        RpcOutcome,
    > {
        let root_is_current = {
            let state = self.state.read().await;
            state.generation.firmware_version == request.firmware_version
                && state.generation.platform_topology_generation
                    == request.platform_topology_generation
                && state
                    .roots
                    .iter()
                    .any(|root| root.root_id == request.root_id)
        };
        if !root_is_current {
            return Err(service_outcome(-116));
        }
        let attestation = self
            .root_resource_attestations
            .lock()
            .await
            .remove(&request.root_id)
            .ok_or_else(|| service_outcome(-2))?;
        let resource = EndpointResource::from_owned_capability(
            attestation,
            &naos_idl::pci_root_resource_attestation::protocol_descriptor(),
            naos_sys::RIGHT_TRANSFER | naos_sys::RIGHT_INSPECT,
            0,
        )
        .map_err(|error| {
            log::error!("root resource attestation handoff rejected: {error:?}");
            service_outcome(-13)
        })?;
        let mut reply = RpcMethodReply::new(
            platform_pci_topology::take_root_resource_attestation_response {
                attestation: ResourceSlot::new(0).ok_or_else(|| service_outcome(-75))?,
            },
        );
        reply.resources.push(resource);
        Ok(reply)
    }

    async fn take_device_resource_attestation<'s>(
        &'s mut self,
        _context: &mut (),
        request: platform_pci_topology::take_device_resource_attestation_request<'_>,
    ) -> Result<
        RpcMethodReply<
            platform_pci_topology::take_device_resource_attestation_response,
            Self::Resource,
        >,
        RpcOutcome,
    > {
        let device_is_current = {
            let state = self.state.read().await;
            state.generation.firmware_version == request.firmware_version
                && state.generation.platform_topology_generation
                    == request.platform_topology_generation
                && state
                    .roots
                    .iter()
                    .any(|root| root.root_id == request.root_id)
                && state.pci_device_paths.iter().any(|path| {
                    path.device_id == request.device_id && path.root_id == request.root_id
                })
        };
        if !device_is_current {
            return Err(service_outcome(-116));
        }
        let attestation = self
            .device_resource_attestations
            .lock()
            .await
            .remove(&request.device_id)
            .ok_or_else(|| service_outcome(-2))?;
        let resource = EndpointResource::from_owned_capability(
            attestation,
            &naos_idl::pci_device_resource_attestation::protocol_descriptor(),
            naos_sys::RIGHT_TRANSFER | naos_sys::RIGHT_INSPECT,
            0,
        )
        .map_err(|error| {
            log::error!("device resource attestation handoff rejected: {error:?}");
            service_outcome(-13)
        })?;
        let mut reply = RpcMethodReply::new(
            platform_pci_topology::take_device_resource_attestation_response {
                attestation: ResourceSlot::new(0).ok_or_else(|| service_outcome(-75))?,
            },
        );
        reply.resources.push(resource);
        Ok(reply)
    }
}

impl ServeHandler for TopologyHandler {
    async fn handle(&mut self, server: &mut Server, request: Request) {
        let dispatched = {
            let rpc_request = request.rpc_request();
            platform_pci_topology::dispatch_servicekit(&rpc_request, &mut (), self).await
        };
        if let Err(error) = server.respond_dispatch(request, dispatched).await {
            log::debug!("platform topology response failed: {error:?}");
        }
    }
}

async fn completion_ready(handle: naos_sys::Handle) -> bool {
    match servicekit::wait_ready_async(&[handle], None).await {
        Ok(event) => event.readable,
        Err(_) => false,
    }
}

struct KernelPowerTransitionBackend {
    endpoint: Arc<ProtocolClientEndpoint>,
    info: FixedHardwareSnapshot,
}

fn power_call_error(error: naos_idl::CallError) -> i64 {
    match error {
        naos_idl::CallError::Outcome {
            protocol_error: error,
            ..
        } if error != 0 => {
            if error < 0 {
                error
            } else {
                -error
            }
        }
        _ => -5,
    }
}

fn commit_take_error(error: naos_idl::CallError) -> CommitError<i64> {
    match error {
        naos_idl::CallError::Outcome {
            execution: naos_idl::EXECUTION_NONE,
            protocol_error,
            ..
        } if protocol_error != 0 => CommitError::Rejected(if protocol_error < 0 {
            protocol_error
        } else {
            -protocol_error
        }),
        naos_idl::CallError::Outcome { .. } => CommitError::Unknown(-5),
        other => CommitError::Unknown(power_call_error(other)),
    }
}

impl KernelPowerTransitionBackend {
    async fn prepare_sleep(&self, type_a: u32, type_b: u32) -> Result<TransitionTicket, i64> {
        let request = acpi_fixed_hardware::prepare_sleep_request {
            firmware_version: self.info.firmware_version.as_str(),
            sleep_type_a: type_a,
            sleep_type_b: type_b,
        };
        let mut request_wire = vec![
            0_u8;
            acpi_fixed_hardware::PREPARE_SLEEP_REQUEST_HEADER_BYTES
                + request.firmware_version.len()
        ];
        let mut invocation = acpi_fixed_hardware::submit_prepare_sleep(
            &self.endpoint,
            &request,
            ResourceTransferBuilder::new(),
            &mut request_wire,
            0,
        )
        .map_err(power_call_error)?;
        if !completion_ready(invocation.get()).await {
            return Err(-5);
        }
        let mut response_wire = [0_u8; 96];
        let response = acpi_fixed_hardware::take_prepare_sleep(&mut invocation, &mut response_wire)
            .map_err(power_call_error)?;
        let ticket = TransitionTicket {
            kind: TransitionKind::Sleep,
            ticket: response.ticket.ticket,
            firmware_version: response.firmware_version.to_owned(),
        };
        if ticket.ticket == 0 || ticket.firmware_version != self.info.firmware_version {
            if ticket.ticket != 0 {
                let _ = self.abort(&ticket).await;
            }
            return Err(-116);
        }
        Ok(ticket)
    }

    async fn prepare_reset(&self) -> Result<TransitionTicket, i64> {
        let request = acpi_fixed_hardware::prepare_reset_request {
            firmware_version: self.info.firmware_version.as_str(),
        };
        let mut request_wire = vec![
            0_u8;
            acpi_fixed_hardware::PREPARE_RESET_REQUEST_HEADER_BYTES
                + request.firmware_version.len()
        ];
        let mut invocation = acpi_fixed_hardware::submit_prepare_reset(
            &self.endpoint,
            &request,
            ResourceTransferBuilder::new(),
            &mut request_wire,
            0,
        )
        .map_err(power_call_error)?;
        if !completion_ready(invocation.get()).await {
            return Err(-5);
        }
        let mut response_wire = [0_u8; 96];
        let response = acpi_fixed_hardware::take_prepare_reset(&mut invocation, &mut response_wire)
            .map_err(power_call_error)?;
        let ticket = TransitionTicket {
            kind: TransitionKind::Reset,
            ticket: response.ticket.ticket,
            firmware_version: response.firmware_version.to_owned(),
        };
        if ticket.ticket == 0 || ticket.firmware_version != self.info.firmware_version {
            if ticket.ticket != 0 {
                let _ = self.abort(&ticket).await;
            }
            return Err(-116);
        }
        Ok(ticket)
    }

    async fn abort(&self, ticket: &TransitionTicket) -> Result<(), i64> {
        let request = acpi_fixed_hardware::abort_transition_request {
            ticket: ticket.ticket,
            firmware_version: ticket.firmware_version.as_str(),
        };
        let mut request_wire = vec![
            0_u8;
            acpi_fixed_hardware::ABORT_TRANSITION_REQUEST_HEADER_BYTES
                + request.firmware_version.len()
        ];
        let mut invocation = acpi_fixed_hardware::submit_abort_transition(
            &self.endpoint,
            &request,
            ResourceTransferBuilder::new(),
            &mut request_wire,
            0,
        )
        .map_err(power_call_error)?;
        if !completion_ready(invocation.get()).await {
            return Err(-5);
        }
        let mut response_wire = [];
        acpi_fixed_hardware::take_abort_transition(&mut invocation, &mut response_wire)
            .map_err(power_call_error)?;
        Ok(())
    }
}

impl TransitionBackend for KernelPowerTransitionBackend {
    type Error = i64;

    async fn prepare(&self, request: TransitionRequest) -> Result<TransitionTicket, Self::Error> {
        match request {
            TransitionRequest::Sleep { type_a, type_b } => self.prepare_sleep(type_a, type_b).await,
            TransitionRequest::Reset => self.prepare_reset().await,
        }
    }

    async fn commit(&self, ticket: &TransitionTicket) -> Result<u32, CommitError<Self::Error>> {
        match ticket.kind {
            TransitionKind::Sleep => {
                let request = acpi_fixed_hardware::commit_sleep_request {
                    ticket: ticket.ticket,
                    firmware_version: ticket.firmware_version.as_str(),
                };
                let mut request_wire = vec![
                    0_u8;
                    acpi_fixed_hardware::COMMIT_SLEEP_REQUEST_HEADER_BYTES
                        + request.firmware_version.len()
                ];
                let mut invocation = acpi_fixed_hardware::submit_commit_sleep(
                    &self.endpoint,
                    &request,
                    ResourceTransferBuilder::new(),
                    &mut request_wire,
                    0,
                )
                .map_err(|error| CommitError::Rejected(power_call_error(error)))?;
                if !completion_ready(invocation.get()).await {
                    return Err(CommitError::Unknown(-5));
                }
                let mut response_wire = [0_u8; 32];
                match acpi_fixed_hardware::take_commit_sleep(&mut invocation, &mut response_wire) {
                    Ok(response) => Ok(response.result.outcome),
                    Err(error) => Err(commit_take_error(error)),
                }
            }
            TransitionKind::Reset => {
                let request = acpi_fixed_hardware::commit_reset_request {
                    ticket: ticket.ticket,
                    firmware_version: ticket.firmware_version.as_str(),
                };
                let mut request_wire = vec![
                    0_u8;
                    acpi_fixed_hardware::COMMIT_RESET_REQUEST_HEADER_BYTES
                        + request.firmware_version.len()
                ];
                let mut invocation = acpi_fixed_hardware::submit_commit_reset(
                    &self.endpoint,
                    &request,
                    ResourceTransferBuilder::new(),
                    &mut request_wire,
                    0,
                )
                .map_err(|error| CommitError::Rejected(power_call_error(error)))?;
                if !completion_ready(invocation.get()).await {
                    return Err(CommitError::Unknown(-5));
                }
                let mut response_wire = [0_u8; 32];
                match acpi_fixed_hardware::take_commit_reset(&mut invocation, &mut response_wire) {
                    Ok(response) => Ok(response.result.outcome),
                    Err(error) => Err(commit_take_error(error)),
                }
            }
        }
    }

    async fn abort(&self, ticket: &TransitionTicket) -> Result<(), Self::Error> {
        KernelPowerTransitionBackend::abort(self, ticket).await
    }
}

struct PlatformPowerControlHandler {
    backend: KernelPowerTransitionBackend,
    s5_sleep_types: Option<rust_acpi_naos::SleepTypes>,
}

impl ServeHandler for PlatformPowerControlHandler {
    async fn handle(&mut self, server: &mut Server, request: Request) {
        let dispatched = {
            let rpc_request = request.rpc_request();
            naos_idl::platform_power_control::dispatch_servicekit(&rpc_request, &mut (), self).await
        };
        if let Err(error) = server.respond_dispatch(request, dispatched).await {
            log::warn!("PlatformPowerControl response failed: {error:?}");
        }
    }
}

impl naos_idl::platform_power_control::PlatformPowerControlServiceKitHandler<()>
    for PlatformPowerControlHandler
{
    type Resource = servicekit::server::EndpointResource;

    async fn power_off<'s>(
        &'s mut self,
        _context: &mut (),
        _request: naos_idl::platform_power_control::power_off_request,
    ) -> Result<
        RpcMethodReply<naos_idl::platform_power_control::power_off_response, Self::Resource>,
        RpcOutcome,
    > {
        let Some(types) = self.s5_sleep_types else {
            return Err(service_outcome(-95));
        };
        let outcome = self
            .execute_transition(TransitionRequest::Sleep {
                type_a: u32::from(types.type_a),
                type_b: u32::from(types.type_b),
            })
            .await?;
        Ok(RpcMethodReply::new(
            naos_idl::platform_power_control::power_off_response {
                result: naos_idl::platform_power_control::PowerTransitionResult { outcome },
            },
        ))
    }

    async fn reboot<'s>(
        &'s mut self,
        _context: &mut (),
        _request: naos_idl::platform_power_control::reboot_request,
    ) -> Result<
        RpcMethodReply<naos_idl::platform_power_control::reboot_response, Self::Resource>,
        RpcOutcome,
    > {
        let outcome = self.execute_transition(TransitionRequest::Reset).await?;
        Ok(RpcMethodReply::new(
            naos_idl::platform_power_control::reboot_response {
                result: naos_idl::platform_power_control::PowerTransitionResult { outcome },
            },
        ))
    }
}

impl PlatformPowerControlHandler {
    async fn execute_transition(&self, transition: TransitionRequest) -> Result<u32, RpcOutcome> {
        match perform_transition(&self.backend, transition).await {
            Ok(TransitionOutcome::Committed) => Ok(0),
            Ok(TransitionOutcome::Failed) => Ok(1),
            Ok(TransitionOutcome::Unknown) => Ok(2),
            Err(TransitionFailure::Prepare(error) | TransitionFailure::CommitRejected(error)) => {
                Err(service_outcome(error))
            }
            Err(TransitionFailure::CommitUnknown(error)) => {
                log::warn!("platform power transition commit transport outcome unknown: {error}");
                Ok(2)
            }
            Err(TransitionFailure::InvalidOutcome(value)) => {
                log::error!("kernel returned invalid ACPI transition outcome={value}");
                Ok(2)
            }
        }
    }
}

fn service_outcome(protocol_error: i64) -> RpcOutcome {
    RpcOutcome {
        execution: 0,
        reason: 0,
        protocol_error,
    }
}

async fn list_objects(endpoint: &ProtocolClientEndpoint) -> Result<Vec<catalog::Object>, String> {
    let mut cursor = 0_u64;
    let mut firmware_version: Option<String> = None;
    let mut objects = Vec::<catalog::Object>::new();
    loop {
        let request = firmware_root::list_objects_request { cursor, limit: 64 };
        let mut request_wire = [0_u8; firmware_root::LIST_OBJECTS_REQUEST_HEADER_BYTES];
        let mut invocation = firmware_root::submit_list_objects(
            endpoint,
            &request,
            ResourceTransferBuilder::new(),
            &mut request_wire,
            0,
        )
        .map_err(|error| format!("list_objects submit: {error:?}"))?;
        if !completion_ready(invocation.get()).await {
            return Err("list_objects did not complete".into());
        }
        let mut response_wire = [0_u8; 4096];
        let response = firmware_root::take_list_objects(&mut invocation, &mut response_wire)
            .map_err(|error| format!("list_objects take: {error:?}"))?;
        if response.firmware_version.is_empty()
            || firmware_version
                .as_deref()
                .is_some_and(|version| version != response.firmware_version)
        {
            return Err("list_objects returned an empty or changing firmware identity".into());
        }
        firmware_version.get_or_insert_with(|| response.firmware_version.to_owned());
        catalog::reserve_append(
            &mut objects,
            response.objects.len(),
            catalog::MAX_FIRMWARE_TABLE_COUNT,
        )
        .map_err(|error| format!("list_objects catalog bound/allocation: {error:?}"))?;
        for object in response.objects {
            objects.push(catalog::Object {
                object_id: object.object_id,
                signature: object.signature.to_le_bytes(),
                length: object.length,
                root_ordinal: object.root_ordinal,
                aml_load_ordinal: object.aml_load_ordinal,
                firmware_version: response.firmware_version.to_owned(),
            });
        }
        if response.next_cursor == 0 {
            return Ok(objects);
        }
        if response.next_cursor <= cursor {
            return Err("list_objects returned a non-advancing cursor".into());
        }
        cursor = response.next_cursor;
    }
}

async fn describe_fixed_hardware(
    context: &Context,
) -> Result<(ProtocolClientEndpoint, FixedHardwareSnapshot), String> {
    let resource =
        servicekit::resolve_resource(context.service_directory_handle(), ACPI_FIXED_HARDWARE_URI)
            .map_err(|error| format!("AcpiFixedHardware resolve: {error:?}"))?;
    let endpoint = unsafe { ProtocolClientEndpoint::from_raw(resource.into_raw()) };

    let describe_request = acpi_fixed_hardware::describe_request {};
    let mut describe_wire = [0_u8; acpi_fixed_hardware::DESCRIBE_REQUEST_HEADER_BYTES];
    let mut describe = acpi_fixed_hardware::submit_describe(
        &endpoint,
        &describe_request,
        ResourceTransferBuilder::new(),
        &mut describe_wire,
        0,
    )
    .map_err(|error| format!("AcpiFixedHardware describe submit: {error:?}"))?;
    if !completion_ready(describe.get()).await {
        return Err("AcpiFixedHardware describe did not complete".into());
    }
    let mut describe_response_wire = [0_u8; 128];
    let description =
        acpi_fixed_hardware::take_describe(&mut describe, &mut describe_response_wire)
            .map_err(|error| format!("AcpiFixedHardware describe take: {error:?}"))?;
    if description.firmware_version.is_empty()
        || description.value.aml_namespace_generation == 0
        || description.value.platform_topology_generation == 0
        || description.value.pci_fabric_generation == 0
    {
        return Err("AcpiFixedHardware returned invalid identity".into());
    }
    log::info!(
        "fixed hardware inventory sci_gsi={} pm1_event_bytes={} gpe0_bytes={} gpe1_bytes={} gpe1_base={:#x} reset_supported={} sleep_supported={}",
        description.value.sci_gsi,
        description.value.pm1_event_bytes,
        description.value.gpe0_bytes,
        description.value.gpe1_bytes,
        description.value.gpe1_base,
        description.value.reset_supported,
        description.value.sleep_supported
    );

    Ok((
        endpoint,
        FixedHardwareSnapshot {
            firmware_version: description.firmware_version.to_owned(),
            value: description.value,
        },
    ))
}

async fn claim_transition_owner(
    endpoint: &ProtocolClientEndpoint,
    info: &FixedHardwareSnapshot,
) -> Result<(), String> {
    let request = acpi_fixed_hardware::claim_transition_owner_request {
        firmware_version: info.firmware_version.as_str(),
    };
    let mut request_wire = vec![
        0_u8;
        acpi_fixed_hardware::CLAIM_TRANSITION_OWNER_REQUEST_HEADER_BYTES
            + request.firmware_version.len()
    ];
    let mut invocation = acpi_fixed_hardware::submit_claim_transition_owner(
        endpoint,
        &request,
        ResourceTransferBuilder::new(),
        &mut request_wire,
        0,
    )
    .map_err(|error| format!("AcpiFixedHardware claim_transition_owner submit: {error:?}"))?;
    if !completion_ready(invocation.get()).await {
        return Err("AcpiFixedHardware claim_transition_owner did not complete".into());
    }
    let mut response_wire = [0_u8; 64];
    let response =
        acpi_fixed_hardware::take_claim_transition_owner(&mut invocation, &mut response_wire)
            .map_err(|error| format!("AcpiFixedHardware claim_transition_owner take: {error:?}"))?;
    if response.firmware_version != info.firmware_version {
        return Err("AcpiFixedHardware transition-owner identity changed".into());
    }
    Ok(())
}

async fn claim_acpi_sci(
    endpoint: &ProtocolClientEndpoint,
    info: &FixedHardwareSnapshot,
) -> Result<(), String> {
    let claim_request = acpi_fixed_hardware::claim_sci_request {
        firmware_version: info.firmware_version.as_str(),
    };
    let mut claim_wire = vec![
        0_u8;
        acpi_fixed_hardware::CLAIM_SCI_REQUEST_HEADER_BYTES
            + claim_request.firmware_version.len()
    ];
    let mut claim = acpi_fixed_hardware::submit_claim_sci(
        endpoint,
        &claim_request,
        ResourceTransferBuilder::new(),
        &mut claim_wire,
        0,
    )
    .map_err(|error| format!("AcpiFixedHardware claim_sci submit: {error:?}"))?;
    if !completion_ready(claim.get()).await {
        return Err("AcpiFixedHardware claim_sci did not complete".into());
    }
    let mut claim_response_wire = [0_u8; 64];
    let claim_response = acpi_fixed_hardware::take_claim_sci(&mut claim, &mut claim_response_wire)
        .map_err(|error| format!("AcpiFixedHardware claim_sci take: {error:?}"))?;
    if claim_response.firmware_version != info.firmware_version {
        return Err("AcpiFixedHardware claim identity changed".into());
    }
    Ok(())
}

#[derive(Clone, Copy, Debug, Default)]
struct GpeEnableMasks {
    gpe0: u64,
    gpe1: u64,
    handlers: usize,
}

async fn resolve_gpe_enable_masks(
    info: &FixedHardwareSnapshot,
    executor: &ExecutorWorker,
) -> Result<GpeEnableMasks, String> {
    executor.begin_sci_work_unit().await?;
    let resolution = async {
        let mut masks = GpeEnableMasks::default();
        let mut visited = 0_usize;
        for (bank, block_bytes, number_base) in [
            (GpeBank::Gpe0, info.gpe0_bytes, 0),
            (GpeBank::Gpe1, info.gpe1_bytes, info.gpe1_base),
        ] {
            let mut candidates = GpeEventIter::all_events(bank, block_bytes, number_base)
                .map_err(|error| format!("invalid {bank:?} GPE inventory: {error:?}"))?;
            while let Some(event) = candidates
                .next_event()
                .map_err(|error| format!("invalid {bank:?} GPE candidate range: {error:?}"))?
            {
                visited = visited
                    .checked_add(1)
                    .filter(|count| *count <= MAX_GPE_EVENTS_PER_SCI)
                    .ok_or_else(|| {
                        "GPE candidate count exceeds the fixed status-bank bound".to_string()
                    })?;
                if let GpeDispatchResult::Handled(_) =
                    executor.resolve_gpe_handler(event.number).await?
                {
                    match event.bank {
                        GpeBank::Gpe0 => masks.gpe0 |= event.mask,
                        GpeBank::Gpe1 => masks.gpe1 |= event.mask,
                    }
                    masks.handlers += 1;
                }
            }
        }
        Ok::<_, String>(masks)
    }
    .await;
    let finish = executor.end_sci_work_unit().await;
    match (resolution, finish) {
        (Ok(masks), Ok(())) => Ok(masks),
        (Err(error), Ok(())) => Err(error),
        (Ok(_), Err(error)) => Err(format!("SCI GPE discovery budget cleanup failed: {error}")),
        (Err(error), Err(finish_error)) => Err(format!(
            "{error}; SCI GPE discovery budget cleanup failed: {finish_error}"
        )),
    }
}

async fn enable_gpe_mask(
    endpoint: &ProtocolClientEndpoint,
    info: &FixedHardwareSnapshot,
    bank: u32,
    mask: u64,
) -> Result<(), String> {
    if mask == 0 {
        return Ok(());
    }
    let request = acpi_fixed_hardware::set_gpe_mask_request {
        firmware_version: info.firmware_version.as_str(),
        bank,
        mask,
        enable: 1,
    };
    let mut request_wire = vec![
        0_u8;
        acpi_fixed_hardware::SET_GPE_MASK_REQUEST_HEADER_BYTES
            + request.firmware_version.len()
    ];
    let mut invocation = acpi_fixed_hardware::submit_set_gpe_mask(
        endpoint,
        &request,
        ResourceTransferBuilder::new(),
        &mut request_wire,
        0,
    )
    .map_err(|error| format!("AcpiFixedHardware set_gpe_mask submit bank={bank}: {error:?}"))?;
    if !completion_ready(invocation.get()).await {
        return Err(format!(
            "AcpiFixedHardware set_gpe_mask did not complete bank={bank}"
        ));
    }
    let mut response_wire = [];
    acpi_fixed_hardware::take_set_gpe_mask(&mut invocation, &mut response_wire)
        .map_err(|error| format!("AcpiFixedHardware set_gpe_mask take bank={bank}: {error:?}"))?;
    Ok(())
}

async fn enable_gpe_handlers(
    endpoint: &ProtocolClientEndpoint,
    info: &FixedHardwareSnapshot,
    masks: GpeEnableMasks,
) -> Result<(), String> {
    enable_gpe_mask(endpoint, info, 0, masks.gpe0).await?;
    enable_gpe_mask(endpoint, info, 1, masks.gpe1).await
}

async fn publish_power_control(
    context: &Context,
    endpoint: Arc<ProtocolClientEndpoint>,
    info: FixedHardwareSnapshot,
    s5_sleep_types: Option<rust_acpi_naos::SleepTypes>,
) -> Result<(), String> {
    let mut server = Server::publish(
        &context.service_directory(),
        PLATFORM_POWER_CONTROL_URI,
        &naos_idl::platform_power_control::protocol_descriptor(),
        validate_power_control_request,
    )
    .await
    .map_err(|error| format!("PlatformPowerControl publish: {error:?}"))?;
    server.set_max_in_flight(1);
    tokio::spawn(async move {
        let mut handler = PlatformPowerControlHandler {
            backend: KernelPowerTransitionBackend { endpoint, info },
            s5_sleep_types,
        };
        let status = server.serve(&mut handler).await;
        log::error!("PlatformPowerControl server stopped status={status}");
    });
    log::info!("PlatformPowerControl published uri={PLATFORM_POWER_CONTROL_URI}");
    Ok(())
}

async fn clear_sci_status(
    endpoint: &ProtocolClientEndpoint,
    info: &FixedHardwareSnapshot,
    sequence: u64,
    pm1_event_mask: u64,
    pm1_event_status_b_mask: u64,
    gpe0_mask: u64,
    gpe1_mask: u64,
) -> Result<(), String> {
    if pm1_event_mask == 0 && pm1_event_status_b_mask == 0 && gpe0_mask == 0 && gpe1_mask == 0 {
        return Ok(());
    }
    let request = acpi_fixed_hardware::clear_sci_status_request {
        firmware_version: info.firmware_version.as_str(),
        sequence,
        pm1_event_mask,
        pm1_event_status_b_mask,
        gpe0_mask,
        gpe1_mask,
    };
    let mut request_wire = vec![
        0_u8;
        acpi_fixed_hardware::CLEAR_SCI_STATUS_REQUEST_HEADER_BYTES
            + request.firmware_version.len()
    ];
    let mut invocation = acpi_fixed_hardware::submit_clear_sci_status(
        endpoint,
        &request,
        ResourceTransferBuilder::new(),
        &mut request_wire,
        0,
    )
    .map_err(|error| format!("SCI status clear submit: {error:?}"))?;
    if !completion_ready(invocation.get()).await {
        return Err("SCI status clear did not complete".into());
    }
    let mut response_wire = [0_u8; acpi_fixed_hardware::CLEAR_SCI_STATUS_RESPONSE_HEADER_BYTES];
    acpi_fixed_hardware::take_clear_sci_status(&mut invocation, &mut response_wire)
        .map_err(|error| format!("SCI status clear take: {error:?}"))?;
    Ok(())
}

async fn drain_and_rearm_sci(
    endpoint: &ProtocolClientEndpoint,
    info: &FixedHardwareSnapshot,
    executor: &ExecutorWorker,
) -> Result<(), String> {
    match tokio::time::timeout(
        SCI_WORK_UNIT_TIMEOUT,
        drain_and_rearm_sci_with_deadline(endpoint, info, executor),
    )
    .await
    {
        Ok(result) => result,
        Err(_) => {
            // The timeout drops the drain future, so its normal End request
            // may never reach the executor. End is idempotent to cover the
            // race where that request was already queued or in flight.
            if let Err(error) = executor.end_sci_work_unit().await {
                log::warn!("SCI aggregate budget timeout cleanup failed: {error}");
            }
            Err("SCI drain exceeded its 10-second work-unit deadline; SCI remains masked".into())
        }
    }
}

async fn drain_and_rearm_sci_with_deadline(
    endpoint: &ProtocolClientEndpoint,
    info: &FixedHardwareSnapshot,
    executor: &ExecutorWorker,
) -> Result<(), String> {
    executor.begin_sci_work_unit().await?;
    let drain_result = drain_and_rearm_sci_with_budget(endpoint, info, executor).await;
    let finish_result = executor.end_sci_work_unit().await;
    match drain_result {
        Err(error) => {
            if let Err(finish_error) = finish_result {
                log::warn!("SCI aggregate budget cleanup failed: {finish_error}");
            }
            Err(error)
        }
        Ok(()) => finish_result,
    }
}

async fn drain_and_rearm_sci_with_budget(
    endpoint: &ProtocolClientEndpoint,
    info: &FixedHardwareSnapshot,
    executor: &ExecutorWorker,
) -> Result<(), String> {
    // Read the sequence only after readiness fires. A describe issued before
    // claim or before the interrupt can race the kernel's sequence advance.
    let describe_request = acpi_fixed_hardware::describe_request {};
    let mut describe_wire = [0_u8; acpi_fixed_hardware::DESCRIBE_REQUEST_HEADER_BYTES];
    let mut describe = acpi_fixed_hardware::submit_describe(
        endpoint,
        &describe_request,
        ResourceTransferBuilder::new(),
        &mut describe_wire,
        0,
    )
    .map_err(|error| format!("AcpiFixedHardware describe submit: {error:?}"))?;
    if !completion_ready(describe.get()).await {
        return Err("AcpiFixedHardware describe did not complete".into());
    }
    let mut describe_response_wire = [0_u8; 128];
    let description =
        acpi_fixed_hardware::take_describe(&mut describe, &mut describe_response_wire)
            .map_err(|error| format!("AcpiFixedHardware describe take: {error:?}"))?;
    if description.firmware_version != info.firmware_version {
        return Err("AcpiFixedHardware identity changed during SCI ownership".into());
    }
    let expected_lineage = SciGenerationLineage::from_info(&info.value, &info.firmware_version);
    if SciGenerationLineage::from_info(&description.value, description.firmware_version)
        != expected_lineage
    {
        return Err("AcpiFixedHardware platform lineage changed during SCI ownership".into());
    }
    let mut sequence = description.value.sci_sequence;
    for _ in 0..8 {
        let drain_request = acpi_fixed_hardware::drain_sci_request { sequence };
        let mut drain_wire = [0_u8; acpi_fixed_hardware::DRAIN_SCI_REQUEST_HEADER_BYTES];
        let mut drain = acpi_fixed_hardware::submit_drain_sci(
            endpoint,
            &drain_request,
            ResourceTransferBuilder::new(),
            &mut drain_wire,
            0,
        )
        .map_err(|error| format!("SCI drain submit: {error:?}"))?;
        if !completion_ready(drain.get()).await {
            return Err("SCI drain did not complete".into());
        }
        let mut drain_response_wire = [0_u8; 128];
        let drain_response =
            acpi_fixed_hardware::take_drain_sci(&mut drain, &mut drain_response_wire)
                .map_err(|error| format!("SCI drain take: {error:?}"))?;
        if !expected_lineage.matches_event(&drain_response.event, drain_response.firmware_version) {
            return Err("SCI event belongs to a stale platform generation lineage".into());
        }
        log::info!(
            "SCI event sequence={} flags={} pm1={:x} gpe0={:x} gpe1={:x}",
            drain_response.event.sequence,
            drain_response.event.flags,
            drain_response.event.pm1_event_status,
            drain_response.event.gpe0_status,
            drain_response.event.gpe1_status
        );

        let pm1_event_timer = drain_response.event.pm1_event_status & PM1_TIMER_STATUS_MASK;
        let pm1_event_b_timer = drain_response.event.pm1_event_status_b & PM1_TIMER_STATUS_MASK;
        let pm1_event_global_lock =
            drain_response.event.pm1_event_status & PM1_GLOBAL_LOCK_STATUS_MASK;
        let pm1_event_b_global_lock =
            drain_response.event.pm1_event_status_b & PM1_GLOBAL_LOCK_STATUS_MASK;
        let pm1_event_clear = pm1_safe_ack_status(drain_response.event.pm1_event_status);
        let pm1_event_b_clear = pm1_safe_ack_status(drain_response.event.pm1_event_status_b);
        if pm1_has_unhandled_status(
            drain_response.event.pm1_event_status,
            drain_response.event.pm1_event_status_b,
        ) {
            return Err(
                "PM1 fixed-event status has no admitted handler; keeping SCI masked".into(),
            );
        }
        if pm1_event_clear != 0 || pm1_event_b_clear != 0 {
            clear_sci_status(
                endpoint,
                info,
                sequence,
                pm1_event_clear,
                pm1_event_b_clear,
                0,
                0,
            )
            .await?;
            if pm1_event_timer != 0 || pm1_event_b_timer != 0 {
                log::info!(
                    "PM1 timer carry status acknowledged sequence={} status_a={:#x} status_b={:#x}",
                    sequence,
                    pm1_event_timer,
                    pm1_event_b_timer
                );
            }
            if pm1_event_global_lock != 0 || pm1_event_b_global_lock != 0 {
                log::info!(
                    "PM1 Global Lock status acknowledged after kernel wake sequence={} status_a={:#x} status_b={:#x}",
                    sequence,
                    pm1_event_global_lock,
                    pm1_event_b_global_lock
                );
            }
        }

        let mut pending_handlers = [None; MAX_GPE_EVENTS_PER_SCI];
        let mut pending_handler_count = 0;
        for (bank, status, block_bytes, number_base) in [
            (
                GpeBank::Gpe0,
                drain_response.event.gpe0_status,
                info.gpe0_bytes,
                0_u32,
            ),
            (
                GpeBank::Gpe1,
                drain_response.event.gpe1_status,
                info.gpe1_bytes,
                info.gpe1_base,
            ),
        ] {
            let mut events = GpeEventIter::new(bank, status, block_bytes, number_base)
                .map_err(|error| format!("invalid {bank:?} status inventory: {error:?}"))?;
            while let Some(event) = events
                .next_event()
                .map_err(|error| format!("invalid {bank:?} status inventory: {error:?}"))?
            {
                if event.bank != bank {
                    return Err("GPE event enumerator returned the wrong bank".into());
                }
                match executor.resolve_gpe_handler(event.number).await? {
                    rust_acpi_naos::GpeDispatchResult::Handled(kind) => {
                        let Some(slot) = pending_handlers.get_mut(pending_handler_count) else {
                            return Err(
                                "GPE event count exceeded fixed status-bank capacity".into()
                            );
                        };
                        *slot = Some((event, kind));
                        pending_handler_count += 1;
                    }
                    rust_acpi_naos::GpeDispatchResult::NoHandler => {
                        return Err(format!(
                            "GPE bank={bank:?} number={:#x} has no AML event handler; keeping SCI masked",
                            event.number
                        ));
                    }
                }
            }
        }

        // ACPI's GPE EOI protocol clears edge status before dispatch and
        // level status after the matching AML method returns.
        let mut edge_gpe0 = 0_u64;
        let mut edge_gpe1 = 0_u64;
        for (event, kind) in pending_handlers[..pending_handler_count].iter().flatten() {
            if gpe_eoi_timing(*kind == rust_acpi_naos::GpeHandlerKind::Edge)
                == GpeEoiTiming::BeforeHandler
            {
                match event.bank {
                    GpeBank::Gpe0 => edge_gpe0 |= event.mask,
                    GpeBank::Gpe1 => edge_gpe1 |= event.mask,
                }
            }
        }
        clear_sci_status(endpoint, info, sequence, 0, 0, edge_gpe0, edge_gpe1).await?;

        for (event, kind) in pending_handlers[..pending_handler_count]
            .iter()
            .flatten()
            .copied()
        {
            executor.execute_gpe_handler(event.number, kind).await?;
            log::info!(
                "SCI GPE handler completed number={:#x} kind={kind:?}",
                event.number
            );
            if gpe_eoi_timing(kind == rust_acpi_naos::GpeHandlerKind::Edge)
                == GpeEoiTiming::AfterHandler
            {
                match event.bank {
                    GpeBank::Gpe0 => {
                        clear_sci_status(endpoint, info, sequence, 0, 0, event.mask, 0).await?;
                    }
                    GpeBank::Gpe1 => {
                        clear_sci_status(endpoint, info, sequence, 0, 0, 0, event.mask).await?;
                    }
                }
            }
        }

        let ack_request = acpi_fixed_hardware::ack_and_rearm_sci_request { sequence };
        let mut ack_wire = [0_u8; acpi_fixed_hardware::ACK_AND_REARM_SCI_REQUEST_HEADER_BYTES];
        let mut ack = acpi_fixed_hardware::submit_ack_and_rearm_sci(
            endpoint,
            &ack_request,
            ResourceTransferBuilder::new(),
            &mut ack_wire,
            0,
        )
        .map_err(|error| format!("SCI ack submit: {error:?}"))?;
        if !completion_ready(ack.get()).await {
            return Err("SCI ack did not complete".into());
        }
        let mut ack_response_wire = [0_u8; 128];
        let ack_response =
            acpi_fixed_hardware::take_ack_and_rearm_sci(&mut ack, &mut ack_response_wire)
                .map_err(|error| format!("SCI ack take: {error:?}"))?;
        if !expected_lineage.matches_ack(&ack_response.ack, ack_response.firmware_version) {
            return Err(
                "SCI acknowledgement belongs to a stale platform generation lineage".into(),
            );
        }
        log::info!(
            "SCI acknowledgement sequence={} state={} flags={:#x}",
            ack_response.ack.sequence,
            ack_response.ack.state,
            ack_response.ack.flags
        );
        if ack_response.ack.state == 0 {
            return Ok(());
        }
        sequence = ack_response.ack.sequence;
    }
    Err("SCI rearm exceeded bounded coalesced-event drain".into())
}

async fn list_relations(
    endpoint: &ProtocolClientEndpoint,
) -> Result<Vec<catalog::Relation>, String> {
    let mut cursor = 0_u64;
    let mut firmware_version: Option<String> = None;
    let mut relations = Vec::<catalog::Relation>::new();
    loop {
        let request = firmware_root::list_relations_request { cursor, limit: 64 };
        let mut request_wire = [0_u8; firmware_root::LIST_RELATIONS_REQUEST_HEADER_BYTES];
        let mut invocation = firmware_root::submit_list_relations(
            endpoint,
            &request,
            ResourceTransferBuilder::new(),
            &mut request_wire,
            0,
        )
        .map_err(|error| format!("list_relations submit: {error:?}"))?;
        if !completion_ready(invocation.get()).await {
            return Err("list_relations did not complete".into());
        }
        let mut response_wire = [0_u8; 4096];
        let response = firmware_root::take_list_relations(&mut invocation, &mut response_wire)
            .map_err(|error| format!("list_relations take: {error:?}"))?;
        if response.firmware_version.is_empty()
            || firmware_version
                .as_deref()
                .is_some_and(|version| version != response.firmware_version)
        {
            return Err("list_relations returned an empty or changing firmware identity".into());
        }
        firmware_version.get_or_insert_with(|| response.firmware_version.to_owned());
        catalog::reserve_append(
            &mut relations,
            response.relations.len(),
            catalog::MAX_FIRMWARE_RELATION_COUNT,
        )
        .map_err(|error| format!("list_relations catalog bound/allocation: {error:?}"))?;
        relations.extend(
            response
                .relations
                .into_iter()
                .map(|relation| catalog::Relation {
                    from_object: relation.from_object,
                    to_object: relation.to_object,
                    kind: relation.kind,
                    ordinal: relation.ordinal,
                    firmware_version: response.firmware_version.to_owned(),
                }),
        );
        if response.next_cursor == 0 {
            return Ok(relations);
        }
        if response.next_cursor <= cursor {
            return Err("list_relations returned a non-advancing cursor".into());
        }
        cursor = response.next_cursor;
    }
}

async fn open_object(
    endpoint: &ProtocolClientEndpoint,
    object_id: u64,
    firmware_version: &str,
) -> Result<MemoryObject, String> {
    let request = firmware_root::open_object_request {
        object_id,
        firmware_version,
    };
    let mut request_wire = vec![
        0_u8;
        firmware_root::OPEN_OBJECT_REQUEST_HEADER_BYTES
            + request.firmware_version.len()
    ];
    let mut invocation = firmware_root::submit_open_object(
        endpoint,
        &request,
        ResourceTransferBuilder::new(),
        &mut request_wire,
        0,
    )
    .map_err(|error| format!("open_object submit: {error:?}"))?;
    if !completion_ready(invocation.get()).await {
        return Err("open_object did not complete".into());
    }
    let mut response_wire = [0_u8; 64];
    let (response, mut resources) =
        firmware_root::take_open_object(&mut invocation, &mut response_wire)
            .map_err(|error| format!("open_object take: {error:?}"))?;
    let handle = resources
        .take(response.object)
        .ok_or_else(|| "open_object omitted the table resource".to_string())?;
    Ok(MemoryObject::from_owned(handle))
}

mod bootstrap;

pub async fn run(context: Context) -> i64 {
    bootstrap::run(context).await
}
