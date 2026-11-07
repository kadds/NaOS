use alloc::sync::Arc;
use core::alloc::{GlobalAlloc, Layout};
use core::marker::PhantomData;
use core::ptr::{self, NonNull};
use core::sync::atomic::{AtomicUsize, Ordering};
use naos_sys::MEMORY_MAP_MAX_BYTES;

#[cfg(target_os = "naos")]
use core::alloc::{AllocError, Allocator};

#[cfg(not(test))]
use crate::ThreadLocal;

unsafe extern "C" {
    fn mi_malloc_aligned(size: usize, alignment: usize) -> *mut u8;
    fn mi_zalloc_aligned(size: usize, alignment: usize) -> *mut u8;
    fn mi_free(pointer: *mut u8);
    fn mi_thread_done();
}

#[cfg(not(test))]
static ACTIVE_HEAP_QUOTA: ThreadLocal<usize> = ThreadLocal::new(1);
#[cfg(not(test))]
static ACTIVE_HEAP_QUOTA_DEPTH: ThreadLocal<usize> = ThreadLocal::new(2);

#[cfg(test)]
std::thread_local! {
    static TEST_ACTIVE_HEAP_QUOTA: core::cell::Cell<usize> = const {
        core::cell::Cell::new(0)
    };
    static TEST_ACTIVE_HEAP_QUOTA_DEPTH: core::cell::Cell<usize> = const {
        core::cell::Cell::new(0)
    };
}

/// A live-allocation quota used by one bounded executor domain.
///
/// Allocations made through an allocator captured from an active
/// [`HeapQuotaScope`] remain charged until their matching deallocation, even
/// if ownership crosses the thread boundary or the scope itself has ended.
pub struct HeapQuota {
    maximum_bytes: usize,
    used_bytes: AtomicUsize,
}

impl HeapQuota {
    pub fn new(maximum_bytes: usize) -> Arc<Self> {
        Arc::new(Self {
            maximum_bytes,
            used_bytes: AtomicUsize::new(0),
        })
    }

    pub fn maximum_bytes(&self) -> usize {
        self.maximum_bytes
    }

    pub fn used_bytes(&self) -> usize {
        self.used_bytes.load(Ordering::Acquire)
    }

    pub fn allocator(self: &Arc<Self>) -> HeapQuotaAllocator {
        HeapQuotaAllocator {
            quota: Some(Arc::clone(self)),
        }
    }

    pub(crate) fn try_reserve(&self, bytes: usize) -> bool {
        let mut current = self.used_bytes.load(Ordering::Acquire);
        loop {
            let Some(next) = current.checked_add(bytes) else {
                return false;
            };
            if next > self.maximum_bytes {
                return false;
            }
            match self.used_bytes.compare_exchange_weak(
                current,
                next,
                Ordering::AcqRel,
                Ordering::Acquire,
            ) {
                Ok(_) => return true,
                Err(observed) => current = observed,
            }
        }
    }

