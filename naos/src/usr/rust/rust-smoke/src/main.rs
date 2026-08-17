#![no_std]
#![no_main]

use core::panic::PanicInfo;

use naos_runtime::{Bootstrap, InitialStack};
use naos_sys as sys;

fn log(message: &'static [u8]) {
    unsafe { sys::_s_log(message.as_ptr()) };
}

#[panic_handler]
fn panic(_info: &PanicInfo<'_>) -> ! {
    log(b"rust-smoke: panic\0");
    unsafe { sys::_s_exit(101) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn naos_app_main(
    stack: *const InitialStack,
    bootstrap: *const Bootstrap,
) -> i64 {
    if stack.is_null() || bootstrap.is_null() {
        log(b"rust-smoke: runtime contract invalid\0");
        return 2;
    }

    let mut left = sys::HANDLE_INVALID;
    let mut right = sys::HANDLE_INVALID;
    let create_status =
        unsafe { sys::_na_channel_create(core::ptr::null(), &mut left, &mut right) };
    if create_status != sys::STATUS_OK
        || left == sys::HANDLE_INVALID
        || right == sys::HANDLE_INVALID
    {
        log(b"rust-smoke: channel create failed\0");
        return 3;
    }

    let left_status = unsafe { sys::_na_handle_close(left) };
    let right_status = unsafe { sys::_na_handle_close(right) };
    if left_status != sys::STATUS_OK || right_status != sys::STATUS_OK {
        log(b"rust-smoke: channel close failed\0");
        return 4;
    }

    log(b"rust-smoke: native bootstrap ready\0");
    0
}
