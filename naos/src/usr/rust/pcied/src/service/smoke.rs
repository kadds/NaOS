use super::*;

#[cfg(feature = "pci-msi-route-smoke")]
const PCI_DRIVER_IMAGE_URI: &str = "naos://service/boot/module/pdrv-edud";

#[cfg(feature = "pci-msi-route-smoke")]
const PCI_DRIVER_PROFILE_BAR0: u32 = 1 << 0;
#[cfg(feature = "pci-msi-route-smoke")]
const QEMU_EDU_DRIVER_BINDING: DriverBinding = DriverBinding {
    compatible_id: DriverCompatibleId::Text("QEMU0002"),
    image_uri: PCI_DRIVER_IMAGE_URI,
    executable_path: "/edud",
    profile_rights: PCI_DRIVER_PROFILE_READ_CONFIG
        | PCI_DRIVER_PROFILE_DMA_DOMAIN
        | PCI_DRIVER_PROFILE_ROUTE_INTERRUPT,
    profile_bar_mask: PCI_DRIVER_PROFILE_BAR0,
};

#[cfg(feature = "pci-msi-route-smoke")]
fn pci_driver_service_uri(address: PciAddress) -> String {
    format!(
        "naos://service/pci/driver/{:04x}-{:02x}-{:02x}-{:x}/probe",
        address.segment(),
        address.bus(),
        address.device(),
        address.function()
    )
}

#[cfg(feature = "pci-msi-route-smoke")]
#[derive(Clone, Copy)]
struct MsixTableLayout {
    table_size: u16,
    table_bar: u8,
    table_offset: u32,
    pba_bar: u8,
    pba_offset: u32,
}

#[cfg(feature = "pci-msi-route-smoke")]
use crate::topology::{
    PciFunctionIdentitySnapshot, dma_fault_batch_matches_lineage, interrupt_event_matches_lineage,
};
#[cfg(feature = "pci-msi-route-smoke")]
async fn read_config(
    endpoint: &ProtocolClientEndpoint,
    offset: u16,
    width: u8,
) -> Result<u32, String> {
    crate::virtio_pci::read_function_config(endpoint, offset, width)
        .await
        .map_err(|error| format!("PciFunction.read_config: {error:?}"))
}

#[cfg(feature = "pci-msi-route-smoke")]
async fn read_msix_table_layout(
    endpoint: &ProtocolClientEndpoint,
) -> Result<Option<MsixTableLayout>, String> {
    const CAPABILITIES_LIST: u32 = 1 << 20;
    const MSIX_CAPABILITY_ID: u8 = 0x11;
    const MAX_CAPABILITY_HOPS: usize = 48;

    if read_config(endpoint, 0x04, 4).await? & CAPABILITIES_LIST == 0 {
        return Ok(None);
    }
    let mut offset = read_config(endpoint, 0x34, 1).await? as u8;
    let mut visited = [false; 64];
    for _ in 0..MAX_CAPABILITY_HOPS {
        if offset == 0 {
            return Ok(None);
        }
        if !(0x40..=0xfc).contains(&offset) || offset & 3 != 0 {
            return Err(format!("malformed PCI capability pointer {offset:#04x}"));
        }
        let slot = usize::from(offset / 4);
        if visited[slot] {
            return Err(format!("PCI capability list loops at {offset:#04x}"));
        }
        visited[slot] = true;

        let header = read_config(endpoint, u16::from(offset), 4).await?;
        let capability_id = header as u8;
        let next = (header >> 8) as u8;
        if capability_id == MSIX_CAPABILITY_ID {
            let control = (header >> 16) as u16;
            let table = read_config(endpoint, u16::from(offset) + 4, 4).await?;
            let pba = read_config(endpoint, u16::from(offset) + 8, 4).await?;
            return Ok(Some(MsixTableLayout {
                table_size: ((control & 0x07ff) + 1),
                table_bar: (table & 0x7) as u8,
                table_offset: table & !0x7,
                pba_bar: (pba & 0x7) as u8,
                pba_offset: pba & !0x7,
            }));
        }
        offset = next;
    }
    if offset == 0 {
        Ok(None)
    } else {
        Err("PCI capability list exceeds the bounded MSI-X smoke walk".into())
    }
}

#[cfg(feature = "pci-msi-route-smoke")]
async fn read_function_identity(
    endpoint: &ProtocolClientEndpoint,
) -> Result<PciFunctionIdentitySnapshot, String> {
    let request = pci_function::get_identity_request {};
    let mut request_wire = [];
    let mut invocation = pci_function::submit_get_identity(
        endpoint,
        &request,
        ResourceTransferBuilder::new(),
        &mut request_wire,
        0,
    )
    .map_err(|error| format!("PciFunction.get_identity submit: {error:?}"))?;
    wait_for_call(invocation.get(), "PciFunction.get_identity").await?;
    let mut response_wire = [0_u8; pci_function::GET_IDENTITY_RESPONSE_HEADER_BYTES + 32];
    pci_function::take_get_identity(&mut invocation, &mut response_wire)
        .map(|response| PciFunctionIdentitySnapshot {
            firmware_version: response.firmware_version.to_owned(),
            value: response.value,
        })
        .map_err(|error| format!("PciFunction.get_identity take: {error:?}"))
}

#[cfg(feature = "pci-msi-route-smoke")]
async fn expect_misaligned_config_read_rejected(
    endpoint: &ProtocolClientEndpoint,
) -> Result<(), String> {
    let request = pci_function::read_config_request {
        offset: 1,
        width: 2,
    };
    let mut request_wire = [0_u8; pci_function::READ_CONFIG_REQUEST_HEADER_BYTES];
    let mut invocation = pci_function::submit_read_config(
        endpoint,
        &request,
        ResourceTransferBuilder::new(),
        &mut request_wire,
        0,
    )
    .map_err(|error| format!("misaligned PciFunction.read_config submit: {error:?}"))?;
    wait_for_call(invocation.get(), "misaligned PciFunction.read_config").await?;
    let mut response_wire = [0_u8; pci_function::READ_CONFIG_RESPONSE_HEADER_BYTES];
    match pci_function::take_read_config(&mut invocation, &mut response_wire) {
        Err(CallError::Outcome {
            protocol_error: -22,
            ..
        }) => Ok(()),
        Err(error) => Err(format!(
            "misaligned config read returned unexpected error: {error:?}"
        )),
        Ok(response) => Err(format!(
            "misaligned config read was accepted with value={:08x}",
            response.value
        )),
    }
}

#[cfg(feature = "pci-msi-route-smoke")]
async fn open_bar_mapping(
    endpoint: &ProtocolClientEndpoint,
    index: u32,
    expected_length: Option<u64>,
    device_name: &str,
) -> Result<(MemoryObject, MappedMemory, usize), String> {
    let request = pci_function::open_bar_request { index };
    let mut request_wire = [0_u8; pci_function::OPEN_BAR_REQUEST_HEADER_BYTES];
    let mut invocation = pci_function::submit_open_bar(
        endpoint,
        &request,
        ResourceTransferBuilder::new(),
        &mut request_wire,
        0,
    )
    .map_err(|error| format!("open_bar submit: {error:?}"))?;
    wait_for_call(invocation.get(), "open_bar").await?;
    let mut response_wire = [0_u8; pci_function::OPEN_BAR_RESPONSE_HEADER_BYTES];
    let (response, mut resources) =
        pci_function::take_open_bar(&mut invocation, &mut response_wire)
            .map_err(|error| format!("open_bar take: {error:?}"))?;
    let length = usize::try_from(response.length)
        .map_err(|_| "open_bar length does not fit the address space")?;
    if length == 0 || expected_length.is_some_and(|expected| response.length != expected) {
        return Err(format!(
            "{device_name} returned an unexpected BAR{index} length: {} (expected {expected_length:?})",
            response.length,
        ));
    }
    let handle = resources
        .take(response.mapping)
        .ok_or_else(|| "open_bar omitted the MemoryObject mapping".to_string())?;
    let object = MemoryObject::from_owned(handle);
    if object.size() != Some(response.length) {
        return Err("open_bar MemoryObject size does not match the response".into());
    }
    let mapping = object
        .map_all(
            servicekit::sys::MEMORY_MAP_READ
                | servicekit::sys::MEMORY_MAP_WRITE
                | servicekit::sys::MEMORY_MAP_SHARED,
        )
        .map_err(|error| format!("map {device_name} BAR{index}: {error:?}"))?;
    Ok((object, mapping, length))
}