    pub(crate) fn release(&self, bytes: usize) {
        let mut current = self.used_bytes.load(Ordering::Acquire);
        loop {
            let next = current.saturating_sub(bytes);
            match self.used_bytes.compare_exchange_weak(
                current,
                next,
                Ordering::AcqRel,
                Ordering::Acquire,
            ) {
                Ok(_) => return,
                Err(observed) => current = observed,
            }
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum HeapQuotaScopeError {
    ThreadLocalUnavailable,
    DifferentQuotaNested,
    NestingOverflow,
}

/// Makes [`HeapQuotaAllocator::current`] return an allocator bound to `quota`
/// on the current NaOS thread.
pub struct HeapQuotaScope {
    _quota: Arc<HeapQuota>,
    _not_send: PhantomData<*mut ()>,
}

impl HeapQuotaScope {
    pub fn enter(quota: &Arc<HeapQuota>) -> Result<Self, HeapQuotaScopeError> {
        let depth = active_heap_quota_depth();
        let previous = if depth == 0 {
            0
        } else {
            active_heap_quota_bits()
        };
        let quota_pointer = Arc::as_ptr(quota) as usize;
        if previous != 0 && previous != quota_pointer {
            return Err(HeapQuotaScopeError::DifferentQuotaNested);
        }
        let next_depth = depth
            .checked_add(1)
            .ok_or(HeapQuotaScopeError::NestingOverflow)?;
        if previous == 0 && !set_active_heap_quota_bits(quota_pointer) {
            return Err(HeapQuotaScopeError::ThreadLocalUnavailable);
        }
        if !set_active_heap_quota_depth(next_depth) {
            if previous == 0 {
                let _ = set_active_heap_quota_bits(0);
            }
            return Err(HeapQuotaScopeError::ThreadLocalUnavailable);
        }
        Ok(Self {
            _quota: Arc::clone(quota),
            _not_send: PhantomData,
        })
    }
}

impl Drop for HeapQuotaScope {
    fn drop(&mut self) {
        let depth = active_heap_quota_depth();
        if depth == 0 || active_heap_quota_bits() != Arc::as_ptr(&self._quota) as usize {
            return;
        }
        let remaining = depth - 1;
        let _ = set_active_heap_quota_depth(remaining);
        if remaining == 0 {
            let _ = set_active_heap_quota_bits(0);
        }
    }
}

/// An allocator that applies a [`HeapQuota`] to allocations explicitly made
/// through it. The default global allocator does not use quota bookkeeping.
#[derive(Default)]
pub struct HeapQuotaAllocator {
    quota: Option<Arc<HeapQuota>>,
}

impl Clone for HeapQuotaAllocator {
    fn clone(&self) -> Self {
        Self {
            quota: self.quota.as_ref().map(Arc::clone),
        }
    }
}

impl HeapQuotaAllocator {
    /// Capture the quota active on the current thread, if any.
    pub fn current() -> Self {
        if active_heap_quota_depth() == 0 {
            return Self::default();
        }
        let quota = active_heap_quota_bits() as *const HeapQuota;
        if quota.is_null() {
            return Self::default();
        }
        // SAFETY: HeapQuotaScope keeps the active Arc alive until the scope is
        // dropped, and the new Arc reference makes the allocator independent
        // of that scope's lifetime.
        unsafe { Arc::increment_strong_count(quota) };
        Self {
            // SAFETY: paired with the increment above; ownership transfers to
            // this allocator value.
            quota: Some(unsafe { Arc::from_raw(quota) }),
        }
    }

    fn alloc_with_quota(&self, layout: Layout, zeroed: bool) -> *mut u8 {
        match &self.quota {
            Some(quota) => unsafe { allocate_quota(layout, zeroed, quota) },
            None => unsafe { allocate_raw(layout, zeroed) },
        }
    }

    unsafe fn dealloc_with_quota(&self, pointer: *mut u8, layout: Layout) {
        if pointer.is_null() || layout.size() == 0 {
            return;
        }
        if let Some(quota) = &self.quota {
            // SAFETY: quota-backed allocations from this allocator store the
            // matching header immediately before the aligned user pointer.
            let header_pointer = unsafe {
                pointer
                    .sub(core::mem::size_of::<AllocationHeader>())
                    .cast::<AllocationHeader>()
            };
            let header = unsafe { header_pointer.read() };
            quota.release(header.quota_charge);
            unsafe { mi_free(header.base) };
        } else {
            unsafe { mi_free(pointer) };
        }
    }
}

#[repr(C)]
#[derive(Clone, Copy)]
struct AllocationHeader {
    base: *mut u8,
    size: usize,
    quota_charge: usize,
}

pub(crate) fn allocation_charge_bytes(layout: Layout) -> Option<usize> {
    let alignment = layout
        .align()
        .max(core::mem::align_of::<AllocationHeader>());
    core::mem::size_of::<AllocationHeader>()
        .checked_add(alignment - 1)
        .and_then(|padding| layout.size().checked_add(padding))
}

unsafe fn allocate_raw(layout: Layout, zeroed: bool) -> *mut u8 {
    if layout.size() == 0 {
        return layout.dangling_ptr().as_ptr();
    }
    let Some(allocation_size) = layout.size().checked_add(layout.align() - 1) else {
        return ptr::null_mut();
    };
    if allocation_size > MEMORY_MAP_MAX_BYTES as usize {
        return ptr::null_mut();
    }
    unsafe {
        if zeroed {
            mi_zalloc_aligned(layout.size(), layout.align())
        } else {
            mi_malloc_aligned(layout.size(), layout.align())
        }
    }
}

unsafe fn allocate_quota(layout: Layout, zeroed: bool, quota: &HeapQuota) -> *mut u8 {
    if layout.size() == 0 {
        return layout.dangling_ptr().as_ptr();
    }
    let Some(total_size) = allocation_charge_bytes(layout) else {
        return ptr::null_mut();
    };
    if total_size > MEMORY_MAP_MAX_BYTES as usize {
        return ptr::null_mut();
    }
    if !quota.try_reserve(total_size) {
        return ptr::null_mut();
    }

    let pointer = unsafe { allocate_quota_without_charge(layout, zeroed) };
    if pointer.is_null() {
        quota.release(total_size);
        return pointer;
    }
    let header_pointer = unsafe {
        pointer
            .sub(core::mem::size_of::<AllocationHeader>())
            .cast::<AllocationHeader>()
    };
    unsafe { (*header_pointer).quota_charge = total_size };
    pointer
}

#[cfg(target_os = "naos")]
unsafe impl Allocator for HeapQuotaAllocator {
    fn allocate(&self, layout: Layout) -> Result<NonNull<[u8]>, AllocError> {
        let pointer = self.alloc_with_quota(layout, false);
        let pointer = NonNull::new(pointer).ok_or(AllocError)?;
        Ok(NonNull::slice_from_raw_parts(pointer, layout.size()))
    }

    fn allocate_zeroed(&self, layout: Layout) -> Result<NonNull<[u8]>, AllocError> {
        let pointer = self.alloc_with_quota(layout, true);
        let pointer = NonNull::new(pointer).ok_or(AllocError)?;
        Ok(NonNull::slice_from_raw_parts(pointer, layout.size()))
    }

    unsafe fn deallocate(&self, pointer: NonNull<u8>, layout: Layout) {
        unsafe { self.dealloc_with_quota(pointer.as_ptr(), layout) };
    }

    unsafe fn grow(
        &self,
        pointer: NonNull<u8>,
        old_layout: Layout,
        new_layout: Layout,
    ) -> Result<NonNull<[u8]>, AllocError> {
        let replacement =
            unsafe { resize_allocation(self, pointer.as_ptr(), old_layout, new_layout, false) };
        let replacement = NonNull::new(replacement).ok_or(AllocError)?;
        Ok(NonNull::slice_from_raw_parts(
            replacement,
            new_layout.size(),
        ))
    }

    unsafe fn grow_zeroed(
        &self,
        pointer: NonNull<u8>,
        old_layout: Layout,
        new_layout: Layout,
    ) -> Result<NonNull<[u8]>, AllocError> {
        let replacement =
            unsafe { resize_allocation(self, pointer.as_ptr(), old_layout, new_layout, true) };
        let replacement = NonNull::new(replacement).ok_or(AllocError)?;
        Ok(NonNull::slice_from_raw_parts(
            replacement,
            new_layout.size(),
        ))
    }

    unsafe fn shrink(
        &self,
        pointer: NonNull<u8>,
        old_layout: Layout,
        new_layout: Layout,
    ) -> Result<NonNull<[u8]>, AllocError> {
        let replacement =
            unsafe { resize_allocation(self, pointer.as_ptr(), old_layout, new_layout, false) };
        let replacement = NonNull::new(replacement).ok_or(AllocError)?;
        Ok(NonNull::slice_from_raw_parts(
            replacement,
            new_layout.size(),
        ))
    }
}

fn active_heap_quota_bits() -> usize {
    #[cfg(test)]
    {
        return TEST_ACTIVE_HEAP_QUOTA.with(core::cell::Cell::get);
    }
    #[cfg(not(test))]
    {
        ACTIVE_HEAP_QUOTA.get().unwrap_or(0)
    }
}

fn set_active_heap_quota_bits(value: usize) -> bool {
    #[cfg(test)]
    {
        TEST_ACTIVE_HEAP_QUOTA.with(|active| active.set(value));
        true
    }
    #[cfg(not(test))]
    {
        ACTIVE_HEAP_QUOTA.set(value)
    }
}

fn active_heap_quota_depth() -> usize {
    #[cfg(test)]
    {
        return TEST_ACTIVE_HEAP_QUOTA_DEPTH.with(core::cell::Cell::get);
    }
    #[cfg(not(test))]
    {
        ACTIVE_HEAP_QUOTA_DEPTH.get().unwrap_or(0)
    }
}

fn set_active_heap_quota_depth(value: usize) -> bool {
    #[cfg(test)]
    {
        TEST_ACTIVE_HEAP_QUOTA_DEPTH.with(|depth| depth.set(value));
        true
    }
    #[cfg(not(test))]
    {
        ACTIVE_HEAP_QUOTA_DEPTH.set(value)
    }
}

pub struct MimallocAllocator;

impl MimallocAllocator {
    pub const fn new() -> Self {
        Self
    }
}

unsafe impl GlobalAlloc for MimallocAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        unsafe { allocate_raw(layout, false) }
    }

    unsafe fn alloc_zeroed(&self, layout: Layout) -> *mut u8 {
        unsafe { allocate_raw(layout, true) }
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
            unsafe { self.dealloc(pointer, layout) };
            return layout.dangling_ptr().as_ptr();
        }

        let Ok(new_layout) = Layout::from_size_align(new_size, layout.align()) else {
            return ptr::null_mut();
        };
        let replacement = unsafe { allocate_raw(new_layout, false) };
        if replacement.is_null() {
            return ptr::null_mut();
        }
        unsafe {
            ptr::copy_nonoverlapping(pointer, replacement, layout.size().min(new_size));
            mi_free(pointer);
        }
        replacement
    }
}

