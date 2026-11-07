//! Single-queue modern Virtio block driver and its BlockDevice provider.

use crate::virtio_dma::{DmaDirection, DmaRegion, DmaRegionError};
use crate::virtio_pci;
use crate::virtio_queue::{DESC_F_NEXT, DESC_F_WRITE, Descriptor, QueueError, SplitQueueLayout};
use crate::virtio_transport::{TransportError, VirtioPciTransport};
use naos_idl::block_device as device_idl;
use naos_idl::block_device_factory as factory_idl;
use naos_idl::interrupt_source;
use naos_idl::pci_dma_domain;
use naos_idl::pci_dma_fault_source;
use naos_idl::pci_function;
use naos_idl::transport::{RpcMethodReply, RpcOutcome};
use naos_idl::{ProtocolClientEndpoint, ResourceSlot, ResourceTransferBuilder};
use servicekit::memory::MEMORY_PAGE_SIZE;
use servicekit::server::{EndpointResource, Request, ServeHandler, Server, ServerError};
use servicekit::{Context, sys};
use std::time::Duration;
use std::vec::Vec;

const FACTORY_URI: &str = "naos://service/pci/block-device-factory";
const BLOCK_BYTES: u64 = 512;
const MAX_TRANSFER_BLOCKS: u64 = 128;
const MAX_TRANSFER_BYTES: usize = (BLOCK_BYTES * MAX_TRANSFER_BLOCKS) as usize;
const MAX_IN_FLIGHT: u64 = 1;
const MAX_BLOCK_LEASES: usize = 256;

const ERR_EIO: i64 = -5;
const ERR_EACCES: i64 = -13;
const ERR_ENODEV: i64 = -19;
const ERR_EINVAL: i64 = -22;
const ERR_EBUSY: i64 = -16;
const ERR_EOVERFLOW: i64 = -75;
const ERR_EOPNOTSUPP: i64 = -95;

const BLOCK_FEATURE_READ_ONLY: u64 = 1;
const BLOCK_FEATURE_FLUSH: u64 = 1 << 1;
const BLOCK_FEATURE_FUA: u64 = 1 << 2;
const BLOCK_RIGHT_INSPECT: u64 = 1 << 13;
const BLOCK_RIGHT_READ: u64 = 1 << 14;
const BLOCK_RIGHT_WRITE: u64 = 1 << 15;
const BLOCK_RIGHT_FLUSH: u64 = 1 << 16;
const ACQUIRE_READ_ONLY: u64 = 1;
const WRITE_FLAG_FUA: u64 = 1;

const VIRTIO_F_ACCESS_PLATFORM: u64 = 1 << 33;
const VIRTIO_F_ORDER_PLATFORM: u64 = 1 << 36;
const VIRTIO_BLK_F_RO: u64 = 1 << 5;
const VIRTIO_BLK_F_FLUSH: u64 = 1 << 9;
const VIRTIO_BLK_T_IN: u32 = 0;
const VIRTIO_BLK_T_OUT: u32 = 1;
const VIRTIO_BLK_T_FLUSH: u32 = 4;
const VIRTIO_BLK_S_OK: u8 = 0;
const VIRTIO_BLK_S_IOERR: u8 = 1;
const VIRTIO_BLK_S_UNSUPP: u8 = 2;

const QUEUE_SIZE_LIMIT: u16 = 8;
const QUEUE_DESC_IOVA: u64 = 0x1000_0000;
const QUEUE_USED_IOVA: u64 = 0x1000_1000;
const REQUEST_HEADER_IOVA: u64 = 0x1000_2000;
const REQUEST_STATUS_IOVA: u64 = 0x1000_3000;
const READ_DATA_IOVA: u64 = 0x1000_4000;
const WRITE_DATA_IOVA: u64 = 0x1001_4000;
#[cfg(feature = "pci-virtio-block-fault-smoke")]
const FAULT_TEST_IOVA: u64 = 0x2000_0000;
const DMA_PAGE_BYTES: usize = MEMORY_PAGE_SIZE;
const COMPLETION_TIMEOUT: Duration = Duration::from_secs(10);
const INTERRUPT_FLAG_PENDING: u32 = 1;
const INTERRUPT_FLAG_MASKED: u32 = 1 << 1;
const INTERRUPT_FLAG_UNCERTAIN: u32 = 1 << 2;

enum CompletionWake {
    DmaFault(Result<servicekit::ReadyEvent, sys::Status>),
    Interrupt(Result<servicekit::ReadyEvent, sys::Status>),
    Timeout,
}

struct DriverState {
    block: VirtioBlock,
    leases: Vec<BlockLease>,
    next_device_id: u64,
}

#[derive(Clone, Copy)]
struct BlockLease {
    resource_id: u64,
    device_id: u64,
    start_lba: u64,
    block_count: u64,
    read_only: bool,
}

struct VirtioBlock {
    transport: VirtioPciTransport,
    dma_domain: ProtocolClientEndpoint,
    _fault_source: ProtocolClientEndpoint,
    fault_readiness: servicekit::ReadinessSet,
    interrupt_source: Option<ProtocolClientEndpoint>,
    interrupt_readiness: servicekit::ReadinessSet,
    interrupt_lineage: crate::topology::GenerationLineage,
    descriptor_available: DmaRegion,
    used: DmaRegion,
    request_header: DmaRegion,
    request_status: DmaRegion,
    read_data: DmaRegion,
    write_data: DmaRegion,
    layout: SplitQueueLayout,
    notify_offset: u16,
    available_index: u16,
    used_index: u16,
    capacity_sectors: u64,
    medium_id: u64,
    read_only: bool,
    flush_supported: bool,
    offline: bool,
    #[cfg(feature = "pci-virtio-block-fault-smoke")]
    fault_injection_pending: bool,
}