#[cfg(feature = "pci-msi-route-smoke")]
async fn open_bar(
    endpoint: &ProtocolClientEndpoint,
    index: u32,
) -> Result<(MemoryObject, MappedMemory, usize), String> {
    open_bar_mapping(endpoint, index, Some(0x0010_0000), "QEMU EDU").await
}

#[cfg(feature = "pci-msi-route-smoke")]
fn rocker_mmio_read_u32(
    mapping: &MappedMemory,
    length: usize,
    offset: usize,
) -> Result<u32, String> {
    let end = offset
        .checked_add(core::mem::size_of::<u32>())
        .ok_or_else(|| "Rocker MMIO read offset overflow".to_string())?;
    if end > length || offset % core::mem::align_of::<u32>() != 0 {
        return Err(format!("Rocker MMIO read outside BAR0: offset={offset:#x}"));
    }
    // SAFETY: open_bar_mapping owns the BAR0 mapping, and bounds/alignment
    // above prove this volatile dword is within the advertised device region.
    let register = unsafe { mapping.as_ptr().add(offset).cast::<u32>() };
    Ok(u32::from_le(unsafe { register.read_volatile() }))
}

#[cfg(feature = "pci-msi-route-smoke")]
fn rocker_mmio_write_u32(
    mapping: &mut MappedMemory,
    length: usize,
    offset: usize,
    value: u32,
) -> Result<(), String> {
    let end = offset
        .checked_add(core::mem::size_of::<u32>())
        .ok_or_else(|| "Rocker MMIO write offset overflow".to_string())?;
    if end > length || offset % core::mem::align_of::<u32>() != 0 {
        return Err(format!(
            "Rocker MMIO write outside BAR0: offset={offset:#x}"
        ));
    }
    // SAFETY: open_bar_mapping owns a writable BAR0 mapping, and
    // bounds/alignment above prove this volatile dword is within that region.
    let register = unsafe { mapping.as_mut_ptr().add(offset).cast::<u32>() };
    unsafe { register.write_volatile(value.to_le()) };
    Ok(())
}

#[cfg(feature = "pci-msi-route-smoke")]
async fn expect_driver_bar_denied(
    endpoint: &ProtocolClientEndpoint,
    index: u32,
) -> Result<(), String> {
    let request = pci_function::open_bar_request { index };
    let mut request_wire = [0_u8; pci_function::OPEN_BAR_REQUEST_HEADER_BYTES];
    let mut invocation = pci_function::submit_open_bar(
        endpoint,
        &request,
        ResourceTransferBuilder::new(),
        &mut request_wire,
        0,
    )
    .map_err(|error| format!("restricted PciFunction.open_bar submit: {error:?}"))?;
    wait_for_call(invocation.get(), "restricted PciFunction.open_bar").await?;
    let mut response_wire = [0_u8; pci_function::OPEN_BAR_RESPONSE_HEADER_BYTES];
    match pci_function::take_open_bar(&mut invocation, &mut response_wire) {
        Err(CallError::Outcome {
            protocol_error: -13,
            ..
        }) => Ok(()),
        Err(CallError::Status(status)) if status == servicekit::sys::STATUS_ACCESS_DENIED => Ok(()),
        Err(error) => Err(format!("restricted open_bar({index}) returned {error:?}")),
        Ok((_response, _resources)) => Err(format!(
            "driver-scoped PciFunction unexpectedly opened BAR {index}"
        )),
    }
}

#[cfg(feature = "pci-msi-route-smoke")]
async fn publish_private_driver_probe(
    context: &Context,
    address: PciAddress,
) -> Result<(), String> {
    let service_uri = pci_driver_service_uri(address);
    let directory_info = naos_idl::handle_info(context.service_directory_handle())
        .map_err(|error| format!("inspect private driver ServiceDirectory: {error:?}"))?;
    if directory_info.protocol_rights & servicekit::sys::SERVICE_DIRECTORY_RIGHT_LOCAL_REGISTER == 0
    {
        return Err(format!(
            "private driver ServiceDirectory lacks local-register right: {:#x}",
            directory_info.protocol_rights
        ));
    }
    let service_object = servicekit::memory::MemoryObject::new(4096)
        .map_err(|error| format!("create private driver probe resource: {error:?}"))?;
    service_object
        .write_at(0, b"driver-private-directory")
        .map_err(|error| format!("fill private driver probe resource: {error:?}"))?;
    servicekit::register_service_async(
        context.service_directory_handle(),
        &service_uri,
        service_object.into_handle(),
    )
    .await
    .map_err(|error| {
        format!(
            "register private driver service {service_uri} with rights={:#x}: {error:?}",
            directory_info.protocol_rights
        )
    })?;
    log::info!("QEMU EDU child published private data resource address={address}");
    Ok(())
}

#[cfg(feature = "pci-msi-route-smoke")]
async fn run_granted_pci_driver(context: Context, capability: OwnedHandle) -> Result<(), String> {
    let endpoint = unsafe { ProtocolClientEndpoint::from_raw(capability.into_raw()) };
    let identity = read_function_identity(&endpoint).await?;
    let lineage = GenerationLineage::from(&identity);
    let segment = u16::try_from(identity.segment)
        .map_err(|_| "granted PCI segment does not fit in a PCI address")?;
    let bus =
        u8::try_from(identity.bus).map_err(|_| "granted PCI bus does not fit in a PCI address")?;
    let device = u8::try_from(identity.device)
        .map_err(|_| "granted PCI device number does not fit in a PCI address")?;
    let function = u8::try_from(identity.function)
        .map_err(|_| "granted PCI function number does not fit in a PCI address")?;
    let address = PciAddress::new(segment, bus, device, function);
    expect_driver_bar_denied(&endpoint, 1).await?;

    match servicekit::resolve_resource(context.service_directory_handle(), PCI_DISCOVERY_URI) {
        Err(CallError::Outcome {
            protocol_error: -13,
            ..
        }) => {}
        Err(CallError::Status(status)) if status == servicekit::sys::STATUS_ACCESS_DENIED => {}
        Err(error) => {
            return Err(format!(
                "driver ServiceDirectory denied discovery: {error:?}"
            ));
        }
        Ok(resource) => {
            drop(resource);
            return Err("driver-scoped ServiceDirectory exposed PCI discovery".into());
        }
    }
    match servicekit::resolve_resource(context.service_directory_handle(), PCI_DRIVER_IMAGE_URI) {
        Err(CallError::Outcome {
            protocol_error: -13,
            ..
        }) => {}
        Err(CallError::Status(status)) if status == servicekit::sys::STATUS_ACCESS_DENIED => {}
        Err(CallError::Status(status)) if status == servicekit::sys::STATUS_WOULD_BLOCK => {}
        Err(error) => {
            return Err(format!(
                "driver ServiceDirectory image locator denial returned {error:?}"
            ));
        }
        Ok(image) => {
            drop(image);
            return Err("driver-scoped ServiceDirectory exposed its boot image locator".into());
        }
    }
    log::info!("child ServiceDirectory cannot resolve driver image locator");

    // Publish before the deliberately slower hardware smoke so the manager can
    // verify private-directory isolation while the child keeps exercising DMA.
    publish_private_driver_probe(&context, address).await?;
    exercise_qemu_edu_function(&endpoint, address, &lineage).await?;
    log::info!("QEMU EDU child owns DMA/IRQ; BAR1 denied address={address}");
    Ok(())
}

