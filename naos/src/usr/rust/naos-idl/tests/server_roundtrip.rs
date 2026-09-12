//! Host-side round-trip tests for the generated server surface, driven over
//! the `loopback` fake kernel (`submit -> receive -> dispatch -> reply ->
//! take_result`).

#![cfg(feature = "test-loopback")]

use naos_idl::loopback::{self, install};
use naos_idl::{
    CallError, DispatchOutcome, FailInvocation, MethodReply, ResourceTable, block_device, echo,
};
use naos_sys as sys;

// ---------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------

struct EchoServer {
    scratch: Vec<u8>,
}

impl EchoServer {
    fn new() -> Self {
        Self {
            scratch: Vec::new(),
        }
    }
}

impl echo::EchoHandler for EchoServer {
    fn echo<'s>(
        &'s mut self,
        request: echo::echo_request<'_>,
    ) -> Result<MethodReply<'s, echo::echo_response<'s>>, FailInvocation> {
        self.scratch.clear();
        self.scratch.extend_from_slice(request.payload);
        Ok(MethodReply::new(echo::echo_response {
            payload: &self.scratch,
        }))
    }
}

struct CountingEchoServer {
    scratch: Vec<u8>,
    served: u32,
}

impl echo::EchoHandler for CountingEchoServer {
    fn echo<'s>(
        &'s mut self,
        request: echo::echo_request<'_>,
    ) -> Result<MethodReply<'s, echo::echo_response<'s>>, FailInvocation> {
        self.scratch.clear();
        self.scratch.extend_from_slice(request.payload);
        self.served += 1;
        Ok(MethodReply::new(echo::echo_response {
            payload: &self.scratch,
        }))
    }
}

enum DeviceMode {
    Ok,
    DomainError,
}

struct TestDevice {
    mode: DeviceMode,
}

impl block_device::BlockDeviceHandler for TestDevice {
    fn get_info<'s>(
        &'s mut self,
        _request: block_device::get_info_request,
    ) -> Result<MethodReply<'s, block_device::get_info_response>, FailInvocation> {
        unreachable!("tests do not call get_info");
    }

    fn read<'s>(
        &'s mut self,
        _request: block_device::read_request,
    ) -> Result<MethodReply<'s, block_device::read_response>, FailInvocation> {
        match self.mode {
            DeviceMode::Ok => Ok(MethodReply::new(block_device::read_response {})),
            DeviceMode::DomainError => Err(FailInvocation::domain(block_device::READ_ERROR_EIO)),
        }
    }

    fn write<'s>(
        &'s mut self,
        _request: block_device::write_request,
    ) -> Result<MethodReply<'s, block_device::write_response>, FailInvocation> {
        unreachable!("tests do not call write");
    }

    fn flush<'s>(
        &'s mut self,
        _request: block_device::flush_request,
    ) -> Result<MethodReply<'s, block_device::flush_response>, FailInvocation> {
        unreachable!("tests do not call flush");
    }

    fn discard<'s>(
        &'s mut self,
        _request: block_device::discard_request,
    ) -> Result<MethodReply<'s, block_device::discard_response>, FailInvocation> {
        unreachable!("tests do not call discard");
    }
}

const MEMORY_OBJECT_SCOPE: u64 = 7;

fn memory_buffer(protocol_rights: u64, scope: u64) -> naos_idl::OwnedHandle {
    loopback::add_capability_with_rights(
        sys::BINDING_MEMORY_OBJECT,
        scope,
        sys::RIGHT_DUPLICATE | sys::RIGHT_TRANSFER,
        protocol_rights,
    )
}

const WRITE_RIGHT: u64 = 1 << 1; // memory_object_write
const MAP_RIGHT: u64 = 1 << 2; // memory_object_map

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

