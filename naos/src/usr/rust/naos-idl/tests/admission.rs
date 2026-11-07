#![cfg(feature = "alloc")]

use naos_idl::block_device::{self, BlockDeviceServiceKitAdmission};
use naos_idl::transport::{RpcOutcome, RpcRequest};
use std::future::Future;
use std::sync::Arc;
use std::task::{Context, Poll, Wake, Waker};

struct NoopWake;

impl Wake for NoopWake {
    fn wake(self: Arc<Self>) {}
}

fn block_on_ready<T>(future: impl Future<Output = T>) -> T {
    let waker = Waker::from(Arc::new(NoopWake));
    let mut context = Context::from_waker(&waker);
    let mut future = Box::pin(future);
    loop {
        match future.as_mut().poll(&mut context) {
            Poll::Ready(value) => return value,
            Poll::Pending => std::thread::yield_now(),
        }
    }
}

#[derive(Default)]
struct HookCalls(Vec<&'static str>);

struct RecordingAdmission;

impl BlockDeviceServiceKitAdmission<HookCalls> for RecordingAdmission {
    type Admission = ();

    async fn get_info(
        &mut self,
        context: &mut HookCalls,
        _request: &block_device::get_info_request,
    ) -> Result<Option<Self::Admission>, RpcOutcome> {
        context.0.push("get_info");
        Ok(Some(()))
    }
}

struct RejectingAdmission;

impl BlockDeviceServiceKitAdmission<()> for RejectingAdmission {
    type Admission = ();

    async fn get_info(
        &mut self,
        _context: &mut (),
        _request: &block_device::get_info_request,
    ) -> Result<Option<Self::Admission>, RpcOutcome> {
        Err(RpcOutcome {
            execution: 0,
            reason: 0,
            protocol_error: -13,
        })
    }
}

fn request(method_id: u64, payload: &[u8]) -> RpcRequest<'_> {
    RpcRequest {
        protocol_uuid: block_device::PROTOCOL_UUID,
        revision: block_device::PROTOCOL_REVISION,
        method_id,
        request_id: 1,
        target: None,
        payload,
        resources: &[],
        bulk: &[],
    }
}

#[test]
fn generated_admission_routes_to_the_typed_method_hook() {
    let request = request(block_device::METHOD_GET_INFO, &[]);
    let mut calls = HookCalls::default();
    let mut hook = RecordingAdmission;

    let outcome = block_on_ready(block_device::admit_servicekit(
        &request, &mut calls, &mut hook,
    ));

    assert_eq!(outcome, Ok(Some(())));
    assert_eq!(calls.0, ["get_info"]);
}

#[test]
fn decoded_request_can_be_admitted_without_decoding_the_wire_again() {
    let request = request(block_device::METHOD_GET_INFO, &[]);
    let decoded = block_device::decode_servicekit_request(&request).unwrap();
    let mut calls = HookCalls::default();
    let mut hook = RecordingAdmission;

    let outcome = block_on_ready(block_device::admit_servicekit_request(
        &decoded, &mut calls, &mut hook,
    ));

    assert_eq!(outcome, Ok(Some(())));
    assert_eq!(calls.0, ["get_info"]);
}

#[test]
fn malformed_request_is_rejected_before_the_admission_hook() {
    let request = request(block_device::METHOD_READ, &[]);
    let mut calls = HookCalls::default();
    let mut hook = RecordingAdmission;

    let outcome = block_on_ready(block_device::admit_servicekit(
        &request, &mut calls, &mut hook,
    ));

    assert_eq!(outcome.unwrap_err().protocol_error, -22);
    assert!(calls.0.is_empty());
}

#[test]
fn unknown_method_is_rejected_without_calling_a_hook() {
    let request = request(99, &[]);
    let mut calls = HookCalls::default();
    let mut hook = RecordingAdmission;

    let outcome = block_on_ready(block_device::admit_servicekit(
        &request, &mut calls, &mut hook,
    ));

    assert_eq!(outcome.unwrap_err().protocol_error, -38);
    assert!(calls.0.is_empty());
}

#[test]
fn stale_revision_is_rejected_without_calling_a_hook() {
    let mut request = request(block_device::METHOD_GET_INFO, &[]);
    request.revision = block_device::PROTOCOL_REVISION + 1;
    let mut calls = HookCalls::default();
    let mut hook = RecordingAdmission;

    let outcome = block_on_ready(block_device::admit_servicekit(
        &request, &mut calls, &mut hook,
    ));

    assert_eq!(outcome.unwrap_err().protocol_error, -71);
    assert!(calls.0.is_empty());
}

#[test]
fn unimplemented_hook_defaults_to_no_admission_plan() {
    let request = request(block_device::METHOD_FLUSH, &[]);
    let mut calls = HookCalls::default();
    let mut hook = RecordingAdmission;

    let outcome = block_on_ready(block_device::admit_servicekit(
        &request, &mut calls, &mut hook,
    ));

    assert_eq!(outcome, Ok(None));
    assert!(calls.0.is_empty());
}

#[test]
fn hook_rejection_is_propagated() {
    let request = request(block_device::METHOD_GET_INFO, &[]);
    let mut context = ();
    let mut hook = RejectingAdmission;

    let outcome = block_on_ready(block_device::admit_servicekit(
        &request,
        &mut context,
        &mut hook,
    ));

    assert_eq!(outcome.unwrap_err().protocol_error, -13);
}