unsafe impl GlobalAlloc for HeapQuotaAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        self.alloc_with_quota(layout, false)
    }

    unsafe fn alloc_zeroed(&self, layout: Layout) -> *mut u8 {
        self.alloc_with_quota(layout, true)
    }

    unsafe fn dealloc(&self, pointer: *mut u8, layout: Layout) {
        unsafe { self.dealloc_with_quota(pointer, layout) };
    }

    unsafe fn realloc(&self, pointer: *mut u8, layout: Layout, new_size: usize) -> *mut u8 {
        if pointer.is_null() {
            let Ok(new_layout) = Layout::from_size_align(new_size, layout.align()) else {
                return ptr::null_mut();
            };
            return self.alloc_with_quota(new_layout, false);
        }
        if layout.size() == 0 {
            let Ok(new_layout) = Layout::from_size_align(new_size, layout.align()) else {
                return ptr::null_mut();
            };
            return self.alloc_with_quota(new_layout, false);
        }
        if new_size == 0 {
            unsafe { self.dealloc_with_quota(pointer, layout) };
            return layout.dangling_ptr().as_ptr();
        }
        let Ok(new_layout) = Layout::from_size_align(new_size, layout.align()) else {
            return ptr::null_mut();
        };
        unsafe { resize_allocation(self, pointer, layout, new_layout, false) }
    }
}

