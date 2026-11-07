#[cfg(target_os = "naos")]
#[tokio::main(flavor = "current_thread")]
async fn main() -> std::process::ExitCode {
    std::process::ExitCode::from(
        servicekit::run("virtio-block-smoke", virtio_block_smoke).await as u8,
    )
}

#[cfg(target_os = "naos")]
async fn virtio_block_smoke(context: servicekit::Context) -> i64 {
    match run_smoke(context).await {
        Ok(()) => 0,
        Err(error) => {
            log::error!("Virtio block smoke failed: {error}");
            println!("Virtio block smoke failed: {error}");
            1
        }
    }
}

#[cfg(not(target_os = "naos"))]
fn main() {
    println!("virtio-block-smoke requires NaOS");
}

#[cfg(target_os = "naos")]
async fn run_smoke(context: servicekit::Context) -> Result<(), String> {
    use naos_idl::block_device_factory as factory;
    use naos_idl::pci_block_device_catalog as catalog;
    use naos_idl::{ProtocolClientEndpoint, ResourceTransferBuilder};
    use servicekit::memory::MemoryObject;
    use std::time::Duration;

    const CATALOG_URI: &str = "naos://system/pci/block-device-catalog";
    const TEST_VENDOR: u16 = 0x1af4;
    const TEST_DEVICE: u16 = 0x1042;
    const TEST_LBA: u64 = 8;
    const TEST_BLOCKS: u64 = 8;
    const TEST_BYTES: usize = (TEST_BLOCKS * 512) as usize;
    const TEST_CAPACITY_SECTORS: u64 = 65_536;

    let catalog_endpoint = servicekit::connect_until(
        context.service_directory_handle(),
        CATALOG_URI,
        catalog::PROTOCOL_UUID,
        catalog::PROTOCOL_RIGHTS,
        catalog::PROTOCOL_REVISION,
        catalog::PROTOCOL_FEATURES,
        Duration::from_secs(5),
    )
    .await
    .map_err(|error| format!("connect PCI block catalog: {error:?}"))?;

    let mut snapshot_firmware_version = String::new();
    let mut snapshot_fabric_generation = 0;
    let mut cursor = 0;
    let mut test_factory = None;
    loop {
        let request = catalog::list_request {
            firmware_version: snapshot_firmware_version.as_str(),
            pci_fabric_generation: snapshot_fabric_generation,
            cursor,
        };
        let mut request_wire =
            vec![0_u8; catalog::LIST_REQUEST_HEADER_BYTES + request.firmware_version.len()];
        let mut invocation = catalog::submit_list(
            &catalog_endpoint,
            &request,
            ResourceTransferBuilder::new(),
            &mut request_wire,
            0,
        )
        .map_err(|error| format!("submit PCI block catalog.list: {error:?}"))?;
        wait_for_call(invocation.get(), "PCI block catalog.list").await?;
        let mut response_wire = [0_u8; catalog::LIST_RESPONSE_HEADER_BYTES + 32];
        let response = catalog::take_list(&mut invocation, &mut response_wire)
            .map_err(|error| format!("take PCI block catalog.list: {error:?}"))?;
        let page = response.value;
        if snapshot_firmware_version.is_empty() {
            snapshot_firmware_version = response.firmware_version.to_owned();
            snapshot_fabric_generation = page.pci_fabric_generation;
        }
        if response.firmware_version != snapshot_firmware_version
            || page.pci_fabric_generation != snapshot_fabric_generation
            || page.count as usize > page.factories.len()
        {
            return Err("PCI block catalog changed generation during enumeration".into());
        }
        test_factory = page.factories[..page.count as usize]
            .iter()
            .copied()
            .find(|identity| {
                identity.segment == 0
                    && identity.bus == 0
                    && identity.device == 3
                    && identity.function == 0
                    && identity.vendor_id == TEST_VENDOR
                    && identity.pci_device_id == TEST_DEVICE
            })
            .or(test_factory);
        if test_factory.is_some() || page.next_cursor == 0 {
            break;
        }
        cursor = page.next_cursor;
    }
    let Some(test_factory) = test_factory else {
        report("Virtio block smoke skipped: QEMU test function 0000:00:03.0 is absent");
        return Ok(());
    };
    report("Virtio block smoke found QEMU test function 0000:00:03.0");

    let request = catalog::open_factory_request {
        firmware_version: snapshot_firmware_version.as_str(),
        pci_fabric_generation: snapshot_fabric_generation,
        factory_id: test_factory.factory_id,
    };
    let mut request_wire =
        vec![0_u8; catalog::OPEN_FACTORY_REQUEST_HEADER_BYTES + request.firmware_version.len()];
    let mut invocation = catalog::submit_open_factory(
        &catalog_endpoint,
        &request,
        ResourceTransferBuilder::new(),
        &mut request_wire,
        0,
    )
    .map_err(|error| format!("submit PCI block catalog.open_factory: {error:?}"))?;
    wait_for_call(invocation.get(), "PCI block catalog.open_factory").await?;
    let mut response_wire = [0_u8; catalog::OPEN_FACTORY_RESPONSE_HEADER_BYTES];
    let (response, mut resources) = catalog::take_open_factory(&mut invocation, &mut response_wire)
        .map_err(|error| format!("take PCI block catalog.open_factory: {error:?}"))?;
    let factory_handle = resources
        .take(response.factory)
        .ok_or_else(|| "PCI block catalog omitted the factory endpoint".to_string())?;
    // SAFETY: NaoIDL validated the moved capability's BlockDeviceFactory scope.
    let factory_endpoint = unsafe { ProtocolClientEndpoint::from_raw(factory_handle.into_raw()) };

    let mut request_wire = [];
    let mut invocation = factory::submit_get_info(
        &factory_endpoint,
        &factory::get_info_request {},
        ResourceTransferBuilder::new(),
        &mut request_wire,
        0,
    )
    .map_err(|error| format!("submit BlockDeviceFactory.get_info: {error:?}"))?;
    wait_for_call(invocation.get(), "BlockDeviceFactory.get_info").await?;
    let mut response_wire = [0_u8; factory::GET_INFO_RESPONSE_HEADER_BYTES];
    let factory_info = factory::take_get_info(&mut invocation, &mut response_wire)
        .map_err(|error| format!("take BlockDeviceFactory.get_info: {error:?}"))?
        .value;
    if factory_info.total_block_count != TEST_CAPACITY_SECTORS
        || factory_info.logical_block_bytes != 512
    {
        return Err(format!(
            "refusing to write QEMU test disk with unexpected geometry: sectors={} block_bytes={}",
            factory_info.total_block_count, factory_info.logical_block_bytes
        ));
    }

    let request = factory::acquire_request {
        start_lba: 0,
        block_count: factory_info.total_block_count,
        flags: 0,
    };
    let mut request_wire = [0_u8; factory::ACQUIRE_REQUEST_HEADER_BYTES];
    let mut invocation = factory::submit_acquire(
        &factory_endpoint,
        &request,
        ResourceTransferBuilder::new(),
        &mut request_wire,
        0,
    )
    .map_err(|error| format!("submit BlockDeviceFactory.acquire: {error:?}"))?;
    wait_for_call(invocation.get(), "BlockDeviceFactory.acquire").await?;
    let mut response_wire = [0_u8; factory::ACQUIRE_RESPONSE_HEADER_BYTES];
    let (response, mut resources) = factory::take_acquire(&mut invocation, &mut response_wire)
        .map_err(|error| format!("take BlockDeviceFactory.acquire: {error:?}"))?;
    let device_handle = resources
        .take(response.device)
        .ok_or_else(|| "BlockDeviceFactory omitted the BlockDevice endpoint".to_string())?;
    // SAFETY: NaoIDL validated the moved capability's BlockDevice scope.
    let device_endpoint = unsafe { ProtocolClientEndpoint::from_raw(device_handle.into_raw()) };

    let buffer = MemoryObject::new(TEST_BYTES)
        .map_err(|error| format!("allocate block smoke MemoryObject: {error:?}"))?;
    let mut mapping = buffer
        .map_all(
            servicekit::sys::MEMORY_MAP_READ
                | servicekit::sys::MEMORY_MAP_WRITE
                | servicekit::sys::MEMORY_MAP_SHARED,
        )
        .map_err(|error| format!("map block smoke MemoryObject: {error:?}"))?;
    let expected: Vec<u8> = (0..TEST_BYTES)
        .map(|index| (index as u8).wrapping_mul(17).wrapping_add(0x5a))
        .collect();
    mapping.as_mut_slice().copy_from_slice(&expected);

    #[cfg(feature = "pci-virtio-block-fault-smoke")]
    {
        report("Virtio block fault smoke submitting a write that must hit an unmapped DMA IOVA");
        call_write_expect_terminal_dma_error(&device_endpoint, &buffer, TEST_LBA, TEST_BLOCKS)
            .await?;
        drop(mapping);
        drop(buffer);
        drop(device_endpoint);
        drop(factory_endpoint);
        report("Virtio block fault smoke waiting for catalog cleanup");
        wait_for_factory_removal(
            &catalog_endpoint,
            &snapshot_firmware_version,
            snapshot_fabric_generation,
            test_factory.factory_id,
        )
        .await?;
        report("Virtio block fault smoke verified terminal DMA failure and catalog removal");
        drop(catalog_endpoint);
        return Ok(());
    }

    #[cfg(not(feature = "pci-virtio-block-fault-smoke"))]
    {
        report("Virtio block smoke submitting BlockDevice.write");
        call_write(&device_endpoint, &buffer, TEST_LBA, TEST_BLOCKS).await?;
        report("Virtio block smoke BlockDevice.write completed");
        report("Virtio block smoke submitting BlockDevice.flush");
        call_flush(&device_endpoint).await?;
        report("Virtio block smoke BlockDevice.flush completed");
        mapping.as_mut_slice().fill(0xa5);
        report("Virtio block smoke submitting BlockDevice.read");
        call_read(&device_endpoint, &buffer, TEST_LBA, TEST_BLOCKS).await?;
        report("Virtio block smoke BlockDevice.read completed");
        if mapping.as_slice() != expected.as_slice() {
            return Err("read-after-write data did not match the DMA block pattern".into());
        }

        report("Virtio block smoke verified write, flush and read-back through BlockDevice");
        drop(mapping);
        drop(buffer);
        drop(device_endpoint);
        drop(factory_endpoint);
        drop(catalog_endpoint);
        Ok(())
    }
}