impl VirtioBlock {
    async fn initialize(function: ProtocolClientEndpoint) -> Result<Self, String> {
        let capabilities = virtio_pci::discover_function_capabilities(&function)
            .await
            .map_err(|error| format!("Virtio PCI capability discovery failed: {error:?}"))?;
        log::info!(
            "Virtio PCI regions common_bar={} notify_bar={} isr_bar={} device_bar={}",
            capabilities.common.bar,
            capabilities.notify.region.bar,
            capabilities.isr.bar,
            capabilities.device.bar
        );
        let mut transport = VirtioPciTransport::open(function, capabilities)
            .await
            .map_err(|error| format!("Virtio PCI BAR open failed: {error:?}"))?;
        transport
            .reset()
            .await
            .map_err(|error| format!("Virtio device reset failed: {error:?}"))?;
        let identity = read_function_identity(transport.function()).await?;
        let interrupt_lineage = crate::topology::GenerationLineage::from(&identity);
        if interrupt_lineage.firmware_version.is_empty()
            || interrupt_lineage.aml_namespace_generation == 0
            || interrupt_lineage.platform_topology_generation == 0
            || interrupt_lineage.pci_fabric_generation == 0
        {
            return Err("Virtio PCI function has an incomplete generation lineage".into());
        }
        let interrupt_source = route_interrupt(transport.function()).await?;
        let mut interrupt_readiness = servicekit::ReadinessSet::new();
        interrupt_readiness
            .refresh(&[interrupt_source.get()])
            .map_err(|status| format!("register Virtio interrupt source: {status}"))?;
        let offered = transport
            .device_features()
            .map_err(|error| format!("Virtio feature read failed: {error:?}"))?;
        if offered & VIRTIO_F_ACCESS_PLATFORM == 0 {
            return Err("Virtio device lacks ACCESS_PLATFORM for the isolated DMA domain".into());
        }
        let mut accepted = (1 << 32) | VIRTIO_F_ACCESS_PLATFORM;
        if offered & VIRTIO_F_ORDER_PLATFORM != 0 {
            accepted |= VIRTIO_F_ORDER_PLATFORM;
        }
        if offered & VIRTIO_BLK_F_RO != 0 {
            accepted |= VIRTIO_BLK_F_RO;
        }
        if offered & VIRTIO_BLK_F_FLUSH != 0 {
            accepted |= VIRTIO_BLK_F_FLUSH;
        }
        transport
            .accept_features(accepted)
            .await
            .map_err(|error| format!("Virtio feature negotiation failed: {error:?}"))?;

        let capacity_sectors = read_capacity(&transport)?;
        if capacity_sectors == 0 {
            return Err("Virtio block device reported zero capacity".into());
        }
        let read_only = offered & VIRTIO_BLK_F_RO != 0;
        let flush_supported = accepted & VIRTIO_BLK_F_FLUSH != 0;

        let (dma_domain, fault_source) = create_dma_domain(transport.function()).await?;
        let mut fault_readiness = servicekit::ReadinessSet::new();
        fault_readiness
            .refresh(&[fault_source.get()])
            .map_err(|status| format!("register Virtio DMA fault source: {status}"))?;
        let requested_queue_size = selected_queue_size(&mut transport)?;
        let layout = SplitQueueLayout::new(requested_queue_size)
            .map_err(|error| format!("Virtio split-queue geometry invalid: {error:?}"))?;

        let mut descriptor_available = DmaRegion::create(
            &dma_domain,
            QUEUE_DESC_IOVA,
            layout.descriptor_available_bytes,
            DmaDirection::DeviceReads,
        )
        .await
        .map_err(|error| format_dma_error("map descriptor/available ring", error))?;
        let mut used = DmaRegion::create(
            &dma_domain,
            QUEUE_USED_IOVA,
            layout.used_bytes,
            DmaDirection::DeviceReadsAndWrites,
        )
        .await
        .map_err(|error| format_dma_error("map used ring", error))?;
        let request_header = DmaRegion::create(
            &dma_domain,
            REQUEST_HEADER_IOVA,
            DMA_PAGE_BYTES,
            DmaDirection::DeviceReads,
        )
        .await
        .map_err(|error| format_dma_error("map request header", error))?;
        let request_status = DmaRegion::create(
            &dma_domain,
            REQUEST_STATUS_IOVA,
            DMA_PAGE_BYTES,
            DmaDirection::DeviceWrites,
        )
        .await
        .map_err(|error| format_dma_error("map request status", error))?;
        let read_data = DmaRegion::create(
            &dma_domain,
            READ_DATA_IOVA,
            MAX_TRANSFER_BYTES,
            DmaDirection::DeviceWrites,
        )
        .await
        .map_err(|error| format_dma_error("map read data", error))?;
        let write_data = DmaRegion::create(
            &dma_domain,
            WRITE_DATA_IOVA,
            MAX_TRANSFER_BYTES,
            DmaDirection::DeviceReads,
        )
        .await
        .map_err(|error| format_dma_error("map write data", error))?;

        layout
            .clear_descriptor_available(descriptor_available.as_mut_slice())
            .map_err(|error| format!("clear descriptor/available ring: {error:?}"))?;
        layout
            .clear_used(used.as_mut_slice())
            .map_err(|error| format!("clear used ring: {error:?}"))?;
        let available_flags_end = layout
            .available_offset
            .checked_add(2)
            .ok_or_else(|| "Virtio available flags offset overflow".to_string())?;
        descriptor_available
            .as_mut_slice()
            .get_mut(layout.available_offset..available_flags_end)
            .ok_or_else(|| "Virtio available flags are outside the ring".to_string())?
            .copy_from_slice(&0_u16.to_le_bytes());

        let notify_offset = transport
            .configure_queue(
                requested_queue_size,
                0,
                layout
                    .descriptor_address(QUEUE_DESC_IOVA)
                    .map_err(|error| format!("Virtio descriptor address invalid: {error:?}"))?,
                layout
                    .available_address(QUEUE_DESC_IOVA)
                    .map_err(|error| format!("Virtio available address invalid: {error:?}"))?,
                layout
                    .used_address(QUEUE_USED_IOVA)
                    .map_err(|error| format!("Virtio used address invalid: {error:?}"))?,
            )
            .map_err(|error| format!("Virtio queue configuration failed: {error:?}"))?;
        bind_interrupt_source(&interrupt_source).await?;
        set_bus_master(&dma_domain, true).await?;
        transport
            .finish_initialization()
            .map_err(|error| format!("Virtio DRIVER_OK transition failed: {error:?}"))?;
        let status = transport
            .device_status()
            .map_err(|error| format!("Virtio device status read failed: {error:?}"))?;
        if status & 0x04 == 0 || status & 0x80 != 0 {
            return Err(format!(
                "Virtio device rejected DRIVER_OK: status={status:#x}"
            ));
        }

        let medium_id = naos_idl::object_id(transport.function().get())
            .map_err(|error| format!("identify Virtio function capability: {error:?}"))?;
        if medium_id == 0 {
            return Err("Virtio function capability has zero object identity".into());
        }
        log::info!(
            "Virtio block online capacity_sectors={} read_only={} flush={} queue_size={} max_transfer_blocks={} dma=isolated interrupt=msix",
            capacity_sectors,
            read_only,
            flush_supported,
            requested_queue_size,
            MAX_TRANSFER_BLOCKS
        );

        Ok(Self {
            transport,
            dma_domain,
            _fault_source: fault_source,
            fault_readiness,
            interrupt_source: Some(interrupt_source),
            interrupt_readiness,
            interrupt_lineage,
            descriptor_available,
            used,
            request_header,
            request_status,
            read_data,
            write_data,
            layout,
            notify_offset,
            available_index: 0,
            used_index: 0,
            capacity_sectors,
            medium_id,
            read_only,
            flush_supported,
            offline: false,
            #[cfg(feature = "pci-virtio-block-fault-smoke")]
            fault_injection_pending: true,
        })
    }