unsafe fn allocate_quota_without_charge(layout: Layout, zeroed: bool) -> *mut u8 {
    if layout.size() == 0 {
        return layout.dangling_ptr().as_ptr();
    }
    let Some(total_size) = allocation_charge_bytes(layout) else {
        return ptr::null_mut();
    };
    if total_size > MEMORY_MAP_MAX_BYTES as usize {
        return ptr::null_mut();
    }
    let alignment = layout
        .align()
        .max(core::mem::align_of::<AllocationHeader>());
    let base = unsafe {
        if zeroed {
            mi_zalloc_aligned(total_size, alignment)
        } else {
            mi_malloc_aligned(total_size, alignment)
        }
    };
    if base.is_null() {
        return ptr::null_mut();
    }
    let Some(user_address) = (base as usize)
        .checked_add(core::mem::size_of::<AllocationHeader>())
        .and_then(|address| address.checked_add(layout.align() - 1))
    else {
        unsafe { mi_free(base) };
        return ptr::null_mut();
    };
    let user_pointer = (user_address & !(layout.align() - 1)) as *mut u8;
    let header_pointer = unsafe {
        user_pointer
            .sub(core::mem::size_of::<AllocationHeader>())
            .cast::<AllocationHeader>()
    };
    unsafe {
        header_pointer.write(AllocationHeader {
            base,
            size: layout.size(),
            quota_charge: 0,
        });
    }
    user_pointer
}