#[cfg(feature = "pci-msi-route-smoke")]
async fn verify_private_driver_service(
    context: Context,
    address: PciAddress,
    pid: u64,
    directory: &OwnedHandle,
) -> Result<(), String> {
    let uri = pci_driver_service_uri(address);
    let endpoint =
        servicekit::resolve_until(directory.get(), &uri, core::time::Duration::from_secs(2))
            .await
            .map_err(|error| format!("resolve child PCI service {uri}: {error:?}"))?;
    let info = naos_idl::handle_info(endpoint.get())
        .map_err(|error| format!("inspect child PCI service {uri}: {error:?}"))?;
    if info.binding != servicekit::sys::BINDING_MEMORY_OBJECT
        || info.scope != naos_idl::memory_object::PROTOCOL_SCOPE
        || info.revision != naos_idl::memory_object::PROTOCOL_REVISION
        || info.protocol_uuid.bytes != naos_idl::memory_object::PROTOCOL_UUID
    {
        return Err(format!(
            "child PCI service {uri} has an unexpected MemoryObject binding"
        ));
    }

    let memory = servicekit::memory::MemoryObject::from_owned(unsafe {
        OwnedHandle::from_raw(endpoint.into_raw())
    });
    let expected = b"driver-private-directory";
    let mut observed = [0_u8; 24];
    memory
        .read_at(0, &mut observed)
        .map_err(|error| format!("read child PCI resource {uri}: {error:?}"))?;
    if observed != expected[..] {
        return Err(format!("child PCI resource {uri} data did not match"));
    }

    match servicekit::resolve_resource(context.service_directory_handle(), &uri) {
        Err(CallError::Outcome {
            protocol_error: -13 | 13,
            ..
        })
        | Err(CallError::Status(servicekit::sys::STATUS_ACCESS_DENIED)) => {}
        Ok(resource) => {
            drop(resource);
            return Err(format!(
                "child PCI service escaped its private directory: {uri}"
            ));
        }
        Err(error) => return Err(format!("global PCI service lookup {uri}: {error:?}")),
    }

    log::info!("PCI driver child data resource stayed private address={address} pid={pid}");
    Ok(())
}

#[cfg(feature = "pci-msi-route-smoke")]
async fn wait_for_pci_driver(context: Context, driver: StartedPciDriver) -> Result<(), String> {
    let StartedPciDriver {
        address,
        pid,
        process,
        manager_directory,
    } = driver;
    verify_private_driver_service(context, address, pid, &manager_directory).await?;
    let request = process_idl::wait_request { flags: 0 };
    let mut request_wire = [0_u8; process_idl::WAIT_REQUEST_HEADER_BYTES];
    let mut invocation = process_idl::submit_wait(
        &process,
        &request,
        ResourceTransferBuilder::new(),
        &mut request_wire,
        0,
    )
    .map_err(|error| format!("granted PCI driver {address} Process.wait submit: {error:?}"))?;
    wait_for_call(invocation.get(), "granted PCI driver Process.wait").await?;
    let mut response_wire = [0_u8; process_idl::WAIT_RESPONSE_HEADER_BYTES];
    let response = process_idl::take_wait(&mut invocation, &mut response_wire)
        .map_err(|error| format!("granted PCI driver {address} Process.wait take: {error:?}"))?;
    if response.status != 0 {
        return Err(format!(
            "granted PCI driver {address} pid={pid} exited with status {}",
            response.status
        ));
    }
    log::info!("QEMU EDU granted driver process exited address={address} pid={pid} status=0");
    Ok(())
}

#[cfg(feature = "pci-msi-route-smoke")]
fn edu_mmio_read_u32(mapping: &MappedMemory, length: usize, offset: usize) -> Result<u32, String> {
    let end = offset
        .checked_add(core::mem::size_of::<u32>())
        .ok_or_else(|| "EDU MMIO read offset overflow".to_string())?;
    if end > length || offset % core::mem::align_of::<u32>() != 0 {
        return Err(format!("EDU MMIO read outside BAR0: offset={offset:#x}"));
    }
    // SAFETY: open_bar returned a live, page-mapped BAR0 view; the dword
    // bounds and alignment are checked above. Device registers require a
    // volatile access rather than a Rust slice load.
    let register = unsafe { mapping.as_ptr().add(offset).cast::<u32>() };
    Ok(u32::from_le(unsafe { register.read_volatile() }))
}

#[cfg(feature = "pci-msi-route-smoke")]
fn edu_mmio_write_u32(
    mapping: &mut MappedMemory,
    length: usize,
    offset: usize,
    value: u32,
) -> Result<(), String> {
    let end = offset
        .checked_add(core::mem::size_of::<u32>())
        .ok_or_else(|| "EDU MMIO write offset overflow".to_string())?;
    if end > length || offset % core::mem::align_of::<u32>() != 0 {
        return Err(format!("EDU MMIO write outside BAR0: offset={offset:#x}"));
    }
    // SAFETY: open_bar returned a live writable BAR0 mapping and the aligned
    // dword is within its advertised length. Device registers require a
    // volatile access rather than a Rust slice store.
    let register = unsafe { mapping.as_mut_ptr().add(offset).cast::<u32>() };
    unsafe { register.write_volatile(value.to_le()) };
    Ok(())
}

#[cfg(feature = "pci-msi-route-smoke")]
fn edu_mmio_write_u64(
    mapping: &mut MappedMemory,
    length: usize,
    offset: usize,
    value: u64,
) -> Result<(), String> {
    let end = offset
        .checked_add(core::mem::size_of::<u64>())
        .ok_or_else(|| "EDU MMIO write offset overflow".to_string())?;
    if end > length || offset % core::mem::align_of::<u64>() != 0 {
        return Err(format!(
            "QEMU EDU DMA register outside BAR0: offset={offset:#x}"
        ));
    }
    // SAFETY: open_bar returned a live writable BAR0 mapping and the aligned
    // qword is within its advertised length. DMA registers require volatile
    // access rather than a Rust slice store.
    let register = unsafe { mapping.as_mut_ptr().add(offset).cast::<u64>() };
    unsafe { register.write_volatile(value.to_le()) };
    Ok(())
}

#[cfg(feature = "pci-msi-route-smoke")]
async fn route_interrupt(
    endpoint: &ProtocolClientEndpoint,
) -> Result<ProtocolClientEndpoint, String> {
    let request = pci_function::route_interrupt_request {};
    let mut request_wire = [];
    let mut invocation = pci_function::submit_route_interrupt(
        endpoint,
        &request,
        ResourceTransferBuilder::new(),
        &mut request_wire,
        0,
    )
    .map_err(|error| format!("route_interrupt submit: {error:?}"))?;
    wait_for_call(invocation.get(), "route_interrupt").await?;
    let mut response_wire = [0_u8; pci_function::ROUTE_INTERRUPT_RESPONSE_HEADER_BYTES];
    let (response, mut resources) =
        pci_function::take_route_interrupt(&mut invocation, &mut response_wire)
            .map_err(|error| format!("route_interrupt take: {error:?}"))?;
    let handle = resources
        .take(response.source)
        .ok_or_else(|| "route_interrupt omitted InterruptSource capability".to_string())?;
    // SAFETY: NaoIDL validated the kernel-transferred resource's protocol
    // scope and its move disposition before returning the owned handle.
    Ok(unsafe { ProtocolClientEndpoint::from_raw(handle.into_raw()) })
}