#[test]
fn echo_round_trip_matches_dispatcher() {
    install();
    let (client, server) = echo::create_endpoints(None).unwrap();

    let request = echo::echo_request { payload: b"" };
    let mut wire = [0u8; 4096];

    let mut invocation =
        echo::submit_echo(&client, &request, ResourceTable::new(), &mut wire, 1024).unwrap();

    // Server side: generic receive, then protocol dispatch.
    let mut server_wire = [0u8; 4096];
    let mut reply_wire = [0u8; 4096];
    let incoming = naos_idl::receive_request(&server, &mut server_wire).unwrap();
    assert_eq!(incoming.method_id, echo::METHOD_ECHO);
    assert_eq!(incoming.caller_pid, loopback::CALLER_PID);
    let outcome = echo::dispatch(&mut EchoServer::new(), incoming, &mut reply_wire).unwrap();
    assert_eq!(outcome, DispatchOutcome::Completed);

    // Client side: typed result.
    let mut result_wire = [0u8; 4096];
    let response = echo::take_echo(&mut invocation, &mut result_wire).unwrap();
    assert!(response.payload.is_empty());
}

#[test]
fn echo_payload_survives_the_full_client_server_cycle() {
    install();
    let (client, server) = echo::create_endpoints(None).unwrap();

    let request = echo::echo_request {
        payload: b"hello naos",
    };
    let mut wire = [0u8; 4096];
    let mut invocation =
        echo::submit_echo(&client, &request, ResourceTable::new(), &mut wire, 1024).unwrap();

    let mut server_wire = [0u8; 4096];
    let mut reply_wire = [0u8; 4096];
    let incoming = naos_idl::receive_request(&server, &mut server_wire).unwrap();
    let outcome = echo::dispatch(&mut EchoServer::new(), incoming, &mut reply_wire).unwrap();
    assert_eq!(outcome, DispatchOutcome::Completed);

    let mut result_wire = [0u8; 4096];
    let response = echo::take_echo(&mut invocation, &mut result_wire).unwrap();
    assert_eq!(response.payload, b"hello naos");
}

#[test]
fn serial_serve_loop_drains_pending_requests_then_reports_would_block() {
    install();
    let (client, server) = echo::create_endpoints(None).unwrap();

    let mut wire = [0u8; 4096];
    let mut invocations = Vec::new();
    for round in 0..3u8 {
        let payload = vec![round; round as usize + 1];
        let request = echo::echo_request { payload: &payload };
        invocations.push(
            echo::submit_echo(&client, &request, ResourceTable::new(), &mut wire, 1024).unwrap(),
        );
    }

    let mut handler = CountingEchoServer {
        scratch: Vec::new(),
        served: 0,
    };
    let mut server_wire = [0u8; 4096];
    let mut reply_wire = [0u8; 4096];
    // No more pending requests after the three: receive reports WOULD_BLOCK
    // and the serial loop terminates with that status.
    let error = echo::serve(&server, &mut handler, &mut server_wire, &mut reply_wire)
        .err()
        .expect("serve must stop on WOULD_BLOCK");
    assert!(matches!(error, CallError::Status(sys::STATUS_WOULD_BLOCK)));
    assert_eq!(handler.served, 3);

    for (round, mut invocation) in invocations.into_iter().enumerate() {
        let mut result_wire = [0u8; 4096];
        let response = echo::take_echo(&mut invocation, &mut result_wire).unwrap();
        let expected = vec![round as u8; round + 1];
        assert_eq!(response.payload, &expected[..]);
    }
}

#[test]
fn block_device_read_round_trip_transfers_duplicate_resource() {
    install();
    let (client, server) = block_device::create_endpoints(None).unwrap();

    let buffer = memory_buffer(WRITE_RIGHT | MAP_RIGHT, MEMORY_OBJECT_SCOPE);

    let mut resources = ResourceTable::new();
    let slot = resources.push_duplicate(&buffer).unwrap();
    let request = block_device::read_request {
        lba: 0,
        block_count: 0,
        buffer: slot,
        flags: 0,
    };
    let mut wire = [0u8; 512];

    let mut invocation =
        block_device::submit_read(&client, &request, resources, &mut wire, 1024).unwrap();

    let mut server_wire = [0u8; 512];
    let mut reply_wire = [0u8; 512];
    let incoming = naos_idl::receive_request(&server, &mut server_wire).unwrap();
    let outcome = block_device::dispatch(
        &mut TestDevice {
            mode: DeviceMode::Ok,
        },
        incoming,
        &mut reply_wire,
    )
    .unwrap();
    assert_eq!(outcome, DispatchOutcome::Completed);

    let mut result_wire = [0u8; 512];
    let _response = block_device::take_read(&mut invocation, &mut result_wire).unwrap();
    // DUPLICATE keeps the source capability alive in the caller.
    assert!(buffer.is_valid());
}