    fn features(&self) -> u64 {
        (if self.read_only {
            BLOCK_FEATURE_READ_ONLY
        } else {
            0
        }) | if self.flush_supported {
            BLOCK_FEATURE_FLUSH | BLOCK_FEATURE_FUA
        } else {
            0
        }
    }

    fn medium_info(&self) -> factory_idl::BlockMediumInfo {
        factory_idl::BlockMediumInfo {
            medium_id: self.medium_id,
            media_generation: 1,
            logical_block_bytes: BLOCK_BYTES,
            physical_block_bytes: BLOCK_BYTES,
            total_block_count: self.capacity_sectors,
            max_transfer_blocks: MAX_TRANSFER_BLOCKS,
            max_transfer_bytes: MAX_TRANSFER_BYTES as u64,
            max_in_flight: MAX_IN_FLIGHT,
            features: self.features(),
        }
    }

    fn block_info(&self, lease: BlockLease) -> device_idl::BlockInfo {
        let info = self.medium_info();
        device_idl::BlockInfo {
            device_id: lease.device_id,
            media_generation: info.media_generation,
            logical_block_bytes: info.logical_block_bytes,
            physical_block_bytes: info.physical_block_bytes,
            block_count: lease.block_count,
            max_transfer_blocks: info.max_transfer_blocks,
            max_transfer_bytes: info.max_transfer_bytes,
            max_in_flight: info.max_in_flight,
            features: info.features
                | if lease.read_only {
                    BLOCK_FEATURE_READ_ONLY
                } else {
                    0
                },
            medium_id: info.medium_id,
        }
    }