#[cfg(feature = "pci-msi-route-smoke")]
async fn bind_interrupt_source(endpoint: &ProtocolClientEndpoint) -> Result<(), String> {
    let mut request_wire = [];
    let mut invocation = interrupt_source::submit_bind(
        endpoint,
        &interrupt_source::bind_request {},
        ResourceTransferBuilder::new(),
        &mut request_wire,
        0,
    )
    .map_err(|error| format!("InterruptSource.bind submit: {error:?}"))?;
    wait_for_call(invocation.get(), "InterruptSource.bind").await?;
    let mut response_wire = [];
    interrupt_source::take_bind(&mut invocation, &mut response_wire)
        .map_err(|error| format!("InterruptSource.bind take: {error:?}"))?;
    Ok(())
}

#[cfg(feature = "pci-msi-route-smoke")]
async fn wait_interrupt_source(
    endpoint: &ProtocolClientEndpoint,
    lineage: &GenerationLineage,
) -> Result<interrupt_source::InterruptEvent, String> {
    let readiness = servicekit::wait_ready_async(&[endpoint.get()], Some(5_000_000))
        .await
        .map_err(|status| format!("InterruptSource event readiness failed: {status}"))?;
    if !readiness.readable {
        return Err(format!(
            "InterruptSource did not become readable (closed={} error={})",
            readiness.read_closed || readiness.write_closed,
            readiness.error
        ));
    }
    let mut request_wire = [];
    let mut invocation = interrupt_source::submit_wait(
        endpoint,
        &interrupt_source::wait_request {},
        ResourceTransferBuilder::new(),
        &mut request_wire,
        0,
    )
    .map_err(|error| format!("InterruptSource.wait submit: {error:?}"))?;
    wait_for_call(invocation.get(), "InterruptSource.wait").await?;
    let mut response_wire = [0_u8; interrupt_source::WAIT_RESPONSE_HEADER_BYTES + 32];
    let response = interrupt_source::take_wait(&mut invocation, &mut response_wire)
        .map_err(|error| format!("InterruptSource.wait take: {error:?}"))?;
    if !interrupt_event_matches_lineage(&response.event, response.firmware_version, lineage) {
        return Err("InterruptSource.wait returned a stale event lineage".into());
    }
    Ok(response.event)
}

#[cfg(feature = "pci-msi-route-smoke")]
async fn ack_interrupt_source(
    endpoint: &ProtocolClientEndpoint,
    sequence: u64,
    lineage: &GenerationLineage,
) -> Result<(), String> {
    let request = interrupt_source::ack_request { sequence };
    let mut request_wire = [0_u8; interrupt_source::ACK_REQUEST_HEADER_BYTES];
    let mut invocation = interrupt_source::submit_ack(
        endpoint,
        &request,
        ResourceTransferBuilder::new(),
        &mut request_wire,
        0,
    )
    .map_err(|error| format!("InterruptSource.ack submit: {error:?}"))?;
    wait_for_call(invocation.get(), "InterruptSource.ack").await?;
    let mut response_wire = [0_u8; interrupt_source::ACK_RESPONSE_HEADER_BYTES + 32];
    let response = interrupt_source::take_ack(&mut invocation, &mut response_wire)
        .map_err(|error| format!("InterruptSource.ack take: {error:?}"))?;
    if !interrupt_event_matches_lineage(&response.event, response.firmware_version, lineage) {
        return Err("InterruptSource.ack returned a stale event lineage".into());
    }
    if response.event.sequence < sequence || response.event.flags & 1 != 0 {
        return Err(format!(
            "InterruptSource.ack did not drain the event sequence: requested={sequence} current={} flags={:#x}",
            response.event.sequence, response.event.flags
        ));
    }
    Ok(())
}

#[cfg(feature = "pci-msi-route-smoke")]
async fn create_dma_domain(
    endpoint: &ProtocolClientEndpoint,
) -> Result<(ProtocolClientEndpoint, ProtocolClientEndpoint), String> {
    let request = pci_function::create_dma_domain_request {};
    let mut request_wire = [];
    let mut invocation = pci_function::submit_create_dma_domain(
        endpoint,
        &request,
        ResourceTransferBuilder::new(),
        &mut request_wire,
        0,
    )
    .map_err(|error| format!("create_dma_domain submit: {error:?}"))?;
    wait_for_call(invocation.get(), "create_dma_domain").await?;
    let mut response_wire = [0_u8; pci_function::CREATE_DMA_DOMAIN_RESPONSE_HEADER_BYTES];
    let (response, mut resources) =
        pci_function::take_create_dma_domain(&mut invocation, &mut response_wire)
            .map_err(|error| format!("create_dma_domain take: {error:?}"))?;
    let domain_handle = resources
        .take(response.domain)
        .ok_or_else(|| "create_dma_domain omitted PciDmaDomain capability".to_string())?;
    let fault_handle = resources
        .take(response.faults)
        .ok_or_else(|| "create_dma_domain omitted PciDmaFaultSource capability".to_string())?;
    // SAFETY: NaoIDL validated this kernel-transferred resource against the
    // PciDmaDomain protocol scope before returning the owned handle.
    let domain = unsafe { ProtocolClientEndpoint::from_raw(domain_handle.into_raw()) };
    // SAFETY: NaoIDL validated this second kernel-transferred resource against
    // the PciDmaFaultSource protocol scope before returning the owned handle.
    let faults = unsafe { ProtocolClientEndpoint::from_raw(fault_handle.into_raw()) };
    Ok((domain, faults))
}

#[cfg(feature = "pci-msi-route-smoke")]
async fn take_dma_faults(
    endpoint: &ProtocolClientEndpoint,
    limit: u32,
    lineage: &GenerationLineage,
) -> Result<OwnedDmaFaultBatch, String> {
    let request = pci_dma_fault_source::take_request { limit };
    let mut request_wire = [0_u8; pci_dma_fault_source::TAKE_REQUEST_HEADER_BYTES];
    let mut invocation = pci_dma_fault_source::submit_take(
        endpoint,
        &request,
        ResourceTransferBuilder::new(),
        &mut request_wire,
        0,
    )
    .map_err(|error| format!("PciDmaFaultSource.take submit: {error:?}"))?;
    wait_for_call(invocation.get(), "PciDmaFaultSource.take").await?;
    let mut response_wire = [0_u8; pci_dma_fault_source::TAKE_RESPONSE_HEADER_BYTES + 16 * 12 + 32];
    let response = pci_dma_fault_source::take_take(&mut invocation, &mut response_wire)
        .map_err(|error| format!("PciDmaFaultSource.take take: {error:?}"))?;
    if !dma_fault_batch_matches_lineage(&response, lineage) {
        return Err("PciDmaFaultSource returned a stale batch lineage".into());
    }
    if response.count as usize != response.records.len() || response.unit_overflow > 1 {
        return Err(format!(
            "PciDmaFaultSource returned inconsistent batch: count={} records={} unit_overflow={}",
            response.count,
            response.records.len(),
            response.unit_overflow
        ));
    }
    Ok(OwnedDmaFaultBatch {
        sequence: response.sequence,
        count: response.count,
        dropped_records: response.dropped_records,
        unit_overflow: response.unit_overflow,
        records: response.records,
    })
}

#[cfg(feature = "pci-msi-route-smoke")]
struct OwnedDmaFaultBatch {
    sequence: u64,
    count: u32,
    dropped_records: u32,
    unit_overflow: u8,
    records: Vec<pci_dma_fault_source::PciDmaFaultRecord>,
}

