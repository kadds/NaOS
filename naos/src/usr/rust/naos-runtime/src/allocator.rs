#![cfg_attr(test, allow(dead_code))]

use core::alloc::{GlobalAlloc, Layout};
use core::cmp::max;
use core::mem::{align_of, size_of};
use core::ptr;
use core::sync::atomic::{AtomicBool, Ordering};

use naos_sys as sys;

const PAGE_SIZE: usize = 4096;
const HEADER_MAGIC: u64 = 0x4e41_4f53_5f41_4c4c;

#[repr(C)]
struct AllocationHeader {
    magic: u64,
    base: *mut u8,
    map_len: usize,
    user_len: usize,
}

pub struct NativeAllocator {
    lock: AtomicBool,
}

impl NativeAllocator {
    pub const fn new() -> Self {
        Self {
            lock: AtomicBool::new(false),
        }
    }

    fn acquire(&self) -> AllocationGuard<'_> {
        while self
            .lock
            .compare_exchange(false, true, Ordering::Acquire, Ordering::Relaxed)
            .is_err()
        {
            core::hint::spin_loop();
        }
        AllocationGuard { allocator: self }
    }

    unsafe fn map(&self, layout: Layout) -> *mut u8 {
        if layout.size() == 0 {
            return layout.dangling_ptr().as_ptr();
        }

        let (alignment, map_len) = match mapping_layout(layout) {
            Some(value) => value,
            None => return ptr::null_mut(),
        };

        let _guard = self.acquire();
        let mut frame = sys::MemoryMapFrame {
            struct_size: size_of::<sys::MemoryMapFrame>() as u32,
            flags: sys::MEMORY_MAP_READ | sys::MEMORY_MAP_WRITE,
            length: map_len as u64,
            ..sys::MemoryMapFrame::default()
        };
        let status = unsafe { sys::_na_memory_map(&mut frame) };
        if status != sys::STATUS_OK || frame.address == 0 {
            return ptr::null_mut();
        }

        let base = frame.address as usize;
        let first = match base.checked_add(size_of::<AllocationHeader>()) {
            Some(value) => value,
            None => return unsafe { self.unmap_failed(frame.address as *mut u8, map_len) },
        };
        let user = match align_up(first, alignment) {
            Some(value) => value,
            None => return unsafe { self.unmap_failed(frame.address as *mut u8, map_len) },
        };
        let end = match user.checked_add(layout.size()) {
            Some(value) => value,
            None => return unsafe { self.unmap_failed(frame.address as *mut u8, map_len) },
        };
        let map_end = match base.checked_add(map_len) {
            Some(value) => value,
            None => return unsafe { self.unmap_failed(frame.address as *mut u8, map_len) },
        };
        if end > map_end {
            return unsafe { self.unmap_failed(frame.address as *mut u8, map_len) };
        }

        let header = (user - size_of::<AllocationHeader>()) as *mut AllocationHeader;
        unsafe {
            ptr::write(
                header,
                AllocationHeader {
                    magic: HEADER_MAGIC,
                    base: frame.address as *mut u8,
                    map_len,
                    user_len: layout.size(),
                },
            );
        }
        user as *mut u8
    }

    unsafe fn unmap_failed(&self, address: *mut u8, map_len: usize) -> *mut u8 {
        let mut frame = sys::MemoryUnmapFrame {
            struct_size: size_of::<sys::MemoryUnmapFrame>() as u32,
            address: address as u64,
            length: map_len as u64,
            ..sys::MemoryUnmapFrame::default()
        };
        let _ = unsafe { sys::_na_memory_unmap(&mut frame) };
        ptr::null_mut()
    }

    unsafe fn unmap(&self, pointer: *mut u8) {
        let header = unsafe { pointer.sub(size_of::<AllocationHeader>()) as *mut AllocationHeader };
        let metadata = unsafe { ptr::read(header) };
        if metadata.magic != HEADER_MAGIC || metadata.base.is_null() || metadata.map_len == 0 {
            return;
        }
        let _guard = self.acquire();
        let mut frame = sys::MemoryUnmapFrame {
            struct_size: size_of::<sys::MemoryUnmapFrame>() as u32,
            address: metadata.base as u64,
            length: metadata.map_len as u64,
            ..sys::MemoryUnmapFrame::default()
        };
        let _ = unsafe { sys::_na_memory_unmap(&mut frame) };
    }
}