#[cfg(all(target_os = "naos", feature = "pci-virtio-block-fault-smoke"))]
async fn call_write_expect_terminal_dma_error(
    endpoint: &naos_idl::ProtocolClientEndpoint,
    buffer: &servicekit::memory::MemoryObject,
    lba: u64,
    block_count: u64,
) -> Result<(), String> {
    use naos_idl::CallError;
    use naos_idl::ResourceTransferBuilder;
    use naos_idl::block_device;

    let mut resources = ResourceTransferBuilder::new();
    let slot = resources
        .push_duplicate(buffer.as_handle())
        .map_err(|error| format!("duplicate fault-smoke block write buffer: {error:?}"))?;
    let request = block_device::write_request {
        lba,
        block_count,
        buffer: slot,
        flags: 0,
    };
    let mut request_wire = [0_u8; block_device::WRITE_REQUEST_HEADER_BYTES];
    let mut invocation =
        block_device::submit_write(endpoint, &request, resources, &mut request_wire, 0)
            .map_err(|error| format!("submit fault-smoke BlockDevice.write: {error:?}"))?;
    wait_for_call(invocation.get(), "fault-smoke BlockDevice.write").await?;
    match block_device::take_write(&mut invocation, &mut []) {
        Err(CallError::Outcome { protocol_error, .. })
            if protocol_error == block_device::WRITE_ERROR_EIO =>
        {
            report("Virtio block fault smoke received the expected BlockDevice EIO");
            Ok(())
        }
        Err(error) => Err(format!(
            "fault-smoke BlockDevice.write returned unexpected error: {error:?}"
        )),
        Ok(_) => Err("fault-smoke BlockDevice.write unexpectedly succeeded".into()),
    }
}

