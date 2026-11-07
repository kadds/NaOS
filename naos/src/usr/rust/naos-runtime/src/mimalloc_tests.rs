use core::alloc::{GlobalAlloc, Layout};
use core::cell::Cell;
use core::ptr;
use std::alloc::System;
use std::sync::Arc;

use crate::mimalloc::{
    HeapQuota, HeapQuotaAllocator, HeapQuotaScope, MimallocAllocator, allocation_charge_bytes,
};
use naos_sys::MEMORY_MAP_MAX_BYTES;

std::thread_local! {
    static MOCK_ALLOCATION_CALLS: Cell<usize> = const { Cell::new(0) };
}
const MOCK_MAX_ALLOCATION_BYTES: usize = 1 << 20;

#[repr(C)]
struct MockAllocationHeader {
    base: *mut u8,
    total_size: usize,
    alignment: usize,
}

unsafe fn mock_allocate(size: usize, requested_alignment: usize, zeroed: bool) -> *mut u8 {
    if size > MOCK_MAX_ALLOCATION_BYTES {
        return ptr::null_mut();
    }
    let alignment = requested_alignment.max(core::mem::align_of::<MockAllocationHeader>());
    let Some(total_size) = core::mem::size_of::<MockAllocationHeader>()
        .checked_add(requested_alignment - 1)
        .and_then(|padding| size.checked_add(padding))
    else {
        return ptr::null_mut();
    };
    let Ok(layout) = Layout::from_size_align(total_size, alignment) else {
        return ptr::null_mut();
    };
    let base = unsafe {
        if zeroed {
            System.alloc_zeroed(layout)
        } else {
            System.alloc(layout)
        }
    };
    if base.is_null() {
        return ptr::null_mut();
    }
    let first_user_byte = unsafe { base.add(core::mem::size_of::<MockAllocationHeader>()) };
    let user_address =
        (first_user_byte as usize + requested_alignment - 1) & !(requested_alignment - 1);
    let user_pointer = user_address as *mut u8;
    let header_pointer = unsafe {
        user_pointer
            .sub(core::mem::size_of::<MockAllocationHeader>())
            .cast::<MockAllocationHeader>()
    };
    unsafe {
        header_pointer.write(MockAllocationHeader {
            base,
            total_size,
            alignment,
        });
    }
    user_pointer
}

#[unsafe(no_mangle)]
unsafe extern "C" fn mi_malloc_aligned(size: usize, alignment: usize) -> *mut u8 {
    MOCK_ALLOCATION_CALLS.with(|calls| calls.set(calls.get() + 1));
    unsafe { mock_allocate(size, alignment, false) }
}

#[unsafe(no_mangle)]
unsafe extern "C" fn mi_zalloc_aligned(size: usize, alignment: usize) -> *mut u8 {
    MOCK_ALLOCATION_CALLS.with(|calls| calls.set(calls.get() + 1));
    unsafe { mock_allocate(size, alignment, true) }
}

#[unsafe(no_mangle)]
unsafe extern "C" fn mi_free(pointer: *mut u8) {
    if pointer.is_null() {
        return;
    }
    let header_pointer = unsafe {
        pointer
            .sub(core::mem::size_of::<MockAllocationHeader>())
            .cast::<MockAllocationHeader>()
    };
    let header = unsafe { header_pointer.read() };
    let layout = Layout::from_size_align(header.total_size, header.alignment).unwrap();
    unsafe { System.dealloc(header.base, layout) };
}

#[unsafe(no_mangle)]
unsafe extern "C" fn mi_thread_done() {}

fn assert_global_allocator<T: GlobalAlloc>() {}

#[test]
fn mimalloc_adapter_implements_global_allocator() {
    assert_global_allocator::<MimallocAllocator>();
    assert_global_allocator::<HeapQuotaAllocator>();
}

#[test]
fn heap_quota_rejects_growth_and_releases_capacity() {
    let quota = HeapQuota::new(64);
    assert!(quota.try_reserve(48));
    assert!(!quota.try_reserve(17));
    assert_eq!(quota.used_bytes(), 48);

    quota.release(16);
    assert!(quota.try_reserve(32));
    assert_eq!(quota.used_bytes(), 64);
    quota.release(64);
    assert_eq!(quota.used_bytes(), 0);
}

