//! Indirection seam over the NaOS channel syscalls used by naos-idl and the
//! generated bindings.
//!
//! Every syscall entry point the crate touches is routed through a table of
//! function pointers that defaults to the real `naos-sys` symbols.  The
//! indirection costs one atomic load per syscall (negligible next to the
//! syscall itself) and lets `naos_idl::loopback` install an in-process fake
//! kernel so server/client round trips can be tested on any host without a
//! running NaOS kernel.

use core::ptr;
use core::sync::atomic::{AtomicPtr, Ordering};

use naos_sys as sys;

// Fields for syscalls the pure-client surface never calls (raw channel
// send/discard, descriptor creation, oneway submit) are still routed through
// the table so `loopback` can fake them; dead-code silence is intentional.
#[allow(dead_code)]
pub(crate) struct KernelOps {
    pub handle_close: unsafe extern "C" fn(sys::Handle) -> sys::Status,
    pub handle_duplicate: unsafe extern "C" fn(sys::Handle, u64, *mut sys::Handle) -> sys::Status,
    pub handle_restrict: unsafe extern "C" fn(
        sys::Handle,
        *const sys::HandleRestriction,
        *mut sys::Handle,
    ) -> sys::Status,
    pub handle_get_info: unsafe extern "C" fn(sys::Handle, *mut sys::HandleInfo) -> sys::Status,
    pub channel_create: unsafe extern "C" fn(
        *const sys::ChannelOptions,
        *mut sys::Handle,
        *mut sys::Handle,
    ) -> sys::Status,
    pub channel_send:
        unsafe extern "C" fn(sys::Handle, *const sys::ChannelSendFrame) -> sys::Status,
    pub channel_receive:
        unsafe extern "C" fn(sys::Handle, *mut sys::ChannelReceiveFrame) -> sys::Status,
    pub channel_discard: unsafe extern "C" fn(sys::Handle) -> sys::Status,
    pub protocol_descriptor_create:
        unsafe extern "C" fn(*const sys::ProtocolDescriptor, *mut sys::Handle) -> sys::Status,
    pub protocol_endpoint_create: unsafe extern "C" fn(
        sys::Handle,
        *const sys::ProtocolEndpointOptions,
        *mut sys::Handle,
        *mut sys::Handle,
    ) -> sys::Status,
    pub invoke_submit:
        unsafe extern "C" fn(sys::Handle, *const sys::SubmitFrame, *mut sys::Handle) -> sys::Status,
    pub invoke_oneway: unsafe extern "C" fn(sys::Handle, *const sys::SubmitFrame) -> sys::Status,
    pub invocation_cancel: unsafe extern "C" fn(sys::Handle) -> sys::Status,
    pub invocation_take_result:
        unsafe extern "C" fn(sys::Handle, *mut sys::ResultFrame) -> sys::Status,
    pub responder_reply: unsafe extern "C" fn(sys::Handle, *const sys::ReplyFrame) -> sys::Status,
    pub responder_fail: unsafe extern "C" fn(sys::Handle, *const sys::FailFrame) -> sys::Status,
}

pub(crate) static SYSTEM_OPS: KernelOps = KernelOps {
    handle_close: sys::_na_handle_close,
    handle_duplicate: sys::_na_handle_duplicate,
    handle_restrict: sys::_na_handle_restrict,
    handle_get_info: sys::_na_handle_get_info,
    channel_create: sys::_na_channel_create,
    channel_send: sys::_na_channel_send,
    channel_receive: sys::_na_channel_receive,
    channel_discard: sys::_na_channel_discard,
    protocol_descriptor_create: sys::_na_protocol_descriptor_create,
    protocol_endpoint_create: sys::_na_protocol_endpoint_create,
    invoke_submit: sys::_na_invoke_submit,
    invoke_oneway: sys::_na_invoke_oneway,
    invocation_cancel: sys::_na_invocation_cancel,
    invocation_take_result: sys::_na_invocation_take_result,
    responder_reply: sys::_na_responder_reply,
    responder_fail: sys::_na_responder_fail,
};

static ACTIVE_OPS: AtomicPtr<KernelOps> = AtomicPtr::new(ptr::null_mut());

#[inline]
pub(crate) fn ops() -> &'static KernelOps {
    let active = ACTIVE_OPS.load(Ordering::Acquire);
    if active.is_null() {
        &SYSTEM_OPS
    } else {
        // SAFETY: installed by `crate::kernel_ops::install`, which leaks a
        // boxed table that is never freed or mutated for the lifetime of the
        // process.
        unsafe { &*active }
    }
}

/// Swap in an override ops table; returns the previous pointer so callers can
/// restore it.
pub(crate) fn install(ops: *mut KernelOps) -> *mut KernelOps {
    ACTIVE_OPS.swap(ops, Ordering::AcqRel)
}

/// True when a host test installed the in-process fake kernel, so a
/// capability carries fake metadata rather than a real kernel object.
pub(crate) fn overridden() -> bool {
    !ACTIVE_OPS.load(Ordering::Acquire).is_null()
}
