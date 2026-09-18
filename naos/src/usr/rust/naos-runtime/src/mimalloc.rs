use core::alloc::{GlobalAlloc, Layout};
use core::ptr;

unsafe extern "C" {
    fn mi_malloc_aligned(size: usize, alignment: usize) -> *mut u8;
    fn mi_zalloc_aligned(size: usize, alignment: usize) -> *mut u8;
    fn mi_realloc_aligned(pointer: *mut u8, size: usize, alignment: usize) -> *mut u8;
    fn mi_free(pointer: *mut u8);
    fn mi_thread_done();
}

pub struct MimallocAllocator;

impl MimallocAllocator {
    pub const fn new() -> Self {
        Self
    }
}

unsafe impl GlobalAlloc for MimallocAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        if layout.size() == 0 {
            return layout.dangling_ptr().as_ptr();
        }
        unsafe { mi_malloc_aligned(layout.size(), layout.align()) }
    }

    unsafe fn alloc_zeroed(&self, layout: Layout) -> *mut u8 {
        if layout.size() == 0 {
            return layout.dangling_ptr().as_ptr();
        }
        unsafe { mi_zalloc_aligned(layout.size(), layout.align()) }
    }

    unsafe fn dealloc(&self, pointer: *mut u8, layout: Layout) {
        if !pointer.is_null() && layout.size() != 0 {
            unsafe { mi_free(pointer) };
        }
    }

    unsafe fn realloc(&self, pointer: *mut u8, layout: Layout, new_size: usize) -> *mut u8 {
        if layout.size() == 0 {
            if new_size == 0 {
                return layout.dangling_ptr().as_ptr();
            }
            let Ok(new_layout) = Layout::from_size_align(new_size, layout.align()) else {
                return ptr::null_mut();
            };
            return unsafe { self.alloc(new_layout) };
        }
        if pointer.is_null() {
            let Ok(new_layout) = Layout::from_size_align(new_size, layout.align()) else {
                return ptr::null_mut();
            };
            return unsafe { self.alloc(new_layout) };
        }
        if new_size == 0 {
            unsafe { mi_free(pointer) };
            return layout.dangling_ptr().as_ptr();
        }
        unsafe { mi_realloc_aligned(pointer, new_size, layout.align()) }
    }
}

pub(crate) unsafe fn thread_done() {
    unsafe { mi_thread_done() };
}