    async fn transfer(
        &mut self,
        request_type: u32,
        sector: u64,
        data_bytes: usize,
    ) -> Result<(), i64> {
        if self.offline {
            return Err(ERR_ENODEV);
        }
        if data_bytes > MAX_TRANSFER_BYTES || data_bytes % BLOCK_BYTES as usize != 0 {
            return Err(ERR_EINVAL);
        }
        let header = self.request_header.as_mut_slice();
        header[0..4].copy_from_slice(&request_type.to_le_bytes());
        header[4..8].fill(0);
        header[8..16].copy_from_slice(&sector.to_le_bytes());
        self.request_status.as_mut_slice()[0] = u8::MAX;

        self.layout
            .write_descriptor(
                self.descriptor_available.as_mut_slice(),
                0,
                Descriptor {
                    address: REQUEST_HEADER_IOVA,
                    length: 16,
                    flags: DESC_F_NEXT,
                    next: 1,
                },
            )
            .map_err(queue_error)?;

        let status_descriptor = if data_bytes == 0 { 1 } else { 2 };
        if data_bytes != 0 {
            let device_writes = request_type == VIRTIO_BLK_T_IN;
            #[cfg(feature = "pci-virtio-block-fault-smoke")]
            let data_iova = if self.fault_injection_pending {
                self.fault_injection_pending = false;
                log::info!("Virtio test injected unmapped DMA IOVA={FAULT_TEST_IOVA:#x}");
                FAULT_TEST_IOVA
            } else if device_writes {
                READ_DATA_IOVA
            } else {
                WRITE_DATA_IOVA
            };
            #[cfg(not(feature = "pci-virtio-block-fault-smoke"))]
            let data_iova = if device_writes {
                READ_DATA_IOVA
            } else {
                WRITE_DATA_IOVA
            };
            self.layout
                .write_descriptor(
                    self.descriptor_available.as_mut_slice(),
                    1,
                    Descriptor {
                        address: data_iova,
                        length: data_bytes as u32,
                        flags: DESC_F_NEXT | if device_writes { DESC_F_WRITE } else { 0 },
                        next: 2,
                    },
                )
                .map_err(queue_error)?;
        }
        self.layout
            .write_descriptor(
                self.descriptor_available.as_mut_slice(),
                status_descriptor,
                Descriptor {
                    address: REQUEST_STATUS_IOVA,
                    length: 1,
                    flags: DESC_F_WRITE,
                    next: 0,
                },
            )
            .map_err(queue_error)?;
        self.available_index = self
            .layout
            .publish_available(
                self.descriptor_available.as_mut_slice(),
                self.available_index,
                0,
            )
            .map_err(queue_error)?;
        if self.transport.notify_queue(0, self.notify_offset).is_err() {
            self.stop_after_queue_error().await;
            return Err(ERR_EIO);
        }
        let deadline = tokio::time::Instant::now() + COMPLETION_TIMEOUT;
        loop {
            let device_index = match self.layout.read_used_index(self.used.as_slice()) {
                Ok(index) => index,
                Err(error) => {
                    log::error!("Virtio used ring became invalid after queue publish: {error:?}");
                    self.stop_after_queue_error().await;
                    return Err(ERR_EIO);
                }
            };
            let pending = device_index.wrapping_sub(self.used_index);
            if pending != 0 {
                if pending != 1 {
                    self.stop_after_queue_error().await;
                    return Err(ERR_EIO);
                }
                let completion = match self
                    .layout
                    .read_used_element(self.used.as_slice(), self.used_index)
                {
                    Ok(completion) => completion,
                    Err(error) => {
                        log::error!("Virtio used element invalid after queue publish: {error:?}");
                        self.stop_after_queue_error().await;
                        return Err(ERR_EIO);
                    }
                };
                self.used_index = device_index;
                let device_written_bytes = if request_type == VIRTIO_BLK_T_IN {
                    data_bytes
                } else {
                    0
                };
                let expected_length =
                    u32::try_from(device_written_bytes.checked_add(1).ok_or(ERR_EIO)?)
                        .map_err(|_| ERR_EIO)?;
                if completion.id != 0 || completion.length != expected_length {
                    log::error!(
                        "Virtio used completion mismatch id={} length={} expected_id=0 expected_length={}",
                        completion.id,
                        completion.length,
                        expected_length
                    );
                    self.stop_after_queue_error().await;
                    return Err(ERR_EIO);
                }
                match self.take_dma_faults_if_ready().await {
                    Ok(false) => {}
                    Ok(true) => {
                        self.stop_after_queue_error().await;
                        return Err(ERR_EIO);
                    }
                    Err(error) => {
                        log::error!("Virtio DMA fault-source read failed: {error}");
                        self.stop_after_queue_error().await;
                        return Err(ERR_EIO);
                    }
                }
                let status = self
                    .request_status
                    .read_volatile_u8(0)
                    .map_err(|_| ERR_EIO)?;
                return match status {
                    VIRTIO_BLK_S_OK => Ok(()),
                    VIRTIO_BLK_S_IOERR => Err(ERR_EIO),
                    VIRTIO_BLK_S_UNSUPP => Err(ERR_EOPNOTSUPP),
                    _ => Err(ERR_EIO),
                };
            }
            if tokio::time::Instant::now() >= deadline {
                self.stop_after_queue_error().await;
                return Err(ERR_EIO);
            }
            let wake = {
                let fault_readiness = &mut self.fault_readiness;
                let interrupt_readiness = &mut self.interrupt_readiness;
                tokio::select! {
                    biased;
                    event = fault_readiness.wait(None) => CompletionWake::DmaFault(event),
                    event = interrupt_readiness.wait(None) => CompletionWake::Interrupt(event),
                    _ = tokio::time::sleep_until(deadline) => CompletionWake::Timeout,
                }
            };
            match wake {
                CompletionWake::DmaFault(Ok(event)) => {
                    self.fault_readiness.clear(event.index);
                    match self.consume_dma_fault_event(event).await {
                        Ok(false) => {}
                        Ok(true) => {
                            self.stop_after_queue_error().await;
                            return Err(ERR_EIO);
                        }
                        Err(error) => {
                            log::error!("Virtio DMA fault-source wait failed: {error}");
                            self.stop_after_queue_error().await;
                            return Err(ERR_EIO);
                        }
                    }
                }
                CompletionWake::DmaFault(Err(status)) => {
                    log::error!("Virtio DMA fault readiness failed: {status}");
                    self.stop_after_queue_error().await;
                    return Err(ERR_EIO);
                }
                CompletionWake::Interrupt(Ok(event)) => {
                    self.interrupt_readiness.clear(event.index);
                    if let Err(error) = self.consume_interrupt_event(event).await {
                        log::error!("Virtio interrupt-source wait failed: {error}");
                        self.stop_after_queue_error().await;
                        return Err(ERR_EIO);
                    }
                }
                CompletionWake::Interrupt(Err(status)) => {
                    log::error!("Virtio interrupt readiness failed: {status}");
                    self.stop_after_queue_error().await;
                    return Err(ERR_EIO);
                }
                CompletionWake::Timeout => {
                    self.stop_after_queue_error().await;
                    return Err(ERR_EIO);
                }
            }
        }
    }

    async fn flush(&mut self) -> Result<(), i64> {
        if self.offline {
            return Err(ERR_ENODEV);
        }
        if !self.flush_supported {
            return Err(ERR_EOPNOTSUPP);
        }
        self.transfer(VIRTIO_BLK_T_FLUSH, 0, 0).await
    }

    async fn take_dma_faults(&mut self, timeout_us: Option<u64>) -> Result<bool, String> {
        let event = match self.fault_readiness.wait(timeout_us).await {
            Ok(event) => event,
            Err(sys::STATUS_WAIT_TIMED_OUT) => return Ok(false),
            Err(status) => return Err(format!("readiness returned status {status}")),
        };
        self.fault_readiness.clear(event.index);
        self.consume_dma_fault_event(event).await
    }

    async fn consume_dma_fault_event(&self, event: servicekit::ReadyEvent) -> Result<bool, String> {
        if event.error || event.read_closed || event.write_closed {
            return Err(format!(
                "fault source closed or failed (read_closed={} write_closed={} error={})",
                event.read_closed, event.write_closed, event.error
            ));
        }
        if !event.readable {
            return Ok(false);
        }

        let request = pci_dma_fault_source::take_request { limit: 16 };
        let mut request_wire = [0_u8; pci_dma_fault_source::TAKE_REQUEST_HEADER_BYTES];
        let mut invocation = pci_dma_fault_source::submit_take(
            &self._fault_source,
            &request,
            ResourceTransferBuilder::new(),
            &mut request_wire,
            0,
        )
        .map_err(|error| format!("submit PciDmaFaultSource.take: {error:?}"))?;
        wait_for_call(invocation.get(), "PciDmaFaultSource.take").await?;
        let mut response_wire = [0_u8; pci_dma_fault_source::TAKE_RESPONSE_HEADER_BYTES + 16 * 12];
        let batch = pci_dma_fault_source::take_take(&mut invocation, &mut response_wire)
            .map_err(|error| format!("take PciDmaFaultSource response: {error:?}"))?;
        if !crate::topology::dma_fault_batch_matches_lineage(&batch, &self.interrupt_lineage) {
            return Err("PciDmaFaultSource returned a stale or foreign generation batch".into());
        }
        if batch.count as usize != batch.records.len() || batch.unit_overflow > 1 {
            return Err("PciDmaFaultSource returned an inconsistent batch".into());
        }
        let faulted = batch.count != 0 || batch.dropped_records != 0 || batch.unit_overflow != 0;
        if faulted {
            for record in &batch.records {
                log::error!(
                    "Virtio DMA fault iova={:#x} requester={:#x} reason={} direction={}",
                    record.iova,
                    record.requester_id,
                    record.reason,
                    record.direction
                );
            }
            log::error!(
                "Virtio DMA fault batch sequence={} count={} dropped={} unit_overflow={}",
                batch.sequence,
                batch.count,
                batch.dropped_records,
                batch.unit_overflow
            );
        }
        Ok(faulted)
    }

