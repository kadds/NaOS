//! Checked layout and little-endian accessors for a modern split virtqueue.

use core::sync::atomic::{Ordering, fence};

pub const DESC_F_NEXT: u16 = 1;
pub const DESC_F_WRITE: u16 = 2;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum QueueError {
    InvalidQueueSize,
    LayoutOverflow,
    RegionTooSmall,
    DescriptorIndexOutOfRange,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct SplitQueueLayout {
    pub queue_size: u16,
    pub descriptor_offset: usize,
    pub available_offset: usize,
    pub descriptor_available_bytes: usize,
    pub used_bytes: usize,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Descriptor {
    pub address: u64,
    pub length: u32,
    pub flags: u16,
    pub next: u16,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct UsedElement {
    pub id: u32,
    pub length: u32,
}

impl SplitQueueLayout {
    pub fn new(queue_size: u16) -> Result<Self, QueueError> {
        if queue_size == 0 || !queue_size.is_power_of_two() || queue_size > 32768 {
            return Err(QueueError::InvalidQueueSize);
        }
        let descriptor_bytes = usize::from(queue_size)
            .checked_mul(16)
            .ok_or(QueueError::LayoutOverflow)?;
        let descriptor_offset = 0;
        let available_offset = align_up(descriptor_bytes, 2)?;
        let available_bytes = 4_usize
            .checked_add(
                usize::from(queue_size)
                    .checked_mul(2)
                    .ok_or(QueueError::LayoutOverflow)?,
            )
            .ok_or(QueueError::LayoutOverflow)?;
        let descriptor_available_end = available_offset
            .checked_add(available_bytes)
            .ok_or(QueueError::LayoutOverflow)?;
        let descriptor_available_bytes = align_up(descriptor_available_end, 4096)?;
        let used_bytes = align_up(
            4_usize
                .checked_add(
                    usize::from(queue_size)
                        .checked_mul(8)
                        .ok_or(QueueError::LayoutOverflow)?,
                )
                .ok_or(QueueError::LayoutOverflow)?,
            4096,
        )?;
        Ok(Self {
            queue_size,
            descriptor_offset,
            available_offset,
            descriptor_available_bytes,
            used_bytes,
        })
    }

    pub fn descriptor_address(self, queue_iova: u64) -> Result<u64, QueueError> {
        queue_iova
            .checked_add(self.descriptor_offset as u64)
            .ok_or(QueueError::LayoutOverflow)
    }

    pub fn available_address(self, queue_iova: u64) -> Result<u64, QueueError> {
        queue_iova
            .checked_add(self.available_offset as u64)
            .ok_or(QueueError::LayoutOverflow)
    }

    pub fn used_address(self, queue_iova: u64) -> Result<u64, QueueError> {
        Ok(queue_iova)
    }

    /// Clear the device-readable descriptor/available backing before mapping
    /// it with the `device_reads` DMA direction.
    pub fn clear_descriptor_available(self, memory: &mut [u8]) -> Result<(), QueueError> {
        if memory.len() < self.descriptor_available_bytes {
            return Err(QueueError::RegionTooSmall);
        }
        memory[..self.descriptor_available_bytes].fill(0);
        Ok(())
    }

    /// Clear the separate used-ring backing before mapping it bidirectionally.
    pub fn clear_used(self, memory: &mut [u8]) -> Result<(), QueueError> {
        if memory.len() < self.used_bytes {
            return Err(QueueError::RegionTooSmall);
        }
        memory[..self.used_bytes].fill(0);
        Ok(())
    }

    pub fn write_descriptor(
        self,
        memory: &mut [u8],
        index: u16,
        descriptor: Descriptor,
    ) -> Result<(), QueueError> {
        self.check_descriptor(index)?;
        self.check_descriptor_available(memory)?;
        let offset = self.descriptor_offset + usize::from(index) * 16;
        write_u64(memory, offset, descriptor.address)?;
        write_u32(memory, offset + 8, descriptor.length)?;
        write_u16(memory, offset + 12, descriptor.flags)?;
        write_u16(memory, offset + 14, descriptor.next)
    }

    /// Publish one chain head in the available ring, then release-order the
    /// updated index before the caller notifies the device.
    pub fn publish_available(
        self,
        memory: &mut [u8],
        available_index: u16,
        descriptor_head: u16,
    ) -> Result<u16, QueueError> {
        self.check_descriptor(descriptor_head)?;
        self.check_descriptor_available(memory)?;
        let slot = available_index % self.queue_size;
        let ring_entry = self.available_offset + 4 + usize::from(slot) * 2;
        write_u16(memory, ring_entry, descriptor_head)?;
        fence(Ordering::Release);
        let next_index = available_index.wrapping_add(1);
        write_u16(memory, self.available_offset + 2, next_index)?;
        fence(Ordering::SeqCst);
        Ok(next_index)
    }

    pub fn read_used_index(self, used_memory: &[u8]) -> Result<u16, QueueError> {
        self.check_used(used_memory)?;
        let index = read_u16(used_memory, 2)?;
        fence(Ordering::Acquire);
        Ok(index)
    }

    pub fn read_used_element(
        self,
        used_memory: &[u8],
        used_index: u16,
    ) -> Result<UsedElement, QueueError> {
        self.check_used(used_memory)?;
        let slot = used_index % self.queue_size;
        let offset = 4 + usize::from(slot) * 8;
        Ok(UsedElement {
            id: read_u32(used_memory, offset)?,
            length: read_u32(used_memory, offset + 4)?,
        })
    }

    fn check_descriptor(self, index: u16) -> Result<(), QueueError> {
        if index >= self.queue_size {
            Err(QueueError::DescriptorIndexOutOfRange)
        } else {
            Ok(())
        }
    }

    fn check_descriptor_available(self, memory: &[u8]) -> Result<(), QueueError> {
        if memory.len() < self.descriptor_available_bytes {
            Err(QueueError::RegionTooSmall)
        } else {
            Ok(())
        }
    }

    fn check_used(self, memory: &[u8]) -> Result<(), QueueError> {
        if memory.len() < self.used_bytes {
            Err(QueueError::RegionTooSmall)
        } else {
            Ok(())
        }
    }
}

fn align_up(value: usize, alignment: usize) -> Result<usize, QueueError> {
    value
        .checked_add(alignment - 1)
        .map(|rounded| rounded & !(alignment - 1))
        .ok_or(QueueError::LayoutOverflow)
}

fn read_u16(memory: &[u8], offset: usize) -> Result<u16, QueueError> {
    memory
        .get(offset..offset.checked_add(2).ok_or(QueueError::LayoutOverflow)?)
        .ok_or(QueueError::RegionTooSmall)?;
    // SAFETY: queue layout keeps u16 fields naturally aligned and the range
    // check proves that two bytes are readable from this DMA-visible mapping.
    let value = unsafe { memory.as_ptr().add(offset).cast::<u16>().read_volatile() };
    Ok(u16::from_le(value))
}

fn read_u32(memory: &[u8], offset: usize) -> Result<u32, QueueError> {
    memory
        .get(offset..offset.checked_add(4).ok_or(QueueError::LayoutOverflow)?)
        .ok_or(QueueError::RegionTooSmall)?;
    // SAFETY: used-element fields are four-byte aligned; the range check
    // proves that four bytes are readable from this DMA-visible mapping.
    let value = unsafe { memory.as_ptr().add(offset).cast::<u32>().read_volatile() };
    Ok(u32::from_le(value))
}

fn write_u16(memory: &mut [u8], offset: usize, value: u16) -> Result<(), QueueError> {
    memory
        .get(offset..offset.checked_add(2).ok_or(QueueError::LayoutOverflow)?)
        .ok_or(QueueError::RegionTooSmall)?;
    // SAFETY: queue layout keeps u16 fields naturally aligned and the range
    // check proves that two bytes are writable in this DMA-visible mapping.
    unsafe {
        memory
            .as_mut_ptr()
            .add(offset)
            .cast::<u16>()
            .write_volatile(value.to_le());
    }
    Ok(())
}

fn write_u32(memory: &mut [u8], offset: usize, value: u32) -> Result<(), QueueError> {
    memory
        .get(offset..offset.checked_add(4).ok_or(QueueError::LayoutOverflow)?)
        .ok_or(QueueError::RegionTooSmall)?;
    // SAFETY: descriptor and used-element fields are four-byte aligned; the
    // range check proves that four bytes are writable in this mapping.
    unsafe {
        memory
            .as_mut_ptr()
            .add(offset)
            .cast::<u32>()
            .write_volatile(value.to_le());
    }
    Ok(())
}

fn write_u64(memory: &mut [u8], offset: usize, value: u64) -> Result<(), QueueError> {
    memory
        .get(offset..offset.checked_add(8).ok_or(QueueError::LayoutOverflow)?)
        .ok_or(QueueError::RegionTooSmall)?;
    // SAFETY: descriptors place the address field at a naturally aligned
    // eight-byte offset; the range check proves the field is writable.
    unsafe {
        memory
            .as_mut_ptr()
            .add(offset)
            .cast::<u64>()
            .write_volatile(value.to_le());
    }
    Ok(())
}