#[cfg(all(target_os = "naos", feature = "pci-virtio-block-fault-smoke"))]
async fn wait_for_factory_removal(
    catalog_endpoint: &naos_idl::ProtocolClientEndpoint,
    firmware_version: &str,
    pci_fabric_generation: u64,
    factory_id: u64,
) -> Result<(), String> {
    use naos_idl::CallError;
    use naos_idl::ResourceTransferBuilder;
    use naos_idl::pci_block_device_catalog as catalog;
    use std::time::Duration;

    let deadline = tokio::time::Instant::now() + Duration::from_secs(10);
    let mut first_snapshot_still_listed = false;
    loop {
        let request = catalog::list_request {
            firmware_version,
            pci_fabric_generation,
            cursor: 0,
        };
        let mut request_wire = [0_u8; catalog::LIST_REQUEST_HEADER_BYTES + 32];
        let mut invocation = catalog::submit_list(
            catalog_endpoint,
            &request,
            ResourceTransferBuilder::new(),
            &mut request_wire,
            0,
        )
        .map_err(|error| format!("submit catalog.list while waiting for driver exit: {error:?}"))?;
        tokio::time::timeout(
            Duration::from_secs(2),
            wait_for_call(invocation.get(), "catalog.list after Virtio driver exit"),
        )
        .await
        .map_err(|_| "catalog.list after Virtio driver exit timed out".to_string())??;
        let mut response_wire = [0_u8; catalog::LIST_RESPONSE_HEADER_BYTES + 32];
        let response = catalog::take_list(&mut invocation, &mut response_wire)
            .map_err(|error| format!("take catalog.list after Virtio driver exit: {error:?}"))?;
        let page = response.value;
        report("Virtio block fault smoke received a catalog snapshot after driver exit");
        if response.firmware_version != firmware_version
            || page.pci_fabric_generation != pci_fabric_generation
        {
            return Err(
                "PCI block catalog generation changed while waiting for driver cleanup".into(),
            );
        }
        let factory_is_listed = page.factories[..page.count as usize]
            .iter()
            .any(|identity| identity.factory_id == factory_id);
        if !factory_is_listed {
            report("Virtio block fault smoke observed its provider missing from the catalog");
            let request = catalog::open_factory_request {
                firmware_version,
                pci_fabric_generation,
                factory_id,
            };
            let mut request_wire = vec![
                0_u8;
                catalog::OPEN_FACTORY_REQUEST_HEADER_BYTES
                    + request.firmware_version.len()
            ];
            let mut invocation = catalog::submit_open_factory(
                catalog_endpoint,
                &request,
                ResourceTransferBuilder::new(),
                &mut request_wire,
                0,
            )
            .map_err(|error| format!("submit removed factory lookup: {error:?}"))?;
            tokio::time::timeout(
                Duration::from_secs(2),
                wait_for_call(
                    invocation.get(),
                    "catalog.open_factory after provider removal",
                ),
            )
            .await
            .map_err(|_| "removed factory lookup timed out".to_string())??;
            let mut response_wire = [0_u8; catalog::OPEN_FACTORY_RESPONSE_HEADER_BYTES];
            match catalog::take_open_factory(&mut invocation, &mut response_wire) {
                Err(CallError::Outcome { protocol_error, .. })
                    if protocol_error == catalog::OPEN_FACTORY_ERROR_ENODEV =>
                {
                    return Ok(());
                }
                Err(error) => {
                    return Err(format!(
                        "removed factory lookup returned unexpected error: {error:?}"
                    ));
                }
                Ok(_) => return Err("PCI block catalog still opened the removed factory".into()),
            }
        }
        if tokio::time::Instant::now() >= deadline {
            return Err(format!(
                "PCI block factory {factory_id} remained listed after driver exit"
            ));
        }
        if factory_is_listed && !first_snapshot_still_listed {
            report("Virtio block fault smoke saw the provider before exit cleanup and will retry");
            first_snapshot_still_listed = true;
        }
        tokio::task::yield_now().await;
    }
}