#[test]
fn explicit_quota_allocator_tracks_realloc_and_cross_thread_deallocation() {
    let quota = HeapQuota::new(100);
    let initial_layout = Layout::from_size_align(16, 8).unwrap();
    let initial_charge = allocation_charge_bytes(initial_layout).unwrap();
    let (allocator, pointer) = {
        let _scope = HeapQuotaScope::enter(&quota).unwrap();
        let allocator = HeapQuotaAllocator::current();
        let pointer = unsafe { allocator.alloc(initial_layout) };
        assert!(!pointer.is_null());
        for index in 0..16 {
            unsafe { pointer.add(index).write(index as u8) };
        }
        assert_eq!(quota.used_bytes(), initial_charge);
        (allocator, pointer)
    };

    let grown_layout = Layout::from_size_align(48, 8).unwrap();
    let grown_charge = allocation_charge_bytes(grown_layout).unwrap();
    let grown = unsafe { allocator.realloc(pointer, initial_layout, grown_layout.size()) };
    assert!(!grown.is_null());
    assert_eq!(quota.used_bytes(), grown_charge);
    for index in 0..16 {
        assert_eq!(unsafe { grown.add(index).read() }, index as u8);
    }

    let over_limit = unsafe { allocator.realloc(grown, grown_layout, 80) };
    assert!(over_limit.is_null());
    assert_eq!(quota.used_bytes(), grown_charge);

    let quota_for_drop = Arc::clone(&quota);
    let grown_address = grown as usize;
    std::thread::spawn(move || {
        let pointer = grown_address as *mut u8;
        unsafe { allocator.dealloc(pointer, grown_layout) };
    })
    .join()
    .unwrap();
    assert_eq!(quota_for_drop.used_bytes(), 0);
}

#[test]
fn unmetered_global_allocations_stay_outside_explicit_quota() {
    let allocator = MimallocAllocator::new();
    let initial_layout = Layout::from_size_align(16, 8).unwrap();
    let quota = HeapQuota::new(64);
    let pointer = {
        let _scope = HeapQuotaScope::enter(&quota).unwrap();
        let pointer = unsafe { allocator.alloc(initial_layout) };
        assert!(!pointer.is_null());
        assert_eq!(quota.used_bytes(), 0);
        pointer
    };
    unsafe { allocator.dealloc(pointer, initial_layout) };
}

#[test]
fn explicit_quota_allocator_rejects_growth_and_releases_charge() {
    let initial_layout = Layout::from_size_align(16, 8).unwrap();
    let grown_layout = Layout::from_size_align(48, 8).unwrap();
    let grown_charge = allocation_charge_bytes(grown_layout).unwrap();
    let quota = HeapQuota::new(grown_charge - 1);
    let allocator = quota.allocator();
    let pointer = unsafe { allocator.alloc(initial_layout) };
    assert!(!pointer.is_null());
    let initial_charge = allocation_charge_bytes(initial_layout).unwrap();
    assert_eq!(quota.used_bytes(), initial_charge);

    assert!(unsafe { allocator.realloc(pointer, initial_layout, grown_layout.size()) }.is_null());
    assert_eq!(quota.used_bytes(), initial_charge);
    unsafe { allocator.dealloc(pointer, initial_layout) };
    assert_eq!(quota.used_bytes(), 0);

    let quota = HeapQuota::new(grown_charge);
    let allocator = quota.allocator();
    let pointer = unsafe { allocator.alloc(initial_layout) };
    assert!(!pointer.is_null());
    for index in 0..initial_layout.size() {
        unsafe { pointer.add(index).write(index as u8) };
    }
    let grown = unsafe { allocator.realloc(pointer, initial_layout, grown_layout.size()) };
    assert!(!grown.is_null());
    assert_eq!(quota.used_bytes(), grown_charge);
    for index in 0..initial_layout.size() {
        assert_eq!(unsafe { grown.add(index).read() }, index as u8);
    }
    unsafe { allocator.dealloc(grown, grown_layout) };
    assert_eq!(quota.used_bytes(), 0);
}

#[test]
fn quota_allocator_includes_header_and_alignment_overhead() {
    let layout = Layout::from_size_align(16, 64).unwrap();
    let quota = HeapQuota::new(layout.size());
    let allocator = quota.allocator();
    let allocations_before = MOCK_ALLOCATION_CALLS.with(Cell::get);

    let pointer = unsafe { allocator.alloc(layout) };

    assert!(pointer.is_null());
    assert_eq!(quota.used_bytes(), 0);
    assert_eq!(MOCK_ALLOCATION_CALLS.with(Cell::get), allocations_before);
}

#[test]
fn allocator_rejects_unmappable_layout_before_calling_mimalloc() {
    let allocator = MimallocAllocator::new();
    let layout = Layout::from_size_align(1, MEMORY_MAP_MAX_BYTES as usize).unwrap();
    let allocations_before = MOCK_ALLOCATION_CALLS.with(Cell::get);

    let pointer = unsafe { allocator.alloc(layout) };

    assert!(pointer.is_null());
    assert_eq!(MOCK_ALLOCATION_CALLS.with(Cell::get), allocations_before);
}
