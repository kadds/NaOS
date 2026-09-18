use core::alloc::GlobalAlloc;

use crate::mimalloc::MimallocAllocator;

fn assert_global_allocator<T: GlobalAlloc>() {}

#[test]
fn mimalloc_adapter_implements_global_allocator() {
    assert_global_allocator::<MimallocAllocator>();
}