    async fn consume_interrupt_event(
        &self,
        readiness: servicekit::ReadyEvent,
    ) -> Result<(), String> {
        let interrupt_source = self
            .interrupt_source
            .as_ref()
            .ok_or_else(|| "Virtio interrupt route is already closed".to_string())?;
        if readiness.error || readiness.read_closed || readiness.write_closed {
            return Err(format!(
                "interrupt source closed or failed (read_closed={} write_closed={} error={})",
                readiness.read_closed, readiness.write_closed, readiness.error
            ));
        }
        if !readiness.readable {
            return Ok(());
        }

        let mut request_wire = [];
        let mut invocation = interrupt_source::submit_wait(
            interrupt_source,
            &interrupt_source::wait_request {},
            ResourceTransferBuilder::new(),
            &mut request_wire,
            0,
        )
        .map_err(|error| format!("submit InterruptSource.wait: {error:?}"))?;
        wait_for_call(invocation.get(), "InterruptSource.wait").await?;
        let mut response_wire = [0_u8; interrupt_source::WAIT_RESPONSE_HEADER_BYTES + 32];
        let response = interrupt_source::take_wait(&mut invocation, &mut response_wire)
            .map_err(|error| format!("take InterruptSource.wait: {error:?}"))?;
        if !crate::topology::interrupt_event_matches_lineage(
            &response.event,
            response.firmware_version,
            &self.interrupt_lineage,
        ) {
            return Err("InterruptSource.wait returned a stale event lineage".into());
        }
        if response.event.flags & (INTERRUPT_FLAG_MASKED | INTERRUPT_FLAG_UNCERTAIN) != 0 {
            return Err(format!(
                "Virtio interrupt route is masked or uncertain: flags={:#x}",
                response.event.flags
            ));
        }
        if response.event.flags & INTERRUPT_FLAG_PENDING == 0 {
            return Ok(());
        }

        let request = interrupt_source::ack_request {
            sequence: response.event.sequence,
        };
        let mut request_wire = [0_u8; interrupt_source::ACK_REQUEST_HEADER_BYTES];
        let mut invocation = interrupt_source::submit_ack(
            interrupt_source,
            &request,
            ResourceTransferBuilder::new(),
            &mut request_wire,
            0,
        )
        .map_err(|error| format!("submit InterruptSource.ack: {error:?}"))?;
        wait_for_call(invocation.get(), "InterruptSource.ack").await?;
        let mut response_wire = [0_u8; interrupt_source::ACK_RESPONSE_HEADER_BYTES + 32];
        let ack = interrupt_source::take_ack(&mut invocation, &mut response_wire)
            .map_err(|error| format!("take InterruptSource.ack: {error:?}"))?;
        if !crate::topology::interrupt_event_matches_lineage(
            &ack.event,
            ack.firmware_version,
            &self.interrupt_lineage,
        ) || ack.event.sequence < response.event.sequence
            || ack.event.flags & (INTERRUPT_FLAG_MASKED | INTERRUPT_FLAG_UNCERTAIN) != 0
        {
            return Err(format!(
                "InterruptSource.ack returned an invalid event: sequence={} flags={:#x}",
                ack.event.sequence, ack.event.flags
            ));
        }
        Ok(())
    }

    async fn take_dma_faults_if_ready(&mut self) -> Result<bool, String> {
        tokio::select! {
            biased;
            faults = self.take_dma_faults(None) => faults,
            _ = tokio::task::yield_now() => Ok(false),
        }
    }

    async fn stop_after_queue_error(&mut self) {
        self.offline = true;
        self.transport.set_failed();
        if let Err(error) = set_bus_master(&self.dma_domain, false).await {
            log::error!("Virtio failure path could not disable bus mastering: {error}");
        }
        if let Err(error) = self.transport.reset().await {
            log::error!("Virtio failure path could not reset the device: {error:?}");
        }
        self.transport.set_failed();
        if let Err(status) = self.interrupt_readiness.refresh(&[]) {
            log::error!("Virtio failure path could not deregister MSI-X readiness: {status}");
        }
        drop(self.interrupt_source.take());
        log::info!("Virtio failure path dropped its MSI-X route capability");
    }
}

pub async fn run(context: Context) -> i64 {
    let function = match servicekit::resolve_resource(
        context.service_directory_handle(),
        servicekit::PCI_FUNCTION_URI,
    ) {
        Ok(handle) => unsafe { ProtocolClientEndpoint::from_raw(handle.into_raw()) },
        Err(error) => {
            log::error!("resolve scoped PciFunction capability failed: {error:?}");
            return 1;
        }
    };
    let block = match VirtioBlock::initialize(function).await {
        Ok(block) => block,
        Err(error) => {
            log::error!("Virtio block initialization failed: {error}");
            return 1;
        }
    };

    let mut server = match Server::publish(
        &context.service_directory(),
        FACTORY_URI,
        &factory_idl::protocol_descriptor(),
        factory_idl::validate_request_transport_resources,
    )
    .await
    {
        Ok(server) => server,
        Err(error) => {
            log::error!("publish Virtio BlockDeviceFactory failed: {error:?}");
            return 1;
        }
    };
    server.set_max_in_flight(MAX_IN_FLIGHT as u32);
    let mut handler = DriverState {
        block,
        leases: Vec::new(),
        next_device_id: 1,
    };
    log::info!("Virtio BlockDeviceFactory published at {FACTORY_URI}");
    server.serve(&mut handler).await as i64
}

