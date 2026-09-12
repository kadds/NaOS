use alloc::vec::Vec;
use core::alloc::Layout;
use core::sync::atomic::{AtomicI32, AtomicU64, Ordering};

use naos_idl::echo;
use naos_idl::stream;
use naos_sys as sys;
use servicekit::Channel;
use servicekit::naos_runtime::{ThreadLocal, spawn};

static THREAD_VALUE: ThreadLocal<u64> = ThreadLocal::new(0);
static STDOUT_STREAM: AtomicU64 = AtomicU64::new(sys::HANDLE_INVALID);

unsafe extern "C" {
    fn memcpy(destination: *mut u8, source: *const u8, length: usize) -> *mut u8;
    fn memcmp(left: *const u8, right: *const u8, length: usize) -> i32;
    fn memmove(destination: *mut u8, source: *const u8, length: usize) -> *mut u8;
    fn memset(destination: *mut u8, value: i32, length: usize) -> *mut u8;
}

#[thread_local]
static mut COMPILER_TLS_VALUE: u64 = 0x07;

pub fn log(message: &'static [u8]) {
    // `_s_log` prefixes the process name; keep the marker namespace on the
    // stdout copy only so serial output does not repeat `rust-smoke-suite:`.
    let native_message = message
        .strip_prefix(b"rust-smoke-suite: ")
        .unwrap_or(message);
    unsafe { sys::_s_log(native_message.as_ptr()) };
    let stream_handle = STDOUT_STREAM.load(Ordering::Relaxed);
    if stream_handle == sys::HANDLE_INVALID {
        return;
    }

    let mut duplicate = sys::HANDLE_INVALID;
    if unsafe { sys::_na_handle_duplicate(stream_handle, 0, &mut duplicate) } != sys::STATUS_OK {
        return;
    }
    let endpoint = unsafe { naos_idl::ProtocolClientEndpoint::from_raw(duplicate) };
    let payload = &message[..message.len().saturating_sub(1)];
    // The smoke writes a diagnostic line, so it owns a region sized exactly to
    // that line: every migrated Stream.write carries its payload in the
    // caller's MemoryObject, never inline in the control message.
    let window = payload.len().max(1);
    let Ok(region) = servicekit::memory::MemoryObject::new(window) else {
        return;
    };
    if region.map_persistent(window, true).is_err() {
        return;
    }
    if !payload.is_empty() && region.write_region(0, payload).is_err() {
        return;
    }
    // The bootstrap stdout stream is NaOS-only; a plain host has no
    // capability table to transfer the region through.
    let Some(owner) = region.native_handle() else {
        return;
    };
    let mut resources = naos_idl::ResourceTable::new();
    let Ok(buffer) = resources.push_duplicate(owner) else {
        return;
    };
    let value = stream::write_request {
        size: payload.len() as u64,
        flags: 0,
        buffer,
    };
    // The request envelope is the generated fixed header; the payload travels
    // in the region.
    let mut request_wire = [0_u8; stream::WRITE_REQUEST_HEADER_BYTES];
    let Ok(mut invocation) =
        stream::submit_write(&endpoint, &value, resources, &mut request_wire, 0)
    else {
        return;
    };
    if !servicekit::wait_for_completion(invocation.get(), u64::MAX) {
        return;
    }
    let mut response_wire = [0_u8; 32];
    let _ = stream::take_write(&mut invocation, &mut response_wire);
}

fn run_native_smoke(context: servicekit::Context) -> i64 {
    STDOUT_STREAM.store(context.stdout_stream(), Ordering::Relaxed);

    if Channel::create(None).is_err() {
        log(b"rust-smoke-suite: channel create failed\0");
        return 3;
    }
    log(b"rust-smoke-suite: native bootstrap ready\0");
    0
}

