//! Bounded parser for the modern Virtio PCI capability chain.

use core::future::Future;

#[cfg(target_os = "naos")]
use naos_idl::{ProtocolClientEndpoint, ResourceTransferBuilder};

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct BarRegion {
    pub bar: u8,
    pub offset: u32,
    pub length: u32,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct NotifyRegion {
    pub region: BarRegion,
    pub offset_multiplier: u32,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct VirtioPciCapabilities {
    pub common: BarRegion,
    pub notify: NotifyRegion,
    pub isr: BarRegion,
    pub device: BarRegion,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum CapabilityParseError {
    ConfigReadFailed,
    NoCapabilityList,
    InvalidCapabilityPointer,
    CapabilityLoop,
    InvalidCapabilityLength,
    InvalidBar,
    RegionOverflow,
    MissingCapability,
}

const PCI_STATUS_CAPABILITY_LIST: u16 = 1 << 4;
const PCI_CAPABILITY_LIST_OFFSET: u16 = 0x34;
const PCI_CAPABILITY_MIN_OFFSET: u8 = 0x40;
const PCI_CAPABILITY_MAX_OFFSET: u8 = 0xfc;
const PCI_CAPABILITY_VENDOR_SPECIFIC: u8 = 0x09;
const VIRTIO_PCI_CAP_COMMON: u8 = 1;
const VIRTIO_PCI_CAP_NOTIFY: u8 = 2;
const VIRTIO_PCI_CAP_ISR: u8 = 3;
const VIRTIO_PCI_CAP_DEVICE: u8 = 4;

async fn read_config<F, Fut>(
    read: &mut F,
    offset: u16,
    width: u8,
) -> Result<u32, CapabilityParseError>
where
    F: FnMut(u16, u8) -> Fut,
    Fut: Future<Output = Option<u32>>,
{
    read(offset, width)
        .await
        .ok_or(CapabilityParseError::ConfigReadFailed)
}

async fn parse_region<F, Fut>(
    read: &mut F,
    pointer: u8,
    capability_length: u8,
) -> Result<BarRegion, CapabilityParseError>
where
    F: FnMut(u16, u8) -> Fut,
    Fut: Future<Output = Option<u32>>,
{
    if capability_length < 16 {
        return Err(CapabilityParseError::InvalidCapabilityLength);
    }
    let bar = read_config(read, pointer as u16 + 4, 1).await? as u8;
    if bar >= 6 {
        return Err(CapabilityParseError::InvalidBar);
    }
    let offset = read_config(read, pointer as u16 + 8, 4).await?;
    let length = read_config(read, pointer as u16 + 12, 4).await?;
    if length == 0 {
        return Err(CapabilityParseError::InvalidCapabilityLength);
    }
    offset
        .checked_add(length)
        .ok_or(CapabilityParseError::RegionOverflow)?;
    Ok(BarRegion {
        bar,
        offset,
        length,
    })
}

/// Read and validate the device's PCI capability chain using only scalar
/// config reads. The caller supplies a BDF-scoped `PciFunction.read_config`
/// closure; no ECAM or general config-space mapping is exposed.
pub async fn discover_capabilities<F, Fut>(
    mut read: F,
) -> Result<VirtioPciCapabilities, CapabilityParseError>
where
    F: FnMut(u16, u8) -> Fut,
    Fut: Future<Output = Option<u32>>,
{
    let status = read_config(&mut read, 0x06, 2).await? as u16;
    if status & PCI_STATUS_CAPABILITY_LIST == 0 {
        return Err(CapabilityParseError::NoCapabilityList);
    }
    let mut pointer = read_config(&mut read, PCI_CAPABILITY_LIST_OFFSET, 1).await? as u8;
    let mut visited = [false; 64];
    let mut common = None;
    let mut notify = None;
    let mut isr = None;
    let mut device = None;
    let mut steps = 0;

    while pointer != 0 {
        if pointer < PCI_CAPABILITY_MIN_OFFSET
            || pointer > PCI_CAPABILITY_MAX_OFFSET
            || pointer & 3 != 0
        {
            return Err(CapabilityParseError::InvalidCapabilityPointer);
        }
        let slot = (pointer / 4) as usize;
        if visited[slot] {
            return Err(CapabilityParseError::CapabilityLoop);
        }
        visited[slot] = true;
        steps += 1;
        if steps > 48 {
            return Err(CapabilityParseError::CapabilityLoop);
        }

        let id = read_config(&mut read, pointer as u16, 1).await? as u8;
        let next = read_config(&mut read, pointer as u16 + 1, 1).await? as u8;
        if id == PCI_CAPABILITY_VENDOR_SPECIFIC {
            let capability_length = read_config(&mut read, pointer as u16 + 2, 1).await? as u8;
            if capability_length < 4
                || (pointer as u16)
                    .checked_add(capability_length as u16)
                    .is_none_or(|end| end > 256)
            {
                return Err(CapabilityParseError::InvalidCapabilityLength);
            }
            let configuration_type = read_config(&mut read, pointer as u16 + 3, 1).await? as u8;
            if matches!(
                configuration_type,
                VIRTIO_PCI_CAP_COMMON
                    | VIRTIO_PCI_CAP_NOTIFY
                    | VIRTIO_PCI_CAP_ISR
                    | VIRTIO_PCI_CAP_DEVICE
            ) {
                let region = parse_region(&mut read, pointer, capability_length).await?;
                match configuration_type {
                    VIRTIO_PCI_CAP_COMMON if common.is_none() => common = Some(region),
                    VIRTIO_PCI_CAP_NOTIFY if notify.is_none() => {
                        if capability_length < 20 {
                            return Err(CapabilityParseError::InvalidCapabilityLength);
                        }
                        let offset_multiplier =
                            read_config(&mut read, pointer as u16 + 16, 4).await?;
                        notify = Some(NotifyRegion {
                            region,
                            offset_multiplier,
                        });
                    }
                    VIRTIO_PCI_CAP_ISR if isr.is_none() => isr = Some(region),
                    VIRTIO_PCI_CAP_DEVICE if device.is_none() => device = Some(region),
                    _ => {}
                }
            }
        }
        pointer = next;
    }

    Ok(VirtioPciCapabilities {
        common: common.ok_or(CapabilityParseError::MissingCapability)?,
        notify: notify.ok_or(CapabilityParseError::MissingCapability)?,
        isr: isr.ok_or(CapabilityParseError::MissingCapability)?,
        device: device.ok_or(CapabilityParseError::MissingCapability)?,
    })
}

#[cfg(target_os = "naos")]
pub async fn read_function_config(
    endpoint: &ProtocolClientEndpoint,
    offset: u16,
    width: u8,
) -> Result<u32, CapabilityParseError> {
    let request = naos_idl::pci_function::read_config_request { offset, width };
    let mut request_wire = [0_u8; naos_idl::pci_function::READ_CONFIG_REQUEST_HEADER_BYTES];
    let mut invocation = naos_idl::pci_function::submit_read_config(
        endpoint,
        &request,
        ResourceTransferBuilder::new(),
        &mut request_wire,
        0,
    )
    .map_err(|_| CapabilityParseError::ConfigReadFailed)?;
    let ready = servicekit::wait_ready_async(&[invocation.get()], None)
        .await
        .map_err(|_| CapabilityParseError::ConfigReadFailed)?;
    if !ready.readable {
        return Err(CapabilityParseError::ConfigReadFailed);
    }
    let mut response_wire = [0_u8; naos_idl::pci_function::READ_CONFIG_RESPONSE_HEADER_BYTES];
    naos_idl::pci_function::take_read_config(&mut invocation, &mut response_wire)
        .map(|response| response.value)
        .map_err(|_| CapabilityParseError::ConfigReadFailed)
}

#[cfg(target_os = "naos")]
pub async fn discover_function_capabilities(
    endpoint: &ProtocolClientEndpoint,
) -> Result<VirtioPciCapabilities, CapabilityParseError> {
    discover_capabilities(|offset, width| async move {
        read_function_config(endpoint, offset, width).await.ok()
    })
    .await
}

impl VirtioPciCapabilities {
    /// BARs the polling transport accesses, suitable for the function's grant mask.
    pub fn bar_mask(self) -> u32 {
        (1 << self.common.bar) | (1 << self.notify.region.bar) | (1 << self.device.bar)
    }
}