#[cfg(target_os = "naos")]
async fn call_write(
    endpoint: &naos_idl::ProtocolClientEndpoint,
    buffer: &servicekit::memory::MemoryObject,
    lba: u64,
    block_count: u64,
) -> Result<(), String> {
    use naos_idl::ResourceTransferBuilder;
    use naos_idl::block_device;

    let mut resources = ResourceTransferBuilder::new();
    let slot = resources
        .push_duplicate(buffer.as_handle())
        .map_err(|error| format!("duplicate block write buffer: {error:?}"))?;
    let request = block_device::write_request {
        lba,
        block_count,
        buffer: slot,
        flags: 0,
    };
    let mut request_wire = [0_u8; block_device::WRITE_REQUEST_HEADER_BYTES];
    let mut invocation =
        block_device::submit_write(endpoint, &request, resources, &mut request_wire, 0)
            .map_err(|error| format!("submit BlockDevice.write: {error:?}"))?;
    wait_for_call(invocation.get(), "BlockDevice.write").await?;
    block_device::take_write(&mut invocation, &mut [])
        .map_err(|error| format!("take BlockDevice.write: {error:?}"))?;
    Ok(())
}

#[cfg(target_os = "naos")]
async fn call_read(
    endpoint: &naos_idl::ProtocolClientEndpoint,
    buffer: &servicekit::memory::MemoryObject,
    lba: u64,
    block_count: u64,
) -> Result<(), String> {
    use naos_idl::ResourceTransferBuilder;
    use naos_idl::block_device;

    let mut resources = ResourceTransferBuilder::new();
    let slot = resources
        .push_duplicate(buffer.as_handle())
        .map_err(|error| format!("duplicate block read buffer: {error:?}"))?;
    let request = block_device::read_request {
        lba,
        block_count,
        buffer: slot,
        flags: 0,
    };
    let mut request_wire = [0_u8; block_device::READ_REQUEST_HEADER_BYTES];
    let mut invocation =
        block_device::submit_read(endpoint, &request, resources, &mut request_wire, 0)
            .map_err(|error| format!("submit BlockDevice.read: {error:?}"))?;
    wait_for_call(invocation.get(), "BlockDevice.read").await?;
    block_device::take_read(&mut invocation, &mut [])
        .map_err(|error| format!("take BlockDevice.read: {error:?}"))?;
    Ok(())
}

