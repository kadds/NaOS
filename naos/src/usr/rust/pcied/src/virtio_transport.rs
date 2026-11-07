//! Modern Virtio PCI BAR access through an owner-granted `PciFunction`.

use crate::virtio_pci::{BarRegion, VirtioPciCapabilities};
use naos_idl::pci_function;
use naos_idl::{ProtocolClientEndpoint, ResourceTransferBuilder};
use servicekit::memory::{MappedMemory, MemoryObject};
use servicekit::{sys, wait_ready_async};
use std::array;

const COMMON_CFG_MIN_BYTES: u32 = 56;
const DEVICE_CFG_MIN_BYTES: u32 = 8;
const PCI_CFG_NO_VECTOR: u16 = u16::MAX;

const COMMON_DEVICE_FEATURE_SELECT: u32 = 0x00;
const COMMON_DEVICE_FEATURE: u32 = 0x04;
const COMMON_DRIVER_FEATURE_SELECT: u32 = 0x08;
const COMMON_DRIVER_FEATURE: u32 = 0x0c;
const COMMON_CONFIG_MSIX_VECTOR: u32 = 0x10;
const COMMON_NUM_QUEUES: u32 = 0x12;
const COMMON_DEVICE_STATUS: u32 = 0x14;
const COMMON_CONFIG_GENERATION: u32 = 0x15;
const COMMON_QUEUE_SELECT: u32 = 0x16;
const COMMON_QUEUE_SIZE: u32 = 0x18;
const COMMON_QUEUE_MSIX_VECTOR: u32 = 0x1a;
const COMMON_QUEUE_ENABLE: u32 = 0x1c;
const COMMON_QUEUE_NOTIFY_OFF: u32 = 0x1e;
const COMMON_QUEUE_DESC: u32 = 0x20;
const COMMON_QUEUE_DRIVER: u32 = 0x28;
const COMMON_QUEUE_DEVICE: u32 = 0x30;

const STATUS_ACKNOWLEDGE: u8 = 1;
const STATUS_DRIVER: u8 = 2;
const STATUS_DRIVER_OK: u8 = 4;
const STATUS_FEATURES_OK: u8 = 8;
const STATUS_FAILED: u8 = 128;
const VIRTIO_F_VERSION_1: u64 = 1 << 32;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum TransportError {
    InvalidBar,
    RegionBounds,
    InvalidWidth,
    MissingBar,
    Map,
    UnsupportedFeature,
    QueueUnavailable,
    QueueInterruptUnavailable,
    QueueGeometry,
    QueueTimeout,
}

struct Bar {
    _object: MemoryObject,
    mapping: MappedMemory,
    bytes: usize,
}

pub struct VirtioPciTransport {
    function: ProtocolClientEndpoint,
    capabilities: VirtioPciCapabilities,
    bars: [Option<Bar>; 6],
}

impl VirtioPciTransport {
    pub async fn open(
        function: ProtocolClientEndpoint,
        capabilities: VirtioPciCapabilities,
    ) -> Result<Self, TransportError> {
        validate_capability_regions(capabilities)?;
        let mask = capabilities.bar_mask();
        let mut bars: [Option<Bar>; 6] = array::from_fn(|_| None);
        for index in 0..6_u32 {
            if mask & (1 << index) == 0 {
                continue;
            }
            bars[index as usize] = Some(open_bar(&function, index).await?);
        }
        let result = Self {
            function,
            capabilities,
            bars,
        };
        result.validate_regions()?;
        Ok(result)
    }

    pub fn function(&self) -> &ProtocolClientEndpoint {
        &self.function
    }

    pub async fn reset(&mut self) -> Result<(), TransportError> {
        self.write8(self.capabilities.common, COMMON_DEVICE_STATUS, 0)?;
        tokio::time::timeout(core::time::Duration::from_secs(1), async {
            loop {
                if self.read8(self.capabilities.common, COMMON_DEVICE_STATUS)? == 0 {
                    return Ok(());
                }
                tokio::task::yield_now().await;
            }
        })
        .await
        .map_err(|_| TransportError::QueueTimeout)?
    }