#[cfg(feature = "pci-msi-route-smoke")]
fn denied_write_batch_matches(
    batch: &OwnedDmaFaultBatch,
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

#[cfg(feature = "pci-msi-route-smoke")]
async fn set_dma_bus_master(
    endpoint: &ProtocolClientEndpoint,
    enabled: bool,
) -> Result<(), String> {
    let request = pci_dma_domain::set_bus_master_request {
        enabled: u8::from(enabled),
    };
    let mut request_wire = [0_u8; pci_dma_domain::SET_BUS_MASTER_REQUEST_HEADER_BYTES];
    let mut invocation = pci_dma_domain::submit_set_bus_master(
        endpoint,
        &request,
        ResourceTransferBuilder::new(),
        &mut request_wire,
        0,
    )
    .map_err(|error| format!("PciDmaDomain.set_bus_master submit: {error:?}"))?;
    wait_for_call(invocation.get(), "PciDmaDomain.set_bus_master").await?;
    let mut response_wire = [];
    pci_dma_domain::take_set_bus_master(&mut invocation, &mut response_wire)
        .map_err(|error| format!("PciDmaDomain.set_bus_master take: {error:?}"))?;
    Ok(())
}

#[cfg(feature = "pci-msi-route-smoke")]
async fn close_dma_domain(endpoint: &ProtocolClientEndpoint) -> Result<(), String> {
    let request = pci_dma_domain::close_request {};
    let mut request_wire = [];
    let mut invocation = pci_dma_domain::submit_close(
        endpoint,
        &request,
        ResourceTransferBuilder::new(),
        &mut request_wire,
        0,
    )
    .map_err(|error| format!("PciDmaDomain.close submit: {error:?}"))?;
    wait_for_call(invocation.get(), "PciDmaDomain.close").await?;
    let mut response_wire = [];
    pci_dma_domain::take_close(&mut invocation, &mut response_wire)
        .map_err(|error| format!("PciDmaDomain.close take: {error:?}"))?;
    Ok(())
}

#[cfg(feature = "pci-msi-route-smoke")]
async fn map_dma_page(
    endpoint: &ProtocolClientEndpoint,
    backing: &MemoryObject,
    iova: u64,
    direction: u32,
) -> Result<(), String> {
    let mut resources = ResourceTransferBuilder::new();
    let backing_slot = resources
        .push_duplicate(backing.as_handle())
        .map_err(|error| format!("PciDmaDomain.map_page resource: {error:?}"))?;
    let request = pci_dma_domain::map_page_request {
        iova,
        object_offset: 0,
        direction,
        backing: backing_slot,
    };
    let mut request_wire = [0_u8; pci_dma_domain::MAP_PAGE_REQUEST_HEADER_BYTES];
    let mut invocation =
        pci_dma_domain::submit_map_page(endpoint, &request, resources, &mut request_wire, 0)
            .map_err(|error| format!("PciDmaDomain.map_page submit: {error:?}"))?;
    wait_for_call(invocation.get(), "PciDmaDomain.map_page").await?;
    let mut response_wire = [];
    pci_dma_domain::take_map_page(&mut invocation, &mut response_wire)
        .map_err(|error| format!("PciDmaDomain.map_page take: {error:?}"))?;
    Ok(())
}

#[cfg(feature = "pci-msi-route-smoke")]
async fn unmap_dma_page(endpoint: &ProtocolClientEndpoint, iova: u64) -> Result<(), String> {
    let request = pci_dma_domain::unmap_page_request { iova };
    let mut request_wire = [0_u8; pci_dma_domain::UNMAP_PAGE_REQUEST_HEADER_BYTES];
    let mut invocation = pci_dma_domain::submit_unmap_page(
        endpoint,
        &request,
        ResourceTransferBuilder::new(),
        &mut request_wire,
        0,
    )
    .map_err(|error| format!("PciDmaDomain.unmap_page submit: {error:?}"))?;
    wait_for_call(invocation.get(), "PciDmaDomain.unmap_page").await?;
    let mut response_wire = [];
    pci_dma_domain::take_unmap_page(&mut invocation, &mut response_wire)
        .map_err(|error| format!("PciDmaDomain.unmap_page take: {error:?}"))?;
    Ok(())
}

#[cfg(feature = "pci-msi-route-smoke")]
pub(super) async fn exercise_qemu_edu_route(
    context: &Context,
    topology_endpoint: &super::PlatformPciTopologyClient,
    tree: &PciFabricTree,
) -> Result<(), String> {
    let catalog = [QEMU_EDU_DRIVER_BINDING];
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
    let plan = plan_driver_bindings_for_functions(&identifiers, &catalog)
        .map_err(|error| format!("PCI driver catalog preflight rejected its policy: {error:?}"))?;
    if plan.is_empty() {
        log::info!("QEMU EDU driver smoke skipped: no compatible test function on this PCI fabric");
        return Ok(());
    }
    log::info!(
        "PCI driver manager preflight complete matches={} functions={}",
        plan.len(),
        functions.len()
    );
    let mut prepared_drivers = Vec::new();
    for group in group_driver_plan_by_image(plan) {
        let image = servicekit::boot::read_module(context, group.image_uri)
            .map_err(|error| format!("read PCI driver image {}: {error:?}", group.image_uri))?;
        for assignment in group.assignments {
            let (root, function) = functions[assignment.function_index];
            let function_capability =
                super::open_bound_function(topology_endpoint, root, function).await?;
            prepared_drivers.push(
                grant_pci_driver(function, &function_capability, assignment.driver, &image).await?,
            );
        }
    }

    // Keep every matched child deferred until all process preparation and
    // per-function grants have succeeded. A later image or grant failure then
    // drops only unstarted children and cannot leave an earlier driver
    // operating with an incomplete device assignment plan.
    log::info!(
        "PCI driver grants staged before child start count={}",
        prepared_drivers.len()
    );
    let mut started_drivers = Vec::with_capacity(prepared_drivers.len());
    let mut first_error = None;
    for prepared in prepared_drivers {
        match start_pci_driver(context, prepared).await {
            Ok(driver) => started_drivers.push(driver),
            Err(error) => {
                log::error!("PCI driver child start failed: {error}");
                if first_error.is_none() {
                    first_error = Some(error);
                }
            }
        }
    }

    // Confirm every prepared child has started before awaiting exits so one
    // driver cannot block the independent startup of the remaining plan.
    let started_count = started_drivers.len();
    log::info!(
        "PCI driver start phase complete started={} awaiting child exits",
        started_count
    );
    let waiters: Vec<_> = started_drivers
        .into_iter()
        .map(|driver| tokio::spawn(wait_for_pci_driver(*context, driver)))
        .collect();
    for waiter in waiters {
        match waiter.await {
            Ok(Ok(())) => {}
            Ok(Err(error)) => {
                log::error!("PCI driver child outcome failed: {error}");
                if first_error.is_none() {
                    first_error = Some(error);
                }
            }
            Err(error) => {
                let error = format!("PCI driver wait task failed: {error}");
                log::error!("{error}");
                if first_error.is_none() {
                    first_error = Some(error);
                }
            }
        }
    }
    if let Some(error) = first_error {
        return Err(format!("PCI driver launch plan did not complete: {error}"));
    }
    Ok(())
}

#[cfg(feature = "pci-msi-route-smoke")]
pub(super) async fn exercise_qemu_rocker_msix_route(
    topology_endpoint: &super::PlatformPciTopologyClient,
    tree: &PciFabricTree,
) -> Result<(), String> {
    const ROCKER_VENDOR_ID: u16 = 0x1b36;
    const ROCKER_DEVICE_ID: u16 = 0x0006;
    const ROCKER_BAR0_BYTES: u64 = 0x2000;
    const ROCKER_TEST_IRQ_OFFSET: usize = 0x20;
    let lineage = GenerationLineage::from(&tree.generation);

    let Some((root, function)) = tree.roots.iter().find_map(|root| {
        root.functions
            .iter()
            .find(|function| {
                function.vendor_id == ROCKER_VENDOR_ID && function.device_id == ROCKER_DEVICE_ID
            })
            .map(|function| (root, function))
    }) else {
        log::info!("QEMU Rocker MSI-X smoke skipped: no Rocker test function on this PCI fabric");
        return Ok(());
    };
    let function_capability = super::open_bound_function(topology_endpoint, root, function).await?;

    let Some(layout) = read_msix_table_layout(&function_capability).await? else {
        return Err("QEMU Rocker test function did not expose an MSI-X capability".into());
    };
    if !(1..=256).contains(&layout.table_size)
        || layout.table_bar != 1
        || layout.table_offset != 0
        || layout.pba_bar != 1
        || layout.pba_offset != 0x1000
    {
        return Err(format!(
            "QEMU Rocker MSI-X layout changed: vectors={} table=BAR{}+{:#x} PBA=BAR{}+{:#x}",
            layout.table_size,
            layout.table_bar,
            layout.table_offset,
            layout.pba_bar,
            layout.pba_offset
        ));
    }
    log::info!(
        "QEMU Rocker MSI-X capability verified address={} vectors={} table=BAR{} pba=BAR{}",
        function.address,
        layout.table_size,
        layout.table_bar,
        layout.pba_bar
    );

    expect_driver_bar_denied(&function_capability, u32::from(layout.table_bar)).await?;
    let (bar_object, mut bar_mapping, bar_length) = open_bar_mapping(
        &function_capability,
        0,
        Some(ROCKER_BAR0_BYTES),
        "QEMU Rocker",
    )
    .await?;
    const ROCKER_TEST_REG_OFFSET: usize = 0x10;
    const TEST_REG_VALUE: u32 = 0x1234_5678;
    rocker_mmio_write_u32(
        &mut bar_mapping,
        bar_length,
        ROCKER_TEST_REG_OFFSET,
        TEST_REG_VALUE,
    )?;
    let observed_test_reg = rocker_mmio_read_u32(&bar_mapping, bar_length, ROCKER_TEST_REG_OFFSET)?;
    if observed_test_reg != TEST_REG_VALUE.wrapping_mul(2) {
        return Err(format!(
            "QEMU Rocker BAR0 liveness check failed: observed={observed_test_reg:08x}"
        ));
    }
    log::info!(
        "QEMU Rocker BAR0 liveness verified bytes={bar_length} value={observed_test_reg:08x}"
    );

    let source = route_interrupt(&function_capability).await?;
    log::info!(
        "QEMU Rocker MSI-X route prepared address={}",
        function.address
    );
    bind_interrupt_source(&source).await?;
    log::info!(
        "QEMU Rocker MSI-X source bound address={}",
        function.address
    );
    let (dma_domain, _dma_faults) = create_dma_domain(&function_capability).await?;
    set_dma_bus_master(&dma_domain, true).await?;
    log::info!("QEMU Rocker MSI-X test ready bus_master=enabled mappings=0");

    // QEMU Rocker uses TEST_IRQ to generate an MSI-X for the selected vector.
    // Vector zero is the single route owned by this InterruptSource.
    rocker_mmio_write_u32(&mut bar_mapping, bar_length, ROCKER_TEST_IRQ_OFFSET, 0)?;
    log::info!("QEMU Rocker TEST_IRQ requested MSI-X vector=0");

    let event = wait_interrupt_source(&source, &lineage).await?;
    if event.sequence != 1 || event.flags & 1 == 0 || event.flags & (1 << 2) != 0 {
        return Err(format!(
            "QEMU Rocker MSI-X event is not safely pending: sequence={} flags={:#x}",
            event.sequence, event.flags
        ));
    }
    log::info!(
        "QEMU Rocker MSI-X interrupt delivered sequence={}",
        event.sequence
    );
    ack_interrupt_source(&source, event.sequence, &lineage).await?;
    log::info!(
        "QEMU Rocker MSI-X event acknowledged sequence={}",
        event.sequence
    );

    set_dma_bus_master(&dma_domain, false).await?;
    close_dma_domain(&dma_domain).await?;
    log::info!("QEMU Rocker MSI-X test domain closed bus_master=disabled");
    drop(source);
    drop(bar_object);
    log::info!(
        "QEMU Rocker MSI-X route source closed address={}",
        function.address
    );
    Ok(())
}

#[cfg(feature = "pci-msi-route-smoke")]
async fn exercise_qemu_edu_function(
    endpoint: &ProtocolClientEndpoint,
    address: PciAddress,
    lineage: &GenerationLineage,
) -> Result<(), String> {
    let expected_requester_id = pci_requester_id(address);
    let config_dword = read_config(endpoint, 0, 4).await?;
    let config_vendor = read_config(endpoint, 0, 2).await?;
    let config_device = read_config(endpoint, 2, 2).await?;
    let config_vendor_low_byte = read_config(endpoint, 0, 1).await?;
    if config_vendor != 0x1234
        || config_device != 0x11e8
        || config_vendor_low_byte != 0x34
        || config_dword != ((config_device << 16) | config_vendor)
    {
        return Err(format!(
            "PciFunction config reads disagreed with discovery: dword={config_dword:08x} vendor={config_vendor:04x} device={config_device:04x}"
        ));
    }
    expect_misaligned_config_read_rejected(endpoint).await?;
    log::info!(
        "QEMU EDU PCI config read verified vendor={config_vendor:04x} device={config_device:04x} widths=1,2,4 bounds=checked"
    );

    const DMA_IOVA: u64 = 0x4000;
    const DMA_FAULT_IOVA: u64 = DMA_IOVA + servicekit::memory::MEMORY_PAGE_SIZE as u64;
    #[cfg(feature = "pci-amdvi-memory-iopf-smoke")]
    const DMA_FAULT_IOVA_AFTER_CLOSE: u64 = DMA_FAULT_IOVA;
    #[cfg(not(feature = "pci-amdvi-memory-iopf-smoke"))]
    const DMA_FAULT_IOVA_AFTER_CLOSE: u64 = DMA_FAULT_IOVA + 0x1000;
    const DMA_TRANSFER_BYTES: usize = 64;
    // QEMU emits device MSI as a PCI bus-master memory write. Give the EDU
    // exactly one writable IOVA page before enabling bus mastering; all other
    // addresses remain denied by the empty remainder of its IOMMU domain.
    let (dma_domain, dma_faults) = create_dma_domain(endpoint).await?;
    let dma_object = MemoryObject::new(servicekit::memory::MEMORY_PAGE_SIZE)
        .map_err(|error| format!("create QEMU EDU DMA backing: {error:?}"))?;
    let mut dma_mapping = dma_object
        .map_all(
            naos_sys::MEMORY_MAP_READ | naos_sys::MEMORY_MAP_WRITE | naos_sys::MEMORY_MAP_SHARED,
        )
        .map_err(|error| format!("map QEMU EDU DMA backing: {error:?}"))?;
    dma_mapping.as_mut_slice().fill(0xa5);
    map_dma_page(&dma_domain, &dma_object, DMA_IOVA, 1).await?;

    let (fault_dma_object, fault_dma_mapping) = {
        let object = MemoryObject::new(servicekit::memory::MEMORY_PAGE_SIZE)
            .map_err(|error| format!("create QEMU EDU read-only DMA backing: {error:?}"))?;
        let mut mapping = object
            .map_all(
                naos_sys::MEMORY_MAP_READ
                    | naos_sys::MEMORY_MAP_WRITE
                    | naos_sys::MEMORY_MAP_SHARED,
            )
            .map_err(|error| format!("map QEMU EDU read-only DMA backing: {error:?}"))?;
        mapping.as_mut_slice().fill(0x5a);
        // The device may read this page, but the next DMA is a device write.
        // AMD then reports a present-page permission fault with PR=1, PE=1,
        // RW=1, I=0, allowing the guest smoke to validate the full IOPF.
        map_dma_page(&dma_domain, &object, DMA_FAULT_IOVA, 0).await?;
        (object, mapping)
    };

    let (bar_object, mut bar_mapping, bar_length) = open_bar(endpoint, 0).await?;
    let device_id = edu_mmio_read_u32(&bar_mapping, bar_length, 0)?;
    if device_id != 0x0100_00ed {
        return Err(format!(
            "unexpected QEMU EDU BAR0 identifier: {device_id:08x}"
        ));
    }
    const LIVENESS_VALUE: u32 = 0x1234_5678;
    edu_mmio_write_u32(&mut bar_mapping, bar_length, 4, LIVENESS_VALUE)?;
    let liveness = edu_mmio_read_u32(&bar_mapping, bar_length, 4)?;
    if liveness != !LIVENESS_VALUE {
        return Err(format!(
            "QEMU EDU BAR0 liveness check failed: {liveness:08x}"
        ));
    }
    log::info!(
        "QEMU EDU BAR0 mapped id={device_id:08x} bytes={bar_length} liveness={liveness:08x}"
    );

    let source = route_interrupt(endpoint).await?;
    log::info!(
        "QEMU EDU MSI route prepared address={} vendor={:04x} device={:04x}",
        address,
        config_vendor,
        config_device
    );
    bind_interrupt_source(&source).await?;
    log::info!("QEMU EDU MSI source bound address={address}");
    set_dma_bus_master(&dma_domain, true).await?;
    log::info!("QEMU EDU default-deny DMA domain ready bus_master=enabled mappings=1");

    // EDU's internal 4 KiB DMA buffer is zero-initialized. Program a device-
    // write transfer into the single authorized IOVA page; completion raises
    // the real device MSI handled by the bound InterruptSource.
    edu_mmio_write_u64(&mut bar_mapping, bar_length, 0x80, 0x40000)?;
    edu_mmio_write_u64(&mut bar_mapping, bar_length, 0x88, DMA_IOVA)?;
    edu_mmio_write_u64(
        &mut bar_mapping,
        bar_length,
        0x90,
        DMA_TRANSFER_BYTES as u64,
    )?;
    edu_mmio_write_u32(&mut bar_mapping, bar_length, 0x98, 0x7)?;
    let event = wait_interrupt_source(&source, lineage).await?;
    if event.flags & 1 == 0 || event.flags & (1 << 2) != 0 {
        return Err(format!(
            "QEMU EDU MSI event is not safely pending: flags={:#x}",
            event.flags
        ));
    }
    let device_status = edu_mmio_read_u32(&bar_mapping, bar_length, 0x24)?;
    if device_status & 0x100 == 0 {
        return Err(format!(
            "QEMU EDU DMA MSI arrived without DMA IRQ status: {device_status:08x}"
        ));
    }
    let transferred = &dma_mapping.as_slice()[..DMA_TRANSFER_BYTES];
    if transferred.iter().any(|byte| *byte != 0)
        || dma_mapping.as_slice()[DMA_TRANSFER_BYTES..DMA_TRANSFER_BYTES + 16]
            .iter()
            .any(|byte| *byte != 0xa5)
    {
        return Err("QEMU EDU DMA write did not update exactly the authorized IOVA range".into());
    }
    log::info!("QEMU EDU DMA transfer verified bytes={DMA_TRANSFER_BYTES} iova={DMA_IOVA:08x}");
    log::info!(
        "QEMU EDU MSI delivered sequence={} status={device_status:08x}",
        event.sequence
    );
    edu_mmio_write_u32(&mut bar_mapping, bar_length, 0x64, device_status)?;
    ack_interrupt_source(&source, event.sequence, lineage).await?;
    log::info!(
        "QEMU EDU MSI device ack completed sequence={}",
        event.sequence
    );

    // EDU still completes and raises MSI when its DMA write faults, so IRQ
    // delivery alone cannot prove that the IOMMU rejected the access.
    edu_mmio_write_u64(&mut bar_mapping, bar_length, 0x80, 0x40000)?;
    edu_mmio_write_u64(&mut bar_mapping, bar_length, 0x88, DMA_FAULT_IOVA)?;
    edu_mmio_write_u64(
        &mut bar_mapping,
        bar_length,
        0x90,
        DMA_TRANSFER_BYTES as u64,
    )?;
    edu_mmio_write_u32(&mut bar_mapping, bar_length, 0x98, 0x7)?;
    let denied_event = wait_interrupt_source(&source, lineage).await?;
    if event.sequence.checked_add(1) != Some(denied_event.sequence)
        || denied_event.flags & 1 == 0
        || denied_event.flags & (1 << 2) != 0
    {
        return Err(format!(
            "QEMU EDU denied DMA completion event is invalid: sequence={} flags={:#x}",
            denied_event.sequence, denied_event.flags
        ));
    }
    let denied_device_status = edu_mmio_read_u32(&bar_mapping, bar_length, 0x24)?;
    if denied_device_status & 0x100 == 0 {
        return Err(format!(
            "QEMU EDU unmapped DMA request raised no DMA IRQ: {denied_device_status:08x}"
        ));
    }
    #[cfg(not(feature = "pci-amdvi-memory-iopf-smoke"))]
    if dma_mapping.as_slice()[..DMA_TRANSFER_BYTES]
        .iter()
        .any(|byte| *byte != 0)
        || dma_mapping.as_slice()[DMA_TRANSFER_BYTES..]
            .iter()
            .any(|byte| *byte != 0xa5)
    {
        return Err("QEMU EDU unmapped DMA request modified the authorized DMA page".into());
    }

    if fault_dma_mapping
        .as_slice()
        .iter()
        .any(|byte| *byte != 0x5a)
    {
        return Err("QEMU EDU denied write modified its device-read-only DMA page".into());
    }

    log::info!(
        "QEMU EDU DMA read-only IOVA write rejected iova={DMA_FAULT_IOVA:08x} sequence={}",
        denied_event.sequence
    );
    edu_mmio_write_u32(&mut bar_mapping, bar_length, 0x64, denied_device_status)?;
    ack_interrupt_source(&source, denied_event.sequence, lineage).await?;
    // This opt-in smoke must still verify owner teardown when the selected
    // provider records no memory-fault batch. Keep the drain attempts bounded
    // and asynchronous so the test never waits forever on an absent event.
    const MAX_FAULT_DRAIN_ATTEMPTS: usize = 256;
    let mut fault_batch = None;
    let mut fault_delivery_issue = None;
    for attempt in 0..MAX_FAULT_DRAIN_ATTEMPTS {
        match take_dma_faults(&dma_faults, 16, lineage).await {
            Ok(batch)
                if batch.count != 0 || batch.dropped_records != 0 || batch.unit_overflow != 0 =>
            {
                fault_batch = Some(batch);
                break;
            }
            Ok(_) => {
                if attempt + 1 < MAX_FAULT_DRAIN_ATTEMPTS {
                    tokio::task::yield_now().await;
                }
            }
            Err(error) => {
                fault_delivery_issue = Some(error);
                break;
            }
        }
    }
    if fault_batch.is_none() && fault_delivery_issue.is_none() {
        fault_delivery_issue = Some(format!(
            "no fault record appeared after {MAX_FAULT_DRAIN_ATTEMPTS} bounded readiness turns"
        ));
    }
    if let Some(fault_delivery_issue) = fault_delivery_issue {
        log::warn!(
            "QEMU EDU DMA fault not reported ({}); validating owner teardown",
            fault_delivery_issue
        );
        set_dma_bus_master(&dma_domain, false).await?;
        log::info!("QEMU EDU DMA domain bus_master=disabled");
        drop(dma_mapping);
        drop(fault_dma_mapping);
        unmap_dma_page(&dma_domain, DMA_FAULT_IOVA).await?;
        drop(fault_dma_object);
        unmap_dma_page(&dma_domain, DMA_IOVA).await?;
        close_dma_domain(&dma_domain).await?;
        log::info!("QEMU EDU DMA page unmapped");
        log::info!("QEMU EDU DMA domain closed after missing fault report");
        drop(source);
        log::info!("QEMU EDU MSI route source closed after missing fault report");
        return Err(format!(
            "QEMU EDU IOMMU fault was not delivered ({fault_delivery_issue}); owner teardown completed"
        ));
    }
    let fault_batch = match fault_batch {
        Some(batch) => batch,
        None => return Err("fault-delivery state lost its batch without an issue".into()),
    };
    if !denied_write_batch_matches(
        &fault_batch,
        expected_requester_id,
        DMA_FAULT_IOVA,
        DMA_TRANSFER_BYTES as u64,
    ) {
        let first_fault = fault_batch.records.first().map(|fault| {
            (
                fault.iova,
                fault.requester_id,
                fault.reason,
                fault.direction,
            )
        });
        return Err(format!(
            "QEMU EDU DMA fault batch did not report the denied write: sequence={} records={} dropped={} unit_overflow={} first={first_fault:?}",
            fault_batch.sequence,
            fault_batch.count,
            fault_batch.dropped_records,
            fault_batch.unit_overflow
        ));
    }
    log::info!(
        "QEMU EDU IOMMU fault delivered bus_master=enabled sequence={} records={} first_backend_reason={:02x} iova_range={DMA_FAULT_IOVA:08x}..{:08x}",
        fault_batch.sequence,
        fault_batch.count,
        fault_batch.records.first().map_or(0, |fault| fault.reason),
        DMA_FAULT_IOVA + DMA_TRANSFER_BYTES as u64
    );
    set_dma_bus_master(&dma_domain, false).await?;
    log::info!("QEMU EDU DMA domain bus_master=disabled");
    let post_disable_fault_batch = take_dma_faults(&dma_faults, 16, lineage).await?;
    if post_disable_fault_batch.count != 0
        || post_disable_fault_batch.dropped_records != 0
        || post_disable_fault_batch.unit_overflow != 0
    {
        return Err(format!(
            "QEMU EDU IOMMU fault batch changed across BME shutdown: records={} dropped={} unit_overflow={}",
            post_disable_fault_batch.count,
            post_disable_fault_batch.dropped_records,
            post_disable_fault_batch.unit_overflow
        ));
    }

    // Keep a second fault queued in the function-scoped source while closing
    // the domain. The source must deliver its final batch before reporting
    // revocation; otherwise the IPC adapter would discard data with EIDRM.
    set_dma_bus_master(&dma_domain, true).await?;
    edu_mmio_write_u64(&mut bar_mapping, bar_length, 0x80, 0x40000)?;
    edu_mmio_write_u64(
        &mut bar_mapping,
        bar_length,
        0x88,
        DMA_FAULT_IOVA_AFTER_CLOSE,
    )?;
    edu_mmio_write_u64(
        &mut bar_mapping,
        bar_length,
        0x90,
        DMA_TRANSFER_BYTES as u64,
    )?;
    edu_mmio_write_u32(&mut bar_mapping, bar_length, 0x98, 0x7)?;
    let late_denied_event = wait_interrupt_source(&source, lineage).await?;
    if denied_event.sequence.checked_add(1) != Some(late_denied_event.sequence)
        || late_denied_event.flags & 1 == 0
        || late_denied_event.flags & (1 << 2) != 0
    {
        return Err(format!(
            "QEMU EDU post-drain denied DMA completion is invalid: sequence={} flags={:#x}",
            late_denied_event.sequence, late_denied_event.flags
        ));
    }
    let late_device_status = edu_mmio_read_u32(&bar_mapping, bar_length, 0x24)?;
    if late_device_status & 0x100 == 0 {
        return Err(format!(
            "QEMU EDU post-drain fault raised no DMA IRQ: {late_device_status:08x}"
        ));
    }
    edu_mmio_write_u32(&mut bar_mapping, bar_length, 0x64, late_device_status)?;
    ack_interrupt_source(&source, late_denied_event.sequence, lineage).await?;

    let close_readiness = servicekit::wait_ready_async(&[dma_faults.get()], Some(5_000_000))
        .await
        .map_err(|status| format!("PciDmaFaultSource close-path readiness failed: {status}"))?;
    if !close_readiness.readable {
        return Err(format!(
            "QEMU EDU close-path fault was not readable before domain close (closed={} error={})",
            close_readiness.read_closed || close_readiness.write_closed,
            close_readiness.error
        ));
    }
    log::info!(
        "QEMU EDU IOMMU fault queued before source close iova={DMA_FAULT_IOVA_AFTER_CLOSE:08x}"
    );
    set_dma_bus_master(&dma_domain, false).await?;
    drop(dma_mapping);
    drop(fault_dma_mapping);
    unmap_dma_page(&dma_domain, DMA_FAULT_IOVA).await?;
    drop(fault_dma_object);
    unmap_dma_page(&dma_domain, DMA_IOVA).await?;
    close_dma_domain(&dma_domain).await?;

    let closed_source_batch = take_dma_faults(&dma_faults, 16, lineage).await?;
    if !denied_write_batch_matches(
        &closed_source_batch,
        expected_requester_id,
        DMA_FAULT_IOVA_AFTER_CLOSE,
        DMA_TRANSFER_BYTES as u64,
    ) {
        let first_fault = closed_source_batch.records.first().map(|fault| {
            (
                fault.iova,
                fault.requester_id,
                fault.reason,
                fault.direction,
            )
        });
        return Err(format!(
            "QEMU EDU closed fault source lost its final batch: sequence={} records={} dropped={} unit_overflow={} first={first_fault:?}",
            closed_source_batch.sequence,
            closed_source_batch.count,
            closed_source_batch.dropped_records,
            closed_source_batch.unit_overflow
        ));
    }
    log::info!(
        "QEMU EDU IOMMU fault drained after source close sequence={} records={} first_backend_reason={:02x} iova_range={DMA_FAULT_IOVA_AFTER_CLOSE:08x}..{:08x}",
        closed_source_batch.sequence,
        closed_source_batch.count,
        closed_source_batch
            .records
            .first()
            .map_or(0, |fault| fault.reason),
        DMA_FAULT_IOVA_AFTER_CLOSE + DMA_TRANSFER_BYTES as u64
    );

    drop(source);
    log::info!("QEMU EDU DMA page unmapped");
    log::info!("QEMU EDU DMA domain closed");
    drop(dma_faults);
    drop(dma_domain);
    drop(dma_object);
    drop(bar_mapping);
    drop(bar_object);
    log::info!("QEMU EDU MSI route source closed address={address}");
    Ok(())
}

#[cfg(feature = "pci-msi-route-smoke")]
pub(super) async fn run_driver(context: Context) -> i64 {
    let capability = match servicekit::resolve_resource(
        context.service_directory_handle(),
        servicekit::PCI_FUNCTION_URI,
    ) {
        Ok(capability) => capability,
        Err(error) => {
            log::error!("resolve granted PciFunction capability failed: {error:?}");
            return 1;
        }
    };
    match run_granted_pci_driver(context, capability).await {
        Ok(()) => 0,
        Err(error) => {
            log::error!("QEMU EDU process-scoped driver failed: {error}");
            1
        }
    }
}