#[cfg(target_os = "naos")]
async fn call_flush(endpoint: &naos_idl::ProtocolClientEndpoint) -> Result<(), String> {
    use naos_idl::ResourceTransferBuilder;
    use naos_idl::block_device;

    let mut request_wire = [];
    let mut invocation = block_device::submit_flush(
        endpoint,
        &block_device::flush_request {},
        ResourceTransferBuilder::new(),
        &mut request_wire,
        0,
    )
    .map_err(|error| format!("submit BlockDevice.flush: {error:?}"))?;
    wait_for_call(invocation.get(), "BlockDevice.flush").await?;
    block_device::take_flush(&mut invocation, &mut [])
        .map_err(|error| format!("take BlockDevice.flush: {error:?}"))?;
    Ok(())
}

#[cfg(target_os = "naos")]
async fn wait_for_call(handle: naos_sys::Handle, operation: &str) -> Result<(), String> {
    let ready = servicekit::wait_ready_async(&[handle], None)
        .await
        .map_err(|status| format!("{operation} readiness failed: {status}"))?;
    if !ready.readable {
        return Err(format!("{operation} completed without a readable result"));
    }
    Ok(())
}

#[cfg(target_os = "naos")]
fn report(message: &str) {
    log::info!("{message}");
    println!("{message}");
}