    pub async fn accept_features(&mut self, features: u64) -> Result<u64, TransportError> {
        self.write8(
            self.capabilities.common,
            COMMON_DEVICE_STATUS,
            STATUS_ACKNOWLEDGE,
        )?;
        self.write8(
            self.capabilities.common,
            COMMON_DEVICE_STATUS,
            STATUS_ACKNOWLEDGE | STATUS_DRIVER,
        )?;

        let offered = self.device_features()?;
        if features & !offered != 0 {
            self.fail_device();
            return Err(TransportError::UnsupportedFeature);
        }
        if features & VIRTIO_F_VERSION_1 == 0 {
            self.fail_device();
            return Err(TransportError::UnsupportedFeature);
        }
        self.write_driver_features(features)?;
        self.write8(
            self.capabilities.common,
            COMMON_DEVICE_STATUS,
            STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK,
        )?;
        let status = self.read8(self.capabilities.common, COMMON_DEVICE_STATUS)?;
        if status & STATUS_FEATURES_OK == 0 {
            self.fail_device();
            return Err(TransportError::UnsupportedFeature);
        }
        self.write16(
            self.capabilities.common,
            COMMON_CONFIG_MSIX_VECTOR,
            PCI_CFG_NO_VECTOR,
        )?;
        Ok(features)
    }

    pub fn finish_initialization(&mut self) -> Result<(), TransportError> {
        let status = self.read8(self.capabilities.common, COMMON_DEVICE_STATUS)?;
        self.write8(
            self.capabilities.common,
            COMMON_DEVICE_STATUS,
            status | STATUS_DRIVER_OK,
        )
    }

    pub fn device_features(&mut self) -> Result<u64, TransportError> {
        self.write32(self.capabilities.common, COMMON_DEVICE_FEATURE_SELECT, 0)?;
        let low = self.read32(self.capabilities.common, COMMON_DEVICE_FEATURE)? as u64;
        self.write32(self.capabilities.common, COMMON_DEVICE_FEATURE_SELECT, 1)?;
        let high = self.read32(self.capabilities.common, COMMON_DEVICE_FEATURE)? as u64;
        Ok(low | (high << 32))
    }

    pub fn num_queues(&self) -> Result<u16, TransportError> {
        self.read16(self.capabilities.common, COMMON_NUM_QUEUES)
    }

    pub fn device_status(&self) -> Result<u8, TransportError> {
        self.read8(self.capabilities.common, COMMON_DEVICE_STATUS)
    }

    pub fn config_generation(&self) -> Result<u8, TransportError> {
        self.read8(self.capabilities.common, COMMON_CONFIG_GENERATION)
    }

    pub fn read_device_config_u64(&self, offset: u32) -> Result<u64, TransportError> {
        self.read64(self.capabilities.device, offset)
    }

    pub fn select_queue(&mut self, queue: u16) -> Result<(), TransportError> {
        self.write16(self.capabilities.common, COMMON_QUEUE_SELECT, queue)
    }

    pub fn queue_size(&self) -> Result<u16, TransportError> {
        self.read16(self.capabilities.common, COMMON_QUEUE_SIZE)
    }

    pub fn configure_queue(
        &mut self,
        queue_size: u16,
        msix_vector: u16,
        descriptor_iova: u64,
        available_iova: u64,
        used_iova: u64,
    ) -> Result<u16, TransportError> {
        let maximum = self.queue_size()?;
        if maximum == 0 {
            return Err(TransportError::QueueUnavailable);
        }
        if queue_size == 0 || !queue_size.is_power_of_two() || queue_size > maximum {
            return Err(TransportError::QueueGeometry);
        }
        if descriptor_iova == 0
            || available_iova == 0
            || used_iova == 0
            || descriptor_iova & 0xf != 0
            || available_iova & 0x1 != 0
            || used_iova & 0x3 != 0
        {
            return Err(TransportError::QueueGeometry);
        }
        self.write16(self.capabilities.common, COMMON_QUEUE_SIZE, queue_size)?;
        self.write16(
            self.capabilities.common,
            COMMON_QUEUE_MSIX_VECTOR,
            msix_vector,
        )?;
        if self.read16(self.capabilities.common, COMMON_QUEUE_MSIX_VECTOR)? != msix_vector {
            return Err(TransportError::QueueInterruptUnavailable);
        }
        self.write64(self.capabilities.common, COMMON_QUEUE_DESC, descriptor_iova)?;
        self.write64(
            self.capabilities.common,
            COMMON_QUEUE_DRIVER,
            available_iova,
        )?;
        self.write64(self.capabilities.common, COMMON_QUEUE_DEVICE, used_iova)?;
        self.write16(self.capabilities.common, COMMON_QUEUE_ENABLE, 1)?;
        let notify_offset = self.read16(self.capabilities.common, COMMON_QUEUE_NOTIFY_OFF)?;
        Ok(notify_offset)
    }

    pub fn notify_queue(
        &mut self,
        queue: u16,
        queue_notify_off: u16,
    ) -> Result<(), TransportError> {
        let relative = u32::from(queue_notify_off)
            .checked_mul(self.capabilities.notify.offset_multiplier)
            .ok_or(TransportError::RegionBounds)?;
        let end = relative
            .checked_add(2)
            .ok_or(TransportError::RegionBounds)?;
        if end > self.capabilities.notify.region.length {
            return Err(TransportError::RegionBounds);
        }
        self.write16(self.capabilities.notify.region, relative, queue)
    }

