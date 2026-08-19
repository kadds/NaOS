#![feature(thread_local)]
#![no_std]

#[cfg(not(test))]
#[panic_handler]
fn panic(_: &core::panic::PanicInfo<'_>) -> ! {
    loop {}
}

#[thread_local]
static mut RUST_TLS_VALUE: u64 = 0x11;

/// Read and replace a compiler TLS value. The C++ caller supplies the mlibc
/// TCB, while this Rust static contributes the same executable's PT_TLS.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn naos_rust_tls_probe(value: u64) -> u64 {
    unsafe {
        let previous = RUST_TLS_VALUE;
        RUST_TLS_VALUE = value;
        previous
    }
}