impl ServeHandler for DriverState {
    async fn handle(&mut self, server: &mut Server, request: Request) {
        self.reclaim_closed(server);
        let result = if request.protocol_uuid == factory_idl::PROTOCOL_UUID {
            let dispatched = {
                let rpc_request = request.rpc_request();
                factory_idl::dispatch_servicekit(&rpc_request, server, self).await
            };
            server.respond_dispatch(request, dispatched).await
        } else if request.protocol_uuid == device_idl::PROTOCOL_UUID {
            let dispatched = {
                let rpc_request = request.rpc_request();
                let mut context = &request;
                device_idl::dispatch_servicekit(&rpc_request, &mut context, self).await
            };
            server.respond_dispatch(request, dispatched).await
        } else {
            let response = servicekit::server::Response::error(&request, ERR_EACCES);
            server.respond(request, response).await
        };
        if let Err(error) = result {
            log::debug!("Virtio block RPC response failed: {error:?}");
        }
        self.reclaim_closed(server);
        if self.block.offline {
            servicekit::tidy_log::fatal(
                "Virtio Block transport entered a terminal offline state; exiting for supervisor cleanup",
            );
        }
    }
}

impl factory_idl::BlockDeviceFactoryServiceKitHandler<Server> for DriverState {
    type Resource = EndpointResource;

    async fn get_info<'s>(
        &'s mut self,
        _server: &mut Server,
        _request: factory_idl::get_info_request,
    ) -> Result<RpcMethodReply<factory_idl::get_info_response, Self::Resource>, RpcOutcome> {
        if self.block.offline {
            return Err(service_outcome(ERR_ENODEV));
        }
        Ok(RpcMethodReply::new(factory_idl::get_info_response {
            value: self.block.medium_info(),
        }))
    }

    async fn acquire<'s>(
        &'s mut self,
        server: &mut Server,
        value: factory_idl::acquire_request,
    ) -> Result<RpcMethodReply<factory_idl::acquire_response, Self::Resource>, RpcOutcome> {
        if self.block.offline {
            return Err(service_outcome(ERR_ENODEV));
        }
        if value.flags & !ACQUIRE_READ_ONLY != 0 || value.block_count == 0 {
            return Err(service_outcome(ERR_EINVAL));
        }
        let Some(end) = value.start_lba.checked_add(value.block_count) else {
            return Err(service_outcome(ERR_EINVAL));
        };
        if end > self.block.capacity_sectors {
            return Err(service_outcome(ERR_EINVAL));
        }
        if self.leases.len() >= MAX_BLOCK_LEASES || self.leases.try_reserve(1).is_err() {
            return Err(service_outcome(ERR_EBUSY));
        }
        if self.leases.iter().any(|lease| {
            value.start_lba < lease.start_lba.saturating_add(lease.block_count)
                && lease.start_lba < end
        }) {
            return Err(service_outcome(ERR_EBUSY));
        }
        let device_id = self.next_device_id;
        let Some(next_device_id) = device_id.checked_add(1) else {
            return Err(service_outcome(ERR_EOVERFLOW));
        };
        let read_only = self.block.read_only || value.flags & ACQUIRE_READ_ONLY != 0;
        let rights = BLOCK_RIGHT_INSPECT
            | BLOCK_RIGHT_READ
            | if read_only {
                0
            } else {
                BLOCK_RIGHT_WRITE
                    | if self.block.flush_supported {
                        BLOCK_RIGHT_FLUSH
                    } else {
                        0
                    }
            };
        let endpoint = server
            .create_endpoint(
                &device_idl::protocol_descriptor(),
                sys::BINDING_CLIENT_END,
                device_idl::PROTOCOL_SCOPE,
                rights,
                device_idl::validate_request_transport_resources,
            )
            .map_err(|_| service_outcome(ERR_EIO))?;
        let resource_id = endpoint.descriptor().resource_id;
        self.leases.push(BlockLease {
            resource_id,
            device_id,
            start_lba: value.start_lba,
            block_count: value.block_count,
            read_only,
        });
        self.next_device_id = next_device_id;
        let Some(device_slot) = ResourceSlot::new(0) else {
            return Err(service_outcome(ERR_EIO));
        };
        let mut reply = RpcMethodReply::new(factory_idl::acquire_response {
            device: device_slot,
        });
        reply.resources.push(endpoint);
        Ok(reply)
    }
}

impl device_idl::BlockDeviceServiceKitHandler<&Request> for DriverState {
    type Resource = EndpointResource;

    async fn get_info<'s>(
        &'s mut self,
        request: &mut &Request,
        _value: device_idl::get_info_request,
    ) -> Result<RpcMethodReply<device_idl::get_info_response, Self::Resource>, RpcOutcome> {
        if self.block.offline {
            return Err(service_outcome(ERR_ENODEV));
        }
        let Some(lease) = self.target_lease(request, BLOCK_RIGHT_INSPECT) else {
            return Err(service_outcome(ERR_EACCES));
        };
        Ok(RpcMethodReply::new(device_idl::get_info_response {
            value: self.block.block_info(lease),
        }))
    }

    async fn read<'s>(
        &'s mut self,
        request: &mut &Request,
        value: device_idl::read_request,
    ) -> Result<RpcMethodReply<device_idl::read_response, Self::Resource>, RpcOutcome> {
        self.read_request(request, value)
            .await
            .map_err(service_outcome)?;
        Ok(RpcMethodReply::new(device_idl::read_response {}))
    }

    async fn write<'s>(
        &'s mut self,
        request: &mut &Request,
        value: device_idl::write_request,
    ) -> Result<RpcMethodReply<device_idl::write_response, Self::Resource>, RpcOutcome> {
        self.write_request(request, value)
            .await
            .map_err(service_outcome)?;
        Ok(RpcMethodReply::new(device_idl::write_response {}))
    }

    async fn flush<'s>(
        &'s mut self,
        request: &mut &Request,
        _value: device_idl::flush_request,
    ) -> Result<RpcMethodReply<device_idl::flush_response, Self::Resource>, RpcOutcome> {
        if self.block.offline {
            return Err(service_outcome(ERR_ENODEV));
        }
        if self.target_lease(request, BLOCK_RIGHT_FLUSH).is_none() {
            return Err(service_outcome(ERR_EACCES));
        }
        self.block.flush().await.map_err(service_outcome)?;
        Ok(RpcMethodReply::new(device_idl::flush_response {}))
    }

    async fn discard<'s>(
        &'s mut self,
        _request: &mut &Request,
        _value: device_idl::discard_request,
    ) -> Result<RpcMethodReply<device_idl::discard_response, Self::Resource>, RpcOutcome> {
        Err(service_outcome(ERR_EOPNOTSUPP))
    }
}