    pub fn set_failed(&mut self) {
        self.fail_device();
    }

    fn fail_device(&mut self) {
        if let Ok(status) = self.read8(self.capabilities.common, COMMON_DEVICE_STATUS) {
            let _ = self.write8(
                self.capabilities.common,
                COMMON_DEVICE_STATUS,
                status | STATUS_FAILED,
            );
        }
    }

    fn validate_regions(&self) -> Result<(), TransportError> {
        for region in [
            self.capabilities.common,
            self.capabilities.notify.region,
            self.capabilities.device,
        ] {
            let bar = self.bars[region.bar as usize]
                .as_ref()
                .ok_or(TransportError::MissingBar)?;
            let end = (region.offset as usize)
                .checked_add(region.length as usize)
                .ok_or(TransportError::RegionBounds)?;
            if end > bar.bytes {
                return Err(TransportError::RegionBounds);
            }
        }
        if self.capabilities.common.length < COMMON_CFG_MIN_BYTES
            || self.capabilities.device.length < DEVICE_CFG_MIN_BYTES
            || self.capabilities.notify.region.length < 2
            || self.capabilities.isr.length < 1
        {
            return Err(TransportError::RegionBounds);
        }
        Ok(())
    }

    fn bar_region(
        &self,
        region: BarRegion,
        offset: u32,
        width: usize,
    ) -> Result<(*const u8, usize), TransportError> {
        if offset as usize % width != 0 {
            return Err(TransportError::InvalidWidth);
        }
        let relative_end = (offset as usize)
            .checked_add(width)
            .ok_or(TransportError::RegionBounds)?;
        if relative_end > region.length as usize {
            return Err(TransportError::RegionBounds);
        }
        let absolute = (region.offset as usize)
            .checked_add(offset as usize)
            .ok_or(TransportError::RegionBounds)?;
        if absolute % width != 0 {
            return Err(TransportError::InvalidWidth);
        }
        let bar = self.bars[region.bar as usize]
            .as_ref()
            .ok_or(TransportError::MissingBar)?;
        let end = absolute
            .checked_add(width)
            .ok_or(TransportError::RegionBounds)?;
        if end > bar.bytes {
            return Err(TransportError::RegionBounds);
        }
        Ok((bar.mapping.as_ptr(), absolute))
    }

    fn bar_region_mut(
        &mut self,
        region: BarRegion,
        offset: u32,
        width: usize,
    ) -> Result<(*mut u8, usize), TransportError> {
        if offset as usize % width != 0 {
            return Err(TransportError::InvalidWidth);
        }
        let relative_end = (offset as usize)
            .checked_add(width)
            .ok_or(TransportError::RegionBounds)?;
        if relative_end > region.length as usize {
            return Err(TransportError::RegionBounds);
        }
        let absolute = (region.offset as usize)
            .checked_add(offset as usize)
            .ok_or(TransportError::RegionBounds)?;
        if absolute % width != 0 {
            return Err(TransportError::InvalidWidth);
        }
        let bar = self.bars[region.bar as usize]
            .as_mut()
            .ok_or(TransportError::MissingBar)?;
        let end = absolute
            .checked_add(width)
            .ok_or(TransportError::RegionBounds)?;
        if end > bar.bytes {
            return Err(TransportError::RegionBounds);
        }
        Ok((bar.mapping.as_mut_ptr(), absolute))
    }

    fn read8(&self, region: BarRegion, offset: u32) -> Result<u8, TransportError> {
        let (base, offset) = self.bar_region(region, offset, 1)?;
        // SAFETY: bar_region proves the byte lies in the live mapped BAR.
        Ok(unsafe { base.add(offset).read_volatile() })
    }

    fn read16(&self, region: BarRegion, offset: u32) -> Result<u16, TransportError> {
        let (base, offset) = self.bar_region(region, offset, 2)?;
        // SAFETY: bar_region proves the aligned word lies in the live mapped BAR.
        let value = unsafe { base.add(offset).cast::<u16>().read_volatile() };
        Ok(u16::from_le(value))
    }

    fn read32(&self, region: BarRegion, offset: u32) -> Result<u32, TransportError> {
        let (base, offset) = self.bar_region(region, offset, 4)?;
        // SAFETY: bar_region proves the aligned dword lies in the live mapped BAR.
        let value = unsafe { base.add(offset).cast::<u32>().read_volatile() };
        Ok(u32::from_le(value))
    }