#[test]
fn object_identity_survives_move_transfer() {
    install();
    let (client, server) = block_device::create_endpoints(None).unwrap();
    let buffer = memory_buffer(WRITE_RIGHT | MAP_RIGHT, MEMORY_OBJECT_SCOPE);
    let identity = naos_idl::object_id(buffer.get()).unwrap();

    let mut resources = ResourceTable::new();
    let slot = resources.push_move(buffer).unwrap();
    let _invocation = loopback::raw_invoke_submit_with_resources(
        client.get(),
        block_device::METHOD_READ,
        &[],
        resources.as_slice(),
    )
    .unwrap();
    // The fake kernel has consumed the MOVE source; only the receiving side
    // remains observable after the raw submit. Dropping the table is safe:
    // its owner now contains the consumed source handle.
    drop(resources);

    let mut server_wire = [0u8; 512];
    let incoming = naos_idl::receive_request(&server, &mut server_wire).unwrap();
    let received = incoming.resources.get(slot).unwrap();
    assert_eq!(naos_idl::object_id(received.get()).unwrap(), identity);
}

#[test]
fn block_device_domain_error_maps_to_negative_errno_outcome() {
    install();
    let (client, server) = block_device::create_endpoints(None).unwrap();

    let buffer = memory_buffer(WRITE_RIGHT | MAP_RIGHT, MEMORY_OBJECT_SCOPE);
    let mut resources = ResourceTable::new();
    let slot = resources.push_duplicate(&buffer).unwrap();
    let request = block_device::read_request {
        lba: 4,
        block_count: 1,
        buffer: slot,
        flags: 0,
    };
    let mut wire = [0u8; 512];
    let mut invocation =
        block_device::submit_read(&client, &request, resources, &mut wire, 1024).unwrap();

    let mut server_wire = [0u8; 512];
    let mut reply_wire = [0u8; 512];
    let incoming = naos_idl::receive_request(&server, &mut server_wire).unwrap();
    let outcome = block_device::dispatch(
        &mut TestDevice {
            mode: DeviceMode::DomainError,
        },
        incoming,
        &mut reply_wire,
    )
    .unwrap();
    assert_eq!(outcome, DispatchOutcome::Completed);

    let mut result_wire = [0u8; 512];
    let error = block_device::take_read(&mut invocation, &mut result_wire).unwrap_err();
    match error {
        CallError::Outcome {
            execution,
            reason,
            protocol_error,
        } => {
            assert_eq!(execution, naos_idl::EXECUTION_NONE);
            assert_eq!(reason, naos_idl::REASON_NONE);
            assert_eq!(protocol_error, block_device::READ_ERROR_EIO);
            assert_eq!(protocol_error, -5); // -EIO
        }
        other => panic!("expected domain-error outcome, got {other:?}"),
    }
    if let Some(name) = block_device::read_error_name(-5) {
        assert_eq!(name, "EIO");
    } else {
        panic!("READ_ERROR_EIO missing from the generated @errors name table");
    }
}