impl DriverState {
    fn reclaim_closed(&mut self, server: &mut Server) {
        for closed in server.take_closed_resources() {
            self.leases
                .retain(|lease| lease.resource_id != closed.resource_id);
        }
    }

    fn target_lease(&self, request: &Request, required_rights: u64) -> Option<BlockLease> {
        let target = request.target?;
        if !request.validate_resource(target) || target.rights & required_rights != required_rights
        {
            return None;
        }
        self.leases
            .iter()
            .find(|lease| lease.resource_id == target.resource_id)
            .copied()
    }

    async fn read_request(
        &mut self,
        request: &Request,
        value: device_idl::read_request,
    ) -> Result<(), i64> {
        if self.block.offline {
            return Err(ERR_ENODEV);
        }
        let lease = self
            .target_lease(request, BLOCK_RIGHT_READ)
            .ok_or(ERR_EACCES)?;
        let (bytes, sector) = self.validate_range(lease, value.lba, value.block_count)?;
        if value.flags != 0 {
            return Err(ERR_EINVAL);
        }
        if !request.validate_memory(value.buffer, false) {
            return Err(ERR_EACCES);
        }
        self.block.transfer(VIRTIO_BLK_T_IN, sector, bytes).await?;
        let source = &self.block.read_data;
        request
            .with_write_memory(value.buffer, 0, bytes, |destination| {
                source
                    .read_volatile_into(0, destination)
                    .map_err(|_| ERR_EIO)
            })
            .map_err(map_region_error)??;
        Ok(())
    }

    async fn write_request(
        &mut self,
        request: &Request,
        value: device_idl::write_request,
    ) -> Result<(), i64> {
        if self.block.offline {
            return Err(ERR_ENODEV);
        }
        let lease = self
            .target_lease(request, BLOCK_RIGHT_WRITE)
            .ok_or(ERR_EACCES)?;
        if lease.read_only {
            return Err(ERR_EACCES);
        }
        let (bytes, sector) = self.validate_range(lease, value.lba, value.block_count)?;
        if value.flags & !WRITE_FLAG_FUA != 0 {
            return Err(ERR_EINVAL);
        }
        if value.flags & WRITE_FLAG_FUA != 0 && !self.block.flush_supported {
            return Err(ERR_EOPNOTSUPP);
        }
        if !request.validate_memory(value.buffer, true) {
            return Err(ERR_EACCES);
        }
        let destination = &mut self.block.write_data.as_mut_slice()[..bytes];
        request
            .with_read_memory(value.buffer, 0, bytes, |source| {
                destination.copy_from_slice(source);
            })
            .map_err(map_region_error)?;
        self.block.transfer(VIRTIO_BLK_T_OUT, sector, bytes).await?;
        if value.flags & WRITE_FLAG_FUA != 0 {
            self.block.flush().await?;
        }
        Ok(())
    }

    fn validate_range(&self, lease: BlockLease, lba: u64, count: u64) -> Result<(usize, u64), i64> {
        if count == 0 || count > MAX_TRANSFER_BLOCKS {
            return Err(ERR_EINVAL);
        }
        let end = lba.checked_add(count).ok_or(ERR_EINVAL)?;
        if end > lease.block_count {
            return Err(ERR_EINVAL);
        }
        let sector = lease.start_lba.checked_add(lba).ok_or(ERR_EINVAL)?;
        let bytes = count.checked_mul(BLOCK_BYTES).ok_or(ERR_EINVAL)?;
        Ok((usize::try_from(bytes).map_err(|_| ERR_EINVAL)?, sector))
    }
}

fn selected_queue_size(transport: &mut VirtioPciTransport) -> Result<u16, String> {
    if transport.num_queues().map_err(transport_error)? == 0 {
        return Err("Virtio block device has no request queue".into());
    }
    transport.select_queue(0).map_err(transport_error)?;
    let maximum = transport.queue_size().map_err(transport_error)?;
    let limit = maximum.min(QUEUE_SIZE_LIMIT);
    let mut selected = 1_u16;
    while selected <= limit / 2 {
        selected *= 2;
    }
    if selected < 4 {
        return Err(format!(
            "Virtio request queue is too small: maximum={maximum}"
        ));
    }
    Ok(selected)
}

fn read_capacity(transport: &VirtioPciTransport) -> Result<u64, String> {
    for _ in 0..8 {
        let before = transport.config_generation().map_err(transport_error)?;
        let capacity = transport
            .read_device_config_u64(0)
            .map_err(transport_error)?;
        let after = transport.config_generation().map_err(transport_error)?;
        if before == after {
            return Ok(capacity);
        }
    }
    Err("Virtio block capacity changed during every config snapshot".into())
}

async fn read_function_identity(
    endpoint: &ProtocolClientEndpoint,
) -> Result<crate::topology::PciFunctionIdentitySnapshot, String> {
    let mut request_wire = [];
    let mut invocation = pci_function::submit_get_identity(
        endpoint,
        &pci_function::get_identity_request {},
        ResourceTransferBuilder::new(),
        &mut request_wire,
        0,
    )
    .map_err(|error| format!("submit PciFunction.get_identity: {error:?}"))?;
    wait_for_call(invocation.get(), "PciFunction.get_identity").await?;
    let mut response_wire = [0_u8; pci_function::GET_IDENTITY_RESPONSE_HEADER_BYTES + 32];
    pci_function::take_get_identity(&mut invocation, &mut response_wire)
        .map(|response| crate::topology::PciFunctionIdentitySnapshot {
            firmware_version: response.firmware_version.to_owned(),
            value: response.value,
        })
        .map_err(|error| format!("take PciFunction.get_identity: {error:?}"))
}