    fn read64(&self, region: BarRegion, offset: u32) -> Result<u64, TransportError> {
        let (base, offset) = self.bar_region(region, offset, 8)?;
        // SAFETY: bar_region proves the aligned qword lies in the live mapped BAR.
        let value = unsafe { base.add(offset).cast::<u64>().read_volatile() };
        Ok(u64::from_le(value))
    }

    fn write8(&mut self, region: BarRegion, offset: u32, value: u8) -> Result<(), TransportError> {
        let (base, offset) = self.bar_region_mut(region, offset, 1)?;
        // SAFETY: bar_region proves the byte lies in the live writable BAR mapping.
        unsafe { base.add(offset).write_volatile(value) };
        Ok(())
    }

    fn write16(
        &mut self,
        region: BarRegion,
        offset: u32,
        value: u16,
    ) -> Result<(), TransportError> {
        let (base, offset) = self.bar_region_mut(region, offset, 2)?;
        // SAFETY: bar_region proves the aligned word lies in the live writable BAR mapping.
        unsafe { base.add(offset).cast::<u16>().write_volatile(value.to_le()) };
        Ok(())
    }

    fn write32(
        &mut self,
        region: BarRegion,
        offset: u32,
        value: u32,
    ) -> Result<(), TransportError> {
        let (base, offset) = self.bar_region_mut(region, offset, 4)?;
        // SAFETY: bar_region proves the aligned dword lies in the live writable BAR mapping.
        unsafe { base.add(offset).cast::<u32>().write_volatile(value.to_le()) };
        Ok(())
    }

    fn write64(
        &mut self,
        region: BarRegion,
        offset: u32,
        value: u64,
    ) -> Result<(), TransportError> {
        let (base, offset) = self.bar_region_mut(region, offset, 8)?;
        // SAFETY: bar_region proves the aligned qword lies in the live writable BAR mapping.
        unsafe { base.add(offset).cast::<u64>().write_volatile(value.to_le()) };
        Ok(())
    }

    fn write_driver_features(&mut self, features: u64) -> Result<(), TransportError> {
        self.write32(self.capabilities.common, COMMON_DRIVER_FEATURE_SELECT, 0)?;
        self.write32(
            self.capabilities.common,
            COMMON_DRIVER_FEATURE,
            features as u32,
        )?;
        self.write32(self.capabilities.common, COMMON_DRIVER_FEATURE_SELECT, 1)?;
        self.write32(
            self.capabilities.common,
            COMMON_DRIVER_FEATURE,
            (features >> 32) as u32,
        )
    }
}

async fn open_bar(function: &ProtocolClientEndpoint, index: u32) -> Result<Bar, TransportError> {
    let request = pci_function::open_bar_request { index };
    let mut request_wire = [0_u8; pci_function::OPEN_BAR_REQUEST_HEADER_BYTES];
    let mut invocation = pci_function::submit_open_bar(
        function,
        &request,
        ResourceTransferBuilder::new(),
        &mut request_wire,
        0,
    )
    .map_err(|_| TransportError::InvalidBar)?;
    let ready = wait_ready_async(&[invocation.get()], None)
        .await
        .map_err(|_| TransportError::InvalidBar)?;
    if !ready.readable {
        return Err(TransportError::InvalidBar);
    }
    let mut response_wire = [0_u8; pci_function::OPEN_BAR_RESPONSE_HEADER_BYTES];
    let (response, mut resources) =
        pci_function::take_open_bar(&mut invocation, &mut response_wire)
            .map_err(|_| TransportError::InvalidBar)?;
    let bytes = usize::try_from(response.length).map_err(|_| TransportError::InvalidBar)?;
    if bytes == 0 {
        return Err(TransportError::InvalidBar);
    }
    let handle = resources
        .take(response.mapping)
        .ok_or(TransportError::InvalidBar)?;
    let object = MemoryObject::from_owned(handle);
    if object.size() != Some(response.length) {
        return Err(TransportError::InvalidBar);
    }
    let mapping = object
        .map_all(sys::MEMORY_MAP_READ | sys::MEMORY_MAP_WRITE | sys::MEMORY_MAP_SHARED)
        .map_err(|_| TransportError::Map)?;
    Ok(Bar {
        _object: object,
        mapping,
        bytes,
    })
}

fn validate_capability_regions(capabilities: VirtioPciCapabilities) -> Result<(), TransportError> {
    for region in [
        capabilities.common,
        capabilities.notify.region,
        capabilities.isr,
        capabilities.device,
    ] {
        if region.bar >= 6 || region.length == 0 {
            return Err(TransportError::InvalidBar);
        }
        if region.offset.checked_add(region.length).is_none() {
            return Err(TransportError::RegionBounds);
        }
    }
    Ok(())
}