fn run_allocator_smoke() -> i64 {
    let layout = Layout::from_size_align(97, 64).unwrap();
    let pointer = unsafe { alloc::alloc::alloc(layout) };
    if pointer.is_null() || !(pointer as usize).is_multiple_of(64) {
        log(b"rust-smoke-suite: aligned allocation failed\0");
        return 10;
    }
    for index in 0..layout.size() {
        unsafe { pointer.add(index).write((index as u8).wrapping_mul(3)) };
    }

    let grown_layout = Layout::from_size_align(4096, 64).unwrap();
    let grown = unsafe { alloc::alloc::realloc(pointer, layout, grown_layout.size()) };
    if grown.is_null() || !(grown as usize).is_multiple_of(64) {
        if !grown.is_null() {
            unsafe { alloc::alloc::dealloc(grown, grown_layout) };
        }
        log(b"rust-smoke-suite: realloc failed\0");
        return 11;
    }
    for index in 0..layout.size() {
        if unsafe { grown.add(index).read() } != (index as u8).wrapping_mul(3) {
            unsafe { alloc::alloc::dealloc(grown, grown_layout) };
            log(b"rust-smoke-suite: realloc data mismatch\0");
            return 12;
        }
    }
    unsafe { alloc::alloc::dealloc(grown, grown_layout) };

    let zero_layout = Layout::from_size_align(129, 32).unwrap();
    let zeroed = unsafe { alloc::alloc::alloc_zeroed(zero_layout) };
    if zeroed.is_null() || !(zeroed as usize).is_multiple_of(32) {
        if !zeroed.is_null() {
            unsafe { alloc::alloc::dealloc(zeroed, zero_layout) };
        }
        log(b"rust-smoke-suite: zeroed allocation failed\0");
        return 13;
    }
    for index in 0..zero_layout.size() {
        if unsafe { zeroed.add(index).read() } != 0 {
            unsafe { alloc::alloc::dealloc(zeroed, zero_layout) };
            log(b"rust-smoke-suite: zeroed data mismatch\0");
            return 14;
        }
    }
    unsafe { alloc::alloc::dealloc(zeroed, zero_layout) };

    let impossible = Layout::from_size_align(1, sys::MEMORY_MAP_MAX_BYTES as usize).unwrap();
    if !unsafe { servicekit::naos_runtime::try_native_alloc(impossible) }.is_null() {
        log(b"rust-smoke-suite: OOM guard failed\0");
        return 15;
    }
    if Layout::from_size_align(usize::MAX, 8).is_ok() {
        log(b"rust-smoke-suite: overflow guard failed\0");
        return 16;
    }
    log(b"rust-smoke-suite: native allocator ready\0");
    0
}

fn run_memory_smoke() -> i64 {
    let left = [1_u8, 2, 3];
    let right = [1_u8, 2, 3];
    if unsafe { memcmp(left.as_ptr(), right.as_ptr(), 0) } != 0
        || unsafe { memcmp(left.as_ptr(), right.as_ptr(), left.len()) } != 0
    {
        log(b"rust-smoke-suite: memcmp failed\0");
        return 17;
    }

    let mut copied = [0_u8; 3];
    if unsafe { memcpy(copied.as_mut_ptr(), left.as_ptr(), left.len()) } != copied.as_mut_ptr()
        || copied != left
    {
        log(b"rust-smoke-suite: memcpy failed\0");
        return 18;
    }

    let mut filled = [0_u8; 3];
    if unsafe { memset(filled.as_mut_ptr(), 0xa5, filled.len()) } != filled.as_mut_ptr()
        || filled != [0xa5, 0xa5, 0xa5]
    {
        log(b"rust-smoke-suite: memset failed\0");
        return 19;
    }

    let mut overlapping = [1_u8, 2, 3, 4, 5, 6];
    unsafe {
        memmove(
            overlapping.as_mut_ptr().add(1),
            overlapping.as_ptr(),
            overlapping.len() - 1,
        );
    }
    if overlapping != [1, 1, 2, 3, 4, 5] {
        log(b"rust-smoke-suite: memmove overlap failed\0");
        return 20;
    }
    0
}