/// Submit a raw (possibly malformed) request straight through the seam and
/// report what dispatch did with it.
fn raw_submit_and_dispatch(
    client: &naos_idl::ProtocolClientEndpoint,
    server: &naos_idl::ProtocolServerEndpoint,
    method_id: u64,
    payload: &[u8],
) -> (DispatchOutcome, naos_idl::loopback::RawResult) {
    let invocation = loopback::raw_invoke_submit(client.get(), method_id, payload).unwrap();

    let mut server_wire = [0u8; 4096];
    let mut reply_wire = [0u8; 4096];
    let incoming = naos_idl::receive_request(server, &mut server_wire).unwrap();
    let outcome = echo::dispatch(&mut EchoServer::new(), incoming, &mut reply_wire).unwrap();

    let result = loopback::raw_take_result(invocation.get()).unwrap();
    (outcome, result)
}

#[test]
fn unknown_method_id_is_rejected_as_unsupported() {
    install();
    let (client, server) = echo::create_endpoints(None).unwrap();
    let (outcome, result) = raw_submit_and_dispatch(&client, &server, 250, &[]);
    assert_eq!(outcome, DispatchOutcome::Rejected);
    assert_eq!(result.execution, naos_idl::EXECUTION_NOT_DELIVERED);
    assert_eq!(result.reason, naos_idl::REASON_UNSUPPORTED);
    assert_eq!(result.protocol_error, 0);
}

#[test]
fn malformed_wire_is_rejected_as_protocol_violation() {
    install();
    let (client, server) = echo::create_endpoints(None).unwrap();
    let request = echo::echo_request { payload: b"" };
    let mut encoded = [0u8; 4096];
    let written = echo::encode_echo_request(&request, &mut encoded).unwrap();
    let truncated = &encoded[..written - 1];
    let (outcome, result) = raw_submit_and_dispatch(&client, &server, echo::METHOD_ECHO, truncated);
    assert_eq!(outcome, DispatchOutcome::Rejected);
    assert_eq!(result.reason, naos_idl::REASON_PROTOCOL_VIOLATION);
    assert_eq!(result.protocol_error, 0);
}

#[test]
fn received_resource_metadata_is_enforced_by_the_dispatcher() {
    install();

    // Wrong scope: a block-device-scoped handle cannot satisfy a
    // memory_object buffer requirement.
    let (client, server) = block_device::create_endpoints(None).unwrap();
    let buffer = memory_buffer(WRITE_RIGHT | MAP_RIGHT, 17);
    let mut resources = ResourceTable::new();
    let slot = resources.push_duplicate(&buffer).unwrap();
    let request = block_device::read_request {
        lba: 0,
        block_count: 1,
        buffer: slot,
        flags: 0,
    };
    let mut wire = [0u8; 512];
    let _invocation =
        block_device::submit_read(&client, &request, resources, &mut wire, 1024).unwrap();
    let mut server_wire = [0u8; 512];
    let mut reply_wire = [0u8; 512];
    let incoming = naos_idl::receive_request(&server, &mut server_wire).unwrap();
    let outcome = block_device::dispatch(
        &mut TestDevice {
            mode: DeviceMode::Ok,
        },
        incoming,
        &mut reply_wire,
    )
    .unwrap();
    assert_eq!(outcome, DispatchOutcome::Rejected);

    // Missing directional permission: a buffer without memory_object_read is
    // rejected even though the scope matches.
    let (client, server) = block_device::create_endpoints(None).unwrap();
    let buffer = memory_buffer(0, MEMORY_OBJECT_SCOPE);
    let mut resources = ResourceTable::new();
    let slot = resources.push_duplicate(&buffer).unwrap();
    let request = block_device::read_request {
        lba: 0,
        block_count: 1,
        buffer: slot,
        flags: 0,
    };
    let mut wire = [0u8; 512];
    let _invocation =
        block_device::submit_read(&client, &request, resources, &mut wire, 1024).unwrap();
    let mut server_wire = [0u8; 512];
    let mut reply_wire = [0u8; 512];
    let incoming = naos_idl::receive_request(&server, &mut server_wire).unwrap();
    let outcome = block_device::dispatch(
        &mut TestDevice {
            mode: DeviceMode::Ok,
        },
        incoming,
        &mut reply_wire,
    )
    .unwrap();
    assert_eq!(outcome, DispatchOutcome::Rejected);
}