unsafe fn resize_allocation(
    allocator: &HeapQuotaAllocator,
    pointer: *mut u8,
    old_layout: Layout,
    new_layout: Layout,
    zero_extension: bool,
) -> *mut u8 {
    if new_layout.size() == 0 {
        unsafe { allocator.dealloc_with_quota(pointer, old_layout) };
        return new_layout.dangling_ptr().as_ptr();
    }
    if old_layout.size() == 0 {
        return allocator.alloc_with_quota(new_layout, zero_extension);
    }
    let Some(quota) = &allocator.quota else {
        let replacement = unsafe { allocate_raw(new_layout, false) };
        if replacement.is_null() {
            return ptr::null_mut();
        }
        unsafe {
            let copied_bytes = old_layout.size().min(new_layout.size());
            ptr::copy_nonoverlapping(pointer, replacement, copied_bytes);
            if zero_extension && new_layout.size() > copied_bytes {
                ptr::write_bytes(
                    replacement.add(copied_bytes),
                    0,
                    new_layout.size() - copied_bytes,
                );
            }
            mi_free(pointer);
        }
        return replacement;
    };

    let Some(new_charge) = allocation_charge_bytes(new_layout) else {
        return ptr::null_mut();
    };
    if new_charge > MEMORY_MAP_MAX_BYTES as usize {
        return ptr::null_mut();
    }
    let old_header_pointer = unsafe {
        pointer
            .sub(core::mem::size_of::<AllocationHeader>())
            .cast::<AllocationHeader>()
    };
    let old_header = unsafe { old_header_pointer.read() };
    let growth = new_charge.saturating_sub(old_header.quota_charge);
    if growth != 0 && !quota.try_reserve(growth) {
        return ptr::null_mut();
    }
    let replacement = unsafe { allocate_quota_without_charge(new_layout, false) };
    if replacement.is_null() {
        quota.release(growth);
        return ptr::null_mut();
    }
    let new_header_pointer = unsafe {
        replacement
            .sub(core::mem::size_of::<AllocationHeader>())
            .cast::<AllocationHeader>()
    };
    unsafe {
        (*new_header_pointer).quota_charge = new_charge;
        let copied_bytes = old_header
            .size
            .min(old_layout.size())
            .min(new_layout.size());
        ptr::copy_nonoverlapping(pointer, replacement, copied_bytes);
        if zero_extension && new_layout.size() > copied_bytes {
            ptr::write_bytes(
                replacement.add(copied_bytes),
                0,
                new_layout.size() - copied_bytes,
            );
        }
        if new_charge < old_header.quota_charge {
            quota.release(old_header.quota_charge - new_charge);
        }
        mi_free(old_header.base);
    }
    replacement
}

pub(crate) unsafe fn thread_done() {
    unsafe { mi_thread_done() };
}