extern "C" fn child_thread(_argument: *mut u8) -> i64 {
    if unsafe { core::ptr::read_volatile(&raw const COMPILER_TLS_VALUE) } != 0x07 {
        return -2;
    }
    unsafe { core::ptr::write_volatile(&raw mut COMPILER_TLS_VALUE, 0x33) };
    if unsafe { core::ptr::read_volatile(&raw const COMPILER_TLS_VALUE) } != 0x33
        || THREAD_VALUE.get().is_some()
        || !THREAD_VALUE.set(0x22)
    {
        return -1;
    }
    0x22
}

extern "C" fn dropped_child(argument: *mut u8) -> i64 {
    let completed = unsafe { &*(argument as *const AtomicI32) };
    completed.store(1, Ordering::Release);
    0x44
}

pub fn run(context: servicekit::Context) -> i64 {
    if run_native_smoke(context) != 0 || !THREAD_VALUE.set(0x11) {
        log(b"rust-smoke-suite: runtime or TLS setup failed\0");
        return 2;
    }

    if run_allocator_smoke() != 0 {
        return 3;
    }
    if run_memory_smoke() != 0 {
        return 4;
    }

    if unsafe { core::ptr::read_volatile(&raw const COMPILER_TLS_VALUE) } != 0x07 {
        log(b"rust-smoke-suite: compiler TLS setup failed\0");
        return 8;
    }
    unsafe { core::ptr::write_volatile(&raw mut COMPILER_TLS_VALUE, 0x11) };

    let invalid_tcb = usize::MAX as *mut u8;
    if unsafe { sys::_s_tcb_set(invalid_tcb) } == 0
        || unsafe { sys::_s_clone(child_thread as *mut u8, core::ptr::null_mut(), invalid_tcb) }
            == 0
    {
        log(b"rust-smoke-suite: invalid TLS/thread syscall accepted\0");
        return 9;
    }

    let dropped_completed = AtomicI32::new(0);
    let dropped = match spawn(
        dropped_child,
        &dropped_completed as *const AtomicI32 as *mut u8,
    ) {
        Ok(handle) => handle,
        Err(_) => {
            log(b"rust-smoke-suite: drop-race spawn failed\0");
            return 10;
        }
    };
    drop(dropped);
    while dropped_completed.load(Ordering::Acquire) == 0 {
        core::hint::spin_loop();
    }

    let mut payload = Vec::with_capacity(5);
    payload.extend_from_slice(b"hello");
    let request = echo::echo_request { payload: &payload };
    let mut wire = [0_u8; 64];
    let written = match echo::encode_echo_request(&request, &mut wire) {
        Ok(size) => size,
        Err(_) => {
            log(b"rust-smoke-suite: IDL encode failed\0");
            return 3;
        }
    };
    let decoded = match echo::decode_echo_request(&wire[..written]) {
        Ok(value) if value.payload == b"hello" => value,
        _ => {
            log(b"rust-smoke-suite: IDL decode failed\0");
            return 4;
        }
    };
    if decoded.payload != payload.as_slice() {
        log(b"rust-smoke-suite: IDL payload mismatch\0");
        return 5;
    }

    for _ in 0..8 {
        let child = match spawn(child_thread, core::ptr::null_mut()) {
            Ok(handle) => handle,
            Err(_) => {
                log(b"rust-smoke-suite: thread spawn failed\0");
                return 6;
            }
        };
        if child.join() != Ok(0x22)
            || THREAD_VALUE.get() != Some(0x11)
            || unsafe { core::ptr::read_volatile(&raw const COMPILER_TLS_VALUE) } != 0x11
        {
            log(b"rust-smoke-suite: TLS isolation or join failed\0");
            return 7;
        }
    }

    log(b"rust-smoke-suite: alloc idl tls thread ready\0");
    0
}