async fn route_interrupt(
    endpoint: &ProtocolClientEndpoint,
) -> Result<ProtocolClientEndpoint, String> {
    let mut request_wire = [];
    let mut invocation = pci_function::submit_route_interrupt(
        endpoint,
        &pci_function::route_interrupt_request {},
        ResourceTransferBuilder::new(),
        &mut request_wire,
        0,
    )
    .map_err(|error| format!("submit PciFunction.route_interrupt: {error:?}"))?;
    wait_for_call(invocation.get(), "PciFunction.route_interrupt").await?;
    let mut response_wire = [0_u8; pci_function::ROUTE_INTERRUPT_RESPONSE_HEADER_BYTES];
    let (response, mut resources) =
        pci_function::take_route_interrupt(&mut invocation, &mut response_wire)
            .map_err(|error| format!("take PciFunction.route_interrupt: {error:?}"))?;
    let source = resources
        .take(response.source)
        .ok_or_else(|| "PciFunction.route_interrupt omitted InterruptSource".to_string())?;
    // SAFETY: NaoIDL validated the kernel-transferred resource's protocol
    // scope and move disposition before returning the owned handle.
    Ok(unsafe { ProtocolClientEndpoint::from_raw(source.into_raw()) })
}

async fn bind_interrupt_source(endpoint: &ProtocolClientEndpoint) -> Result<(), String> {
    let mut request_wire = [];
    let mut invocation = interrupt_source::submit_bind(
        endpoint,
        &interrupt_source::bind_request {},
        ResourceTransferBuilder::new(),
        &mut request_wire,
        0,
    )
    .map_err(|error| format!("submit InterruptSource.bind: {error:?}"))?;
    wait_for_call(invocation.get(), "InterruptSource.bind").await?;
    let mut response_wire = [];
    interrupt_source::take_bind(&mut invocation, &mut response_wire)
        .map_err(|error| format!("take InterruptSource.bind: {error:?}"))?;
    Ok(())
}

async fn create_dma_domain(
    function: &ProtocolClientEndpoint,
) -> Result<(ProtocolClientEndpoint, ProtocolClientEndpoint), String> {
    let request = pci_function::create_dma_domain_request {};
    let mut request_wire = [];
    let mut invocation = pci_function::submit_create_dma_domain(
        function,
        &request,
        ResourceTransferBuilder::new(),
        &mut request_wire,
        0,
    )
    .map_err(|error| format!("create Virtio PciDmaDomain request: {error:?}"))?;
    wait_for_call(invocation.get(), "create Virtio PciDmaDomain").await?;
    let mut response_wire = [0_u8; pci_function::CREATE_DMA_DOMAIN_RESPONSE_HEADER_BYTES];
    let (response, mut resources) =
        pci_function::take_create_dma_domain(&mut invocation, &mut response_wire)
            .map_err(|error| format!("take Virtio PciDmaDomain response: {error:?}"))?;
    let domain = resources
        .take(response.domain)
        .ok_or_else(|| "PciFunction omitted the DMA domain".to_string())?;
    let faults = resources
        .take(response.faults)
        .ok_or_else(|| "PciFunction omitted the DMA fault source".to_string())?;
    // SAFETY: NaoIDL validated both kernel-transferred handles against their
    // declared protocol scopes before returning the owned resources.
    Ok((
        unsafe { ProtocolClientEndpoint::from_raw(domain.into_raw()) },
        unsafe { ProtocolClientEndpoint::from_raw(faults.into_raw()) },
    ))
}

async fn set_bus_master(domain: &ProtocolClientEndpoint, enabled: bool) -> Result<(), String> {
    let request = pci_dma_domain::set_bus_master_request {
        enabled: u8::from(enabled),
    };
    let mut request_wire = [0_u8; pci_dma_domain::SET_BUS_MASTER_REQUEST_HEADER_BYTES];
    let mut invocation = pci_dma_domain::submit_set_bus_master(
        domain,
        &request,
        ResourceTransferBuilder::new(),
        &mut request_wire,
        0,
    )
    .map_err(|error| format!("set Virtio bus-master request: {error:?}"))?;
    wait_for_call(invocation.get(), "set Virtio bus master").await?;
    let mut response_wire = [0_u8; pci_dma_domain::SET_BUS_MASTER_RESPONSE_HEADER_BYTES];
    pci_dma_domain::take_set_bus_master(&mut invocation, &mut response_wire)
        .map_err(|error| format!("take Virtio bus-master response: {error:?}"))?;
    Ok(())
}

async fn wait_for_call(handle: sys::Handle, operation: &str) -> Result<(), String> {
    let ready = servicekit::wait_ready_async(&[handle], None)
        .await
        .map_err(|status| format!("{operation} readiness failed: {status}"))?;
    if !ready.readable {
        return Err(format!("{operation} completed without a readable result"));
    }
    Ok(())
}

fn map_region_error(error: ServerError) -> i64 {
    match error {
        ServerError::Status(sys::STATUS_INVALID_ARGUMENT) => ERR_EINVAL,
        ServerError::Status(sys::STATUS_ACCESS_DENIED) => ERR_EACCES,
        _ => ERR_EIO,
    }
}

fn service_outcome(protocol_error: i64) -> RpcOutcome {
    RpcOutcome {
        execution: 0,
        reason: 0,
        protocol_error,
    }
}

fn format_dma_error(operation: &str, error: DmaRegionError) -> String {
    format!("{operation} failed: {error:?}")
}

fn transport_error(error: TransportError) -> String {
    format!("{error:?}")
}

fn queue_error(error: QueueError) -> i64 {
    log::error!("Virtio split-queue access failed: {error:?}");
    ERR_EIO
}
