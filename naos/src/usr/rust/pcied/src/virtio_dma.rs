//! Explicitly managed, directionally mapped DMA buffers for Virtio devices.

use naos_idl::pci_dma_domain;
use naos_idl::{ProtocolClientEndpoint, ResourceTransferBuilder};
use servicekit::memory::{MEMORY_PAGE_SIZE, MappedMemory, MemoryError, MemoryObject};
use servicekit::sys;

const MAX_DMA_REGION_BYTES: usize = 16 * 1024 * 1024;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum DmaDirection {
    DeviceReads,
    DeviceWrites,
    DeviceReadsAndWrites,
}

impl DmaDirection {
    fn wire_value(self) -> u32 {
        match self {
            Self::DeviceReads => 0,
            Self::DeviceWrites => 1,
            Self::DeviceReadsAndWrites => 2,
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum DmaRegionError {
    InvalidRange,
    TooLarge,
    Memory(MemoryError),
    RegionBounds,
    Protocol,
    MapFailed,
    MapRollbackFailed,
    UnmapFailed,
}

/// Owns a CPU mapping and its per-page IOMMU mappings. It must be closed
/// explicitly before the `PciDmaDomain`; Drop cannot issue teardown RPCs. If
/// close reports an error, close the whole domain before freeing/reusing any
/// IOVA covered by this region.
pub struct DmaRegion {
    object: MemoryObject,
    mapping: MappedMemory,
    iova: u64,
    size: usize,
    mapped_pages: usize,
    direction: DmaDirection,
}

impl DmaRegion {
    pub async fn create(
        domain: &ProtocolClientEndpoint,
        iova: u64,
        size: usize,
        direction: DmaDirection,
    ) -> Result<Self, DmaRegionError> {
        let page_size = MEMORY_PAGE_SIZE;
        if iova == 0 || iova % page_size as u64 != 0 || size == 0 || size % page_size != 0 {
            return Err(DmaRegionError::InvalidRange);
        }
        if size > MAX_DMA_REGION_BYTES {
            return Err(DmaRegionError::TooLarge);
        }
        iova.checked_add(size as u64)
            .ok_or(DmaRegionError::InvalidRange)?;

        let object = MemoryObject::new(size).map_err(DmaRegionError::Memory)?;
        let mut mapping = object
            .map_all(sys::MEMORY_MAP_READ | sys::MEMORY_MAP_WRITE | sys::MEMORY_MAP_SHARED)
            .map_err(DmaRegionError::Memory)?;
        // Materialize every backing frame before granting DMA access. The
        // kernel rejects lazy or unready pages at PciDmaDomain.map_page.
        mapping.as_mut_slice().fill(0);

        let pages = size / page_size;
        let mut region = Self {
            object,
            mapping,
            iova,
            size,
            mapped_pages: 0,
            direction,
        };
        while region.mapped_pages < pages {
            let offset = region
                .mapped_pages
                .checked_mul(page_size)
                .ok_or(DmaRegionError::InvalidRange)?;
            let page_iova = iova
                .checked_add(offset as u64)
                .ok_or(DmaRegionError::InvalidRange)?;
            if let Err(error) = map_page(domain, &region.object, page_iova, offset, direction).await
            {
                if region.unmap_prefix(domain).await.is_err() {
                    return Err(DmaRegionError::MapRollbackFailed);
                }
                return Err(error);
            }
            region.mapped_pages += 1;
        }
        Ok(region)
    }

    pub fn iova(&self) -> u64 {
        self.iova
    }

    pub fn size(&self) -> usize {
        self.size
    }

    pub fn direction(&self) -> DmaDirection {
        self.direction
    }

    pub fn as_slice(&self) -> &[u8] {
        self.mapping.as_slice()
    }

    pub fn as_mut_slice(&mut self) -> &mut [u8] {
        self.mapping.as_mut_slice()
    }

    pub fn read_volatile_u8(&self, offset: usize) -> Result<u8, DmaRegionError> {
        let byte = self
            .mapping
            .as_slice()
            .get(offset)
            .ok_or(DmaRegionError::RegionBounds)?;
        // SAFETY: the checked byte is inside the live DMA mapping; u8 has no
        // alignment requirement, and volatile observes device-written state.
        Ok(unsafe { core::ptr::read_volatile(byte) })
    }

    pub fn read_volatile_into(
        &self,
        offset: usize,
        destination: &mut [u8],
    ) -> Result<(), DmaRegionError> {
        let end = offset
            .checked_add(destination.len())
            .ok_or(DmaRegionError::InvalidRange)?;
        let source = self
            .mapping
            .as_slice()
            .get(offset..end)
            .ok_or(DmaRegionError::RegionBounds)?;
        for (destination_byte, source_byte) in destination.iter_mut().zip(source) {
            // SAFETY: each checked byte is inside the live DMA mapping; u8
            // has no alignment requirement and is loaded volatilely.
            *destination_byte = unsafe { core::ptr::read_volatile(source_byte) };
        }
        Ok(())
    }

    pub async fn close(mut self, domain: &ProtocolClientEndpoint) -> Result<(), DmaRegionError> {
        self.unmap_prefix(domain).await
    }

    async fn unmap_prefix(
        &mut self,
        domain: &ProtocolClientEndpoint,
    ) -> Result<(), DmaRegionError> {
        while self.mapped_pages != 0 {
            let page_index = self.mapped_pages - 1;
            let offset = page_index
                .checked_mul(MEMORY_PAGE_SIZE)
                .ok_or(DmaRegionError::InvalidRange)?;
            let page_iova = self
                .iova
                .checked_add(offset as u64)
                .ok_or(DmaRegionError::InvalidRange)?;
            unmap_page(domain, page_iova).await?;
            self.mapped_pages = page_index;
        }
        Ok(())
    }
}

async fn map_page(
    domain: &ProtocolClientEndpoint,
    backing: &MemoryObject,
    iova: u64,
    object_offset: usize,
    direction: DmaDirection,
) -> Result<(), DmaRegionError> {
    let mut resources = ResourceTransferBuilder::new();
    let backing_slot = resources
        .push_duplicate(backing.as_handle())
        .map_err(|_| DmaRegionError::Protocol)?;
    let request = pci_dma_domain::map_page_request {
        iova,
        object_offset: object_offset as u64,
        direction: direction.wire_value(),
        backing: backing_slot,
    };
    let mut request_wire = [0_u8; pci_dma_domain::MAP_PAGE_REQUEST_HEADER_BYTES];
    let mut invocation =
        pci_dma_domain::submit_map_page(domain, &request, resources, &mut request_wire, 0)
            .map_err(|_| DmaRegionError::Protocol)?;
    let ready = servicekit::wait_ready_async(&[invocation.get()], None)
        .await
        .map_err(|_| DmaRegionError::Protocol)?;
    if !ready.readable {
        return Err(DmaRegionError::Protocol);
    }
    pci_dma_domain::take_map_page(&mut invocation, &mut [])
        .map_err(|_| DmaRegionError::MapFailed)?;
    Ok(())
}

async fn unmap_page(domain: &ProtocolClientEndpoint, iova: u64) -> Result<(), DmaRegionError> {
    let request = pci_dma_domain::unmap_page_request { iova };
    let mut request_wire = [0_u8; pci_dma_domain::UNMAP_PAGE_REQUEST_HEADER_BYTES];
    let mut invocation = pci_dma_domain::submit_unmap_page(
        domain,
        &request,
        ResourceTransferBuilder::new(),
        &mut request_wire,
        0,
    )
    .map_err(|_| DmaRegionError::Protocol)?;
    let ready = servicekit::wait_ready_async(&[invocation.get()], None)
        .await
        .map_err(|_| DmaRegionError::Protocol)?;
    if !ready.readable {
        return Err(DmaRegionError::Protocol);
    }
    pci_dma_domain::take_unmap_page(&mut invocation, &mut [])
        .map_err(|_| DmaRegionError::UnmapFailed)?;
    Ok(())
}