struct AllocationGuard<'a> {
    allocator: &'a NativeAllocator,
}

impl Drop for AllocationGuard<'_> {
    fn drop(&mut self) {
        self.allocator.lock.store(false, Ordering::Release);
    }
}

unsafe impl GlobalAlloc for NativeAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        unsafe { self.map(layout) }
    }

    unsafe fn alloc_zeroed(&self, layout: Layout) -> *mut u8 {
        let pointer = unsafe { self.map(layout) };
        if !pointer.is_null() && layout.size() != 0 {
            unsafe { ptr::write_bytes(pointer, 0, layout.size()) };
        }
        pointer
    }

    unsafe fn dealloc(&self, pointer: *mut u8, layout: Layout) {
        if !pointer.is_null() && layout.size() != 0 {
            unsafe { self.unmap(pointer) };
        }
    }

    unsafe fn realloc(&self, pointer: *mut u8, layout: Layout, new_size: usize) -> *mut u8 {
        if layout.size() == 0 {
            if new_size == 0 {
                return layout.dangling_ptr().as_ptr();
            }
            let new_layout = match Layout::from_size_align(new_size, layout.align()) {
                Ok(value) => value,
                Err(_) => return ptr::null_mut(),
            };
            return unsafe { self.map(new_layout) };
        }
        if pointer.is_null() {
            let new_layout = match Layout::from_size_align(new_size, layout.align()) {
                Ok(value) => value,
                Err(_) => return ptr::null_mut(),
            };
            return unsafe { self.map(new_layout) };
        }
        if new_size == 0 {
            unsafe { self.unmap(pointer) };
            return layout.dangling_ptr().as_ptr();
        }
        let new_layout = match Layout::from_size_align(new_size, layout.align()) {
            Ok(value) => value,
            Err(_) => return ptr::null_mut(),
        };
        let replacement = unsafe { self.map(new_layout) };
        if replacement.is_null() {
            return ptr::null_mut();
        }
        unsafe { ptr::copy_nonoverlapping(pointer, replacement, layout.size().min(new_size)) };
        unsafe { self.unmap(pointer) };
        replacement
    }
}

fn align_up(value: usize, alignment: usize) -> Option<usize> {
    let mask = alignment.checked_sub(1)?;
    value.checked_add(mask).map(|value| value & !mask)
}

fn round_up(value: usize, alignment: usize) -> Option<usize> {
    align_up(value, alignment)
}

fn mapping_layout(layout: Layout) -> Option<(usize, usize)> {
    let alignment = max(layout.align(), align_of::<AllocationHeader>());
    let overhead = size_of::<AllocationHeader>().checked_add(alignment.checked_sub(1)?)?;
    let requested = overhead.checked_add(layout.size())?;
    let map_len = round_up(requested, PAGE_SIZE)?;
    (map_len as u64 <= sys::MEMORY_MAP_MAX_BYTES).then_some((alignment, map_len))
}

#[cfg(test)]
mod tests {
    use super::{NativeAllocator, align_up, mapping_layout, round_up};
    use core::alloc::Layout;

    #[test]
    fn alignment_and_page_rounding_are_checked() {
        assert_eq!(align_up(1, 16), Some(16));
        assert_eq!(align_up(usize::MAX, 2), None);
        assert_eq!(round_up(4097, 4096), Some(8192));
    }

    #[test]
    fn allocation_size_overflow_and_native_map_limit_fail_closed() {
        assert!(mapping_layout(Layout::from_size_align(isize::MAX as usize, 1).unwrap()).is_none());
        assert!(mapping_layout(Layout::from_size_align(1 << 30, 1).unwrap()).is_none());
        assert!(mapping_layout(Layout::from_size_align(1, 1 << 30).unwrap()).is_none());
        assert!(mapping_layout(Layout::from_size_align(1, 64).unwrap()).is_some());
    }

    #[test]
    fn zero_sized_deallocation_does_not_inspect_dangling_pointer() {
        let allocator = NativeAllocator::new();
        let layout = Layout::from_size_align(0, 64).unwrap();
        unsafe {
            <NativeAllocator as core::alloc::GlobalAlloc>::dealloc(
                &allocator,
                layout.dangling_ptr().as_ptr(),
                layout,
            )
        };
    }
}
