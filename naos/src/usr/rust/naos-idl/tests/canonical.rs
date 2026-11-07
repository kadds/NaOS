extern crate alloc;

#[cfg(feature = "alloc")]
use naos_idl::CodecError;
use naos_idl::{Decoder, Encoder, OwnedHandle, ResourceError, ResourceTransferBuilder, echo};

const TEST_FIRMWARE_VERSION: &str = "01000000000000000200000000000000";

#[test]
fn canonical_integer_and_bytes_encoding_matches_naos_wire() {
    let mut bytes = [0_u8; 32];
    let mut encoder = Encoder::new(&mut bytes);
    encoder.put_u64(0x0102_0304_0506_0708).unwrap();
    encoder.put_bytes(b"echo").unwrap();
    assert_eq!(
        encoder.as_bytes(),
        &[8, 7, 6, 5, 4, 3, 2, 1, b'e', b'c', b'h', b'o']
    );
    let written = encoder.written();
    drop(encoder);

    let mut decoder = Decoder::new(&bytes[..written]);
    assert_eq!(decoder.get_u64().unwrap(), 0x0102_0304_0506_0708);
    assert_eq!(decoder.get_bytes(4).unwrap(), b"echo");
    assert!(decoder.is_empty());
}

#[test]
fn canonical_decoder_rejects_truncated_input() {
    let mut decoder = Decoder::new(&[1, 2, 3]);
    assert!(decoder.get_u64().is_err());
    assert!(decoder.is_failed());
}

#[test]
fn owned_handle_is_move_only_at_the_api_boundary() {
    let owner = unsafe { OwnedHandle::from_raw(0x42) };
    let raw = owner.into_raw();
    assert_eq!(raw, 0x42);
}

#[test]
fn generated_echo_binding_round_trips_the_cpp_canonical_shape() {
    let value = echo::echo_request { payload: b"hello" };
    let mut bytes = [0_u8; 64];
    let written = echo::encode_echo_request(&value, &mut bytes).unwrap();
    assert_eq!(
        &bytes[..written],
        &[8, 0, 0, 0, 5, 0, 0, 0, b'h', b'e', b'l', b'l', b'o']
    );

    let decoded = echo::decode_echo_request(&bytes[..written]).unwrap();
    assert_eq!(decoded.payload, b"hello");

    bytes[0] = 7;
    assert!(echo::decode_echo_request(&bytes[..written]).is_err());
}

#[test]
fn generated_echo_binding_exposes_protocol_descriptor_and_typed_endpoints() {
    let descriptor = echo::protocol_descriptor();
    assert_eq!(
        descriptor.struct_size as usize,
        core::mem::size_of::<naos_sys::ProtocolDescriptor>()
    );
    assert_eq!(descriptor.uuid.bytes, echo::PROTOCOL_UUID);
    assert_eq!(descriptor.scope, echo::PROTOCOL_SCOPE);
    assert_eq!(descriptor.revision, 1);
    assert_eq!(descriptor.method_count, 1);
    assert_eq!(descriptor.method_bitmap[0], 1);
    assert_eq!(descriptor.method_rights[0], naos_sys::PROTOCOL_RIGHT_INVOKE);

    let endpoint = unsafe { naos_idl::ProtocolClientEndpoint::from_raw(0x42) };
    assert_eq!(endpoint.into_raw(), 0x42);
}

#[test]
fn resource_table_makes_move_and_duplicate_operations_explicit() {
    let moved = unsafe { OwnedHandle::from_raw(0x42) };
    let borrowed = unsafe { OwnedHandle::from_raw(0x43) };
    let mut table = ResourceTransferBuilder::new();
    let move_slot = table.push_move(moved).unwrap();
    let duplicate_slot = table.push_duplicate(&borrowed).unwrap();
    assert_eq!(move_slot.index(), 0);
    assert_eq!(duplicate_slot.index(), 1);
    assert_eq!(table.as_slice()[0].operation, naos_sys::RESOURCE_MOVE);
    assert_eq!(table.as_slice()[1].operation, naos_sys::RESOURCE_DUPLICATE);
    table.commit_move();
    core::mem::forget(borrowed);

    assert_eq!(
        ResourceTransferBuilder::new().push_move(OwnedHandle::invalid()),
        Err(ResourceError::InvalidHandle)
    );
}

#[test]
fn resource_transfer_builder_keeps_small_stack_storage_and_spills_to_the_abi_limit() {
    assert!(core::mem::size_of::<ResourceTransferBuilder<'static>>() < 512);

    let borrowed = unsafe { OwnedHandle::from_raw(0x44) };
    let mut builder = ResourceTransferBuilder::new();
    for _ in 0..naos_idl::MAX_RESOURCES {
        builder.push_duplicate(&borrowed).unwrap();
    }
    assert_eq!(builder.len(), naos_idl::MAX_RESOURCES);
    assert_eq!(builder.push_duplicate(&borrowed), Err(ResourceError::Full));
    core::mem::forget(borrowed);
}

#[cfg(feature = "alloc")]
#[test]
fn generated_vector_binding_round_trips_fixed_elements() {
    let value = naos_idl::vector::values_request {
        items: alloc::vec![1, 2, 3],
    };
    let mut bytes = [0_u8; 64];
    let written = naos_idl::vector::encode_values_request(&value, &mut bytes).unwrap();
    assert_eq!(
        &bytes[..written],
        &[8, 0, 0, 0, 3, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 3, 0, 0, 0]
    );
    let decoded = naos_idl::vector::decode_values_request(&bytes[..written]).unwrap();
    assert_eq!(decoded.items, alloc::vec![1, 2, 3]);
}

#[cfg(feature = "alloc")]
#[test]
fn generated_inline_vector_binding_round_trips_fixed_elements() {
    let value = naos_idl::vector::inline_values_request {
        items: alloc::vec![4, 5, 6],
    };
    let mut bytes = [0_u8; 64];
    let written = naos_idl::vector::encode_inline_values_request(&value, &mut bytes).unwrap();
    assert_eq!(&bytes[..written], &[4, 0, 0, 0, 5, 0, 0, 0, 6, 0, 0, 0]);
    let decoded = naos_idl::vector::decode_inline_values_request(&bytes[..written]).unwrap();
    assert_eq!(decoded.items, alloc::vec![4, 5, 6]);
}

#[test]
fn generated_nested_array_binding_round_trips_fixed_elements() {
    let value = naos_idl::vector::matrix_request {
        items: [[1, 2], [3, 4]],
    };
    let mut bytes = [0_u8; 16];
    let written = naos_idl::vector::encode_matrix_request(&value, &mut bytes).unwrap();
    assert_eq!(&bytes[..written], &[1, 0, 2, 0, 3, 0, 4, 0]);
    let decoded = naos_idl::vector::decode_matrix_request(&bytes[..written]).unwrap();
    assert_eq!(decoded.items, [[1, 2], [3, 4]]);
}

#[cfg(feature = "alloc")]
#[test]
fn generated_resource_binding_preserves_resource_slots() {
    let value = naos_idl::resources::pass_request {
        values: alloc::vec![naos_idl::ResourceSlot::new(0).unwrap()],
        pair: naos_idl::resources::ResourcePair {
            first: naos_idl::ResourceSlot::new(1).unwrap(),
            second: naos_idl::ResourceSlot::new(2).unwrap(),
        },
    };
    let mut bytes = [0_u8; 64];
    let written = naos_idl::resources::encode_pass_request(&value, &mut bytes).unwrap();
    let decoded = naos_idl::resources::decode_pass_request(&bytes[..written]).unwrap();
    assert_eq!(decoded.values[0].index(), 0);
    assert_eq!(decoded.pair.first.index(), 1);
    assert_eq!(decoded.pair.second.index(), 2);
}

#[cfg(feature = "alloc")]
#[test]
fn generated_resource_binding_validates_disposition_contract() {
    let value = naos_idl::resources::pass_request {
        values: alloc::vec![naos_idl::ResourceSlot::new(0).unwrap()],
        pair: naos_idl::resources::ResourcePair {
            first: naos_idl::ResourceSlot::new(1).unwrap(),
            second: naos_idl::ResourceSlot::new(2).unwrap(),
        },
    };
    let dispositions = [
        naos_sys::ResourceDisposition {
            handle: 0x10,
            operation: naos_sys::RESOURCE_MOVE,
            rights: naos_sys::RIGHT_TRANSFER,
            scope: 1,
            ..naos_sys::ResourceDisposition::default()
        },
        naos_sys::ResourceDisposition {
            handle: 0x11,
            operation: naos_sys::RESOURCE_DUPLICATE,
            rights: naos_sys::RIGHT_DUPLICATE | naos_sys::RIGHT_TRANSFER,
            scope: 1,
            ..naos_sys::ResourceDisposition::default()
        },
        naos_sys::ResourceDisposition {
            handle: 0x12,
            operation: naos_sys::RESOURCE_MOVE,
            rights: naos_sys::RIGHT_TRANSFER,
            scope: 1,
            ..naos_sys::ResourceDisposition::default()
        },
    ];
    assert!(naos_idl::resources::validate_pass_request_resources(&value, &dispositions).is_ok());

    let mut invalid = dispositions;
    invalid[1].operation = naos_sys::RESOURCE_MOVE;
    assert_eq!(
        naos_idl::resources::validate_pass_request_resources(&value, &invalid),
        Err(CodecError::InvalidResource)
    );
}

#[cfg(feature = "alloc")]
#[test]
fn generated_servicekit_admission_validator_decodes_by_method() {
    use naos_idl::directory;
    use naos_idl::transport::RpcRequestOwned;

    let value = directory::open_request {
        mode: 1,
        flags: 0,
        path: b"/",
    };
    let mut wire = [0_u8; 64];
    let written = directory::encode_open_request(&value, &mut wire).unwrap();
    let request = RpcRequestOwned {
        protocol_uuid: directory::PROTOCOL_UUID,
        revision: directory::PROTOCOL_REVISION,
        method_id: directory::METHOD_OPEN,
        request_id: 1,
        target: None,
        payload: alloc::vec::Vec::from(&wire[..written]),
        resources: alloc::vec::Vec::new(),
        bulk: alloc::vec::Vec::new(),
    };

    assert_eq!(
        directory::validate_request_transport_resources(&request),
        Ok(())
    );

    let mut malformed = request.clone();
    malformed.payload.clear();
    assert_eq!(
        directory::validate_request_transport_resources(&malformed),
        Err(-22)
    );

    malformed.method_id = 0;
    assert_eq!(
        directory::validate_request_transport_resources(&malformed),
        Err(-38)
    );
}

#[cfg(feature = "alloc")]
#[test]
fn generated_servicekit_dispatch_routes_and_encodes_typed_requests() {
    use naos_idl::echo;
    use naos_idl::transport::{RpcDispatchResult, RpcMethodReply, RpcOutcome, RpcRequest};

    struct EchoHandler {
        reply: alloc::vec::Vec<u8>,
    }

    impl echo::EchoServiceKitHandler<()> for EchoHandler {
        type Resource = ();

        async fn echo<'s>(
            &'s mut self,
            _context: &mut (),
            request: echo::echo_request<'_>,
        ) -> Result<RpcMethodReply<echo::echo_response<'s>, Self::Resource>, RpcOutcome> {
            self.reply.clear();
            self.reply.extend_from_slice(request.payload);
            Ok(RpcMethodReply::new(echo::echo_response {
                payload: &self.reply,
            }))
        }
    }

    let value = echo::echo_request { payload: b"typed" };
    let mut request_wire = [0; 64];
    let request_bytes = echo::encode_echo_request(&value, &mut request_wire).unwrap();
    let request = RpcRequest {
        protocol_uuid: echo::PROTOCOL_UUID,
        revision: echo::PROTOCOL_REVISION,
        method_id: echo::METHOD_ECHO,
        request_id: 17,
        target: None,
        payload: &request_wire[..request_bytes],
        resources: &[],
        bulk: &[],
    };
    let mut handler = EchoHandler {
        reply: alloc::vec::Vec::new(),
    };
    let mut context = ();

    let dispatched = {
        use core::future::Future;
        use std::task::{Context as TaskContext, Poll, Waker};

        let future = echo::dispatch_servicekit(&request, &mut context, &mut handler);
        let mut future = core::pin::pin!(future);
        let waker = Waker::noop();
        let mut task_context = TaskContext::from_waker(waker);
        match future.as_mut().poll(&mut task_context) {
            Poll::Ready(value) => value,
            Poll::Pending => panic!("synchronous test handler must be immediately ready"),
        }
    };
    assert_eq!(dispatched.method_id, echo::METHOD_ECHO);
    assert_eq!(dispatched.outcome, RpcOutcome::SUCCESS);
    assert!(dispatched.resources.is_empty());
    let response = echo::decode_echo_response(&dispatched.payload).unwrap();
    assert_eq!(response.payload, b"typed");

    let unknown = RpcRequest {
        method_id: 0,
        ..request
    };
    let dispatched: RpcDispatchResult<()> = {
        use core::future::Future;
        use std::task::{Context as TaskContext, Poll, Waker};

        let future = echo::dispatch_servicekit(&unknown, &mut context, &mut handler);
        let mut future = core::pin::pin!(future);
        let waker = Waker::noop();
        let mut task_context = TaskContext::from_waker(waker);
        match future.as_mut().poll(&mut task_context) {
            Poll::Ready(value) => value,
            Poll::Pending => panic!("unknown method dispatch must be immediately ready"),
        }
    };
    assert_eq!(dispatched.outcome.protocol_error, -38);
}

#[cfg(feature = "alloc")]
#[test]
fn platform_pci_feature_response_preserves_compatibility_profile_identity() {
    use naos_idl::platform_pci_topology as topology;

    assert_eq!(
        topology::PciRootFeatureSupport::segment_groups as u32,
        1 << 3
    );
    let profile_hash = 0x0123_4567_89ab_cdef;
    let response = topology::negotiate_pci_features_response {
        firmware_version: TEST_FIRMWARE_VERSION,
        value: topology::PciRootFeatureSet {
            root_id: 1,
            requested_support: 2,
            requested_control: 3,
            query_support: 4,
            query_granted_control: 5,
            commit_support: 6,
            granted_control: 7,
            denied_control: 8,
            masked_control: 9,
            query_errors: 10,
            commit_errors: 11,
            aml_namespace_generation: 14,
            platform_topology_generation: 15,
            compatibility_profile_hash: profile_hash,
            pci_fabric_generation: 16,
        },
    };
    let mut wire = [0_u8; 256];
    let written = topology::encode_negotiate_pci_features_response(&response, &mut wire).unwrap();
    let decoded = topology::decode_negotiate_pci_features_response(&wire[..written]).unwrap();

    assert_eq!(decoded.value.query_granted_control, 5);
    assert_eq!(decoded.value.commit_support, 6);
    assert_eq!(decoded.value.pci_fabric_generation, 16);
    assert_eq!(decoded.value.compatibility_profile_hash, profile_hash);
    assert_eq!(topology::protocol_descriptor().revision, 1);
}

#[cfg(feature = "alloc")]
#[test]
fn platform_pci_roots_round_trip_device_identity_and_flat_resource_windows() {
    use naos_idl::platform_pci_topology as topology;

    let compatibility_profile_hash = 0xfeed_cafe_1234_5678;
    let response = topology::get_pci_roots_response {
        firmware_version: TEST_FIRMWARE_VERSION,
        roots: alloc::vec![topology::PlatformPciRoot {
            root_id: 0x10,
            segment: 0,
            start_bus: 32,
            end_bus: 63,
            aml_namespace_generation: 3,
            platform_topology_generation: 4,
            pci_fabric_generation: 5,
            device_id: 0x20,
            resource_window_start: 0,
            resource_window_count: 2,
            compatibility_profile_hash,
        }],
        resource_windows: alloc::vec![
            topology::PlatformPciResourceWindow {
                resource_type: 0,
                descriptor_revision: 1,
                general_flags: 12,
                type_specific_flags: 3,
                granularity: 0,
                minimum: 0x8000_0000,
                maximum: 0x8fff_ffff,
                translation_offset: 0,
                length: 0x1000_0000,
                type_specific_attributes: 0,
            },
            topology::PlatformPciResourceWindow {
                resource_type: 2,
                descriptor_revision: 0,
                general_flags: 12,
                type_specific_flags: 0,
                granularity: 0,
                minimum: 32,
                maximum: 63,
                translation_offset: 0,
                length: 32,
                type_specific_attributes: 0,
            },
        ],
    };
    let mut wire = [0_u8; 1024];
    let written = topology::encode_get_pci_roots_response(&response, &mut wire).unwrap();
    let decoded = topology::decode_get_pci_roots_response(&wire[..written]).unwrap();

    assert_eq!(decoded.roots.len(), 1);
    assert_eq!(decoded.roots[0].root_id, response.roots[0].root_id);
    assert_eq!(decoded.roots[0].device_id, response.roots[0].device_id);
    assert_eq!(
        decoded.roots[0].compatibility_profile_hash,
        compatibility_profile_hash
    );
    assert_eq!(
        decoded.roots[0].resource_window_start,
        response.roots[0].resource_window_start
    );
    assert_eq!(
        decoded.roots[0].resource_window_count,
        response.roots[0].resource_window_count
    );
    assert_eq!(decoded.resource_windows.len(), 2);
    assert_eq!(decoded.resource_windows[0].minimum, 0x8000_0000);
    assert_eq!(decoded.resource_windows[1].resource_type, 2);
    assert_eq!(topology::protocol_descriptor().revision, 1);

    let mut max_roots = alloc::vec![response.roots[0].clone(); 64];
    for (index, root) in max_roots.iter_mut().enumerate() {
        root.resource_window_start = (index * 12) as u32;
        root.resource_window_count = 12;
    }
    let max_response = topology::get_pci_roots_response {
        firmware_version: TEST_FIRMWARE_VERSION,
        roots: max_roots,
        resource_windows: alloc::vec![response.resource_windows[0].clone(); 768],
    };
    let mut max_wire = alloc::vec![0_u8; 65_536];
    let max_written =
        topology::encode_get_pci_roots_response(&max_response, &mut max_wire).unwrap();
    assert!(max_written <= 65_536);
}

#[cfg(feature = "alloc")]
#[test]
fn platform_generation_binds_the_compatibility_profile_hash() {
    use naos_idl::platform_pci_topology as topology;

    let profile_hash = 0xfeed_cafe_1234_5678;
    let response = topology::generation_response {
        firmware_version: TEST_FIRMWARE_VERSION,
        value: topology::PlatformGeneration {
            aml_namespace_generation: 3,
            platform_topology_generation: 4,
            pci_fabric_generation: 5,
            health: 0,
            compatibility_profile_hash: profile_hash,
        },
    };
    let mut wire = [0_u8; 128];
    let written = topology::encode_generation_response(&response, &mut wire).unwrap();
    let decoded = topology::decode_generation_response(&wire[..written]).unwrap();
    assert_eq!(decoded.value.compatibility_profile_hash, profile_hash);
}

#[cfg(feature = "alloc")]
#[test]
fn platform_device_snapshot_and_resource_methods_round_trip_with_bounded_flat_tables() {
    use naos_idl::platform_pci_topology as topology;

    let request = topology::list_devices_request {
        firmware_version: TEST_FIRMWARE_VERSION,
        platform_topology_generation: 0x0304,
        cursor: 5,
        limit: 4,
        filter_flags: 3,
    };
    let mut request_wire = [0_u8; 64];
    let request_written =
        topology::encode_list_devices_request(&request, &mut request_wire).unwrap();
    let decoded_request =
        topology::decode_list_devices_request(&request_wire[..request_written]).unwrap();
    assert_eq!(decoded_request.firmware_version, TEST_FIRMWARE_VERSION);
    assert_eq!(decoded_request.cursor, 5);
    assert_eq!(decoded_request.filter_flags, 3);

    let response = topology::list_devices_response {
        firmware_version: TEST_FIRMWARE_VERSION,
        devices: alloc::vec![topology::PlatformDeviceSnapshot {
            device_id: 0x11,
            namespace_generation: 0x22,
            status: 0xf,
            evaluation_flags: 0x1f,
            initialized: 1,
            degraded: 0,
            identifier_start: 0,
            identifier_count: 1,
            resource_start: 0,
            resource_count: 1,
            routing_entries: 2,
            pci_root_id: 7,
            pci_path_start: 0,
            pci_path_count: 2,
            platform_topology_generation: 3,
            pci_fabric_generation: 4,
        }],
        identifiers: alloc::vec![topology::PlatformDeviceIdentifier {
            kind: 1,
            value_kind: 1,
            text_start: 0,
            text_length: 7,
            integer: 0,
        }],
        resources: alloc::vec![topology::PlatformDeviceResource {
            kind: 2,
            base: 0x8000_0000,
            length: 0x1000,
            flags: 3,
        }],
        identifier_text: b"PNP0A08".to_vec(),
        next_cursor: 0,
        pci_device_function_path: alloc::vec![1, 0x42],
    };
    let mut response_wire = alloc::vec![0_u8; 65_536];
    let response_written =
        topology::encode_list_devices_response(&response, &mut response_wire).unwrap();
    let decoded =
        topology::decode_list_devices_response(&response_wire[..response_written]).unwrap();
    assert_eq!(decoded.devices[0].device_id, 0x11);
    assert_eq!(decoded.devices[0].evaluation_flags, 0x1f);
    assert_eq!(decoded.identifiers[0].text_length, 7);
    assert_eq!(decoded.identifier_text, b"PNP0A08");
    assert_eq!(decoded.resources[0].base, 0x8000_0000);
    assert_eq!(decoded.devices[0].pci_root_id, 7);
    assert_eq!(decoded.devices[0].pci_path_count, 2);
    assert_eq!(decoded.pci_device_function_path, [1, 0x42]);

    let resources = topology::get_resources_response {
        resources: response.resources,
        evaluated: 1,
        degraded: 0,
    };
    let mut resource_wire = alloc::vec![0_u8; 65_536];
    let resources_written =
        topology::encode_get_resources_response(&resources, &mut resource_wire).unwrap();
    let decoded_resources =
        topology::decode_get_resources_response(&resource_wire[..resources_written]).unwrap();
    assert_eq!(decoded_resources.resources[0].length, 0x1000);
    assert_eq!(decoded_resources.evaluated, 1);

    let max_devices = topology::list_devices_response {
        firmware_version: TEST_FIRMWARE_VERSION,
        devices: alloc::vec![decoded.devices[0].clone(); 4],
        identifiers: alloc::vec![decoded.identifiers[0].clone(); 72],
        resources: alloc::vec![decoded.resources[0].clone(); 1024],
        identifier_text: alloc::vec![b'x'; 72 * 255],
        next_cursor: 0,
        pci_device_function_path: alloc::vec![0x42; 64],
    };
    let mut max_device_wire = alloc::vec![0_u8; 65_536];
    let max_written =
        topology::encode_list_devices_response(&max_devices, &mut max_device_wire).unwrap();
    assert!(max_written <= 65_536);
    assert_eq!(topology::protocol_descriptor().revision, 1);
}

#[cfg(feature = "alloc")]
#[test]
fn platform_pci_routing_pages_round_trip_typed_source_kinds() {
    use naos_idl::platform_pci_topology as topology;

    let response = topology::get_interrupt_routing_response {
        routes: alloc::vec![
            topology::PlatformPciRoutingEntry {
                address: 0x0002_ffff,
                pin: 0,
                source_kind: topology::PciRoutingSourceKind::direct_gsi,
                source_device_id: 0,
                source_index: 23,
            },
            topology::PlatformPciRoutingEntry {
                address: 0x0003_ffff,
                pin: 3,
                source_kind: topology::PciRoutingSourceKind::link_device,
                source_device_id: 0xabc,
                source_index: 1,
            },
        ],
        next_cursor: 0,
        evaluated: 1,
        complete: 1,
    };
    let mut wire = alloc::vec![0; 1024];
    let written = topology::encode_get_interrupt_routing_response(&response, &mut wire).unwrap();
    let decoded = topology::decode_get_interrupt_routing_response(&wire[..written]).unwrap();

    assert_eq!(decoded.routes.len(), 2);
    assert_eq!(
        decoded.routes[0].source_kind,
        topology::PciRoutingSourceKind::direct_gsi
    );
    assert_eq!(decoded.routes[0].source_index, 23);
    assert_eq!(
        decoded.routes[1].source_kind,
        topology::PciRoutingSourceKind::link_device
    );
    assert_eq!(decoded.routes[1].source_device_id, 0xabc);
    assert_eq!(decoded.routes[1].source_index, 1);
    assert_eq!(decoded.complete, 1);
    assert_eq!(topology::protocol_descriptor().revision, 1);
}

#[cfg(feature = "alloc")]
#[test]
fn acpi_executor_thread_binding_round_trips_the_kernel_designated_tid() {
    use naos_idl::acpi_resource_authority as authority;

    let request = authority::bind_execution_thread_request {
        firmware_version: TEST_FIRMWARE_VERSION,
        request: authority::AcpiResourceThreadBindRequest {
            execution_thread_id: 0x3344,
        },
    };
    let mut wire = [0_u8; 64];
    let written = authority::encode_bind_execution_thread_request(&request, &mut wire).unwrap();
    let decoded = authority::decode_bind_execution_thread_request(&wire[..written]).unwrap();
    assert_eq!(decoded.request.execution_thread_id, 0x3344);
}

#[cfg(feature = "alloc")]
#[test]
fn acpi_executor_osc_evidence_round_trips_for_validation_without_minting() {
    use naos_idl::acpi_resource_authority as authority;

    let request = authority::validate_pci_root_features_request {
        firmware_version: TEST_FIRMWARE_VERSION,
        evidence: authority::PciRootFeatureEvidence {
            pci_config_scope_id: 17,
            root_id: 18,
            aml_namespace_generation: 21,
            platform_topology_generation: 22,
            pci_fabric_generation: 23,
            compatibility_profile_hash: 24,
            segment: 2,
            start_bus: 32,
            end_bus: 63,
            requested_support: 1 << 3,
            requested_control: 1 << 4,
            query_support: 1 << 3,
            query_granted_control: 1 << 4,
            commit_support: 1 << 3,
            granted_control: 1 << 4,
            denied_control: 0,
            masked_control: 0,
            query_errors: 0,
            commit_errors: 0,
        },
    };
    let mut wire = [0_u8; 256];
    let written =
        authority::encode_validate_pci_root_features_request(&request, &mut wire).unwrap();
    let decoded = authority::decode_validate_pci_root_features_request(&wire[..written]).unwrap();
    assert_eq!(decoded.evidence.pci_config_scope_id, 17);
    assert_eq!(decoded.evidence.granted_control, 1 << 4);
    assert_eq!(authority::protocol_descriptor().revision, 1);
}

#[cfg(feature = "alloc")]
#[test]
fn acpi_root_resource_attestation_round_trips_bounded_crs_windows() {
    use naos_idl::acpi_resource_authority as authority;

    let request = authority::mint_pci_root_resource_attestation_request {
        pci_config_scope_id: 17,
        root_id: 18,
        firmware_version: TEST_FIRMWARE_VERSION,
        aml_namespace_generation: 21,
        platform_topology_generation: 22,
        pci_fabric_generation: 23,
        compatibility_profile_hash: 24,
        segment: 2,
        start_bus: 32,
        end_bus: 63,
        windows: alloc::vec![
            authority::PciRootResourceWindow {
                resource_type: 2,
                descriptor_revision: 1,
                general_flags: 0,
                type_specific_flags: 0,
                granularity: 0,
                minimum: 32,
                maximum: 63,
                translation_offset: 0,
                length: 32,
                type_specific_attributes: 0,
            },
            authority::PciRootResourceWindow {
                resource_type: 0,
                descriptor_revision: 1,
                general_flags: 0,
                type_specific_flags: 0,
                granularity: 0xffff,
                minimum: 0x8000_0000,
                maximum: 0x80ff_ffff,
                translation_offset: 0,
                length: 0x0100_0000,
                type_specific_attributes: 0,
            },
        ],
    };
    let mut wire = alloc::vec![0_u8; 1024];
    let written =
        authority::encode_mint_pci_root_resource_attestation_request(&request, &mut wire).unwrap();
    let decoded =
        authority::decode_mint_pci_root_resource_attestation_request(&wire[..written]).unwrap();
    assert_eq!(decoded.pci_config_scope_id, 17);
    assert_eq!(decoded.windows.len(), 2);
    assert_eq!(decoded.windows[1].minimum, 0x8000_0000);
    assert_eq!(decoded.windows[1].length, 0x0100_0000);
    assert_eq!(authority::protocol_descriptor().revision, 1);
}

#[cfg(feature = "alloc")]
#[test]
fn acpi_fixed_hardware_abort_transition_binding_round_trips_ticket_identity() {
    use naos_idl::acpi_fixed_hardware as fixed_hardware;

    let request = fixed_hardware::abort_transition_request {
        ticket: 0x21,
        firmware_version: TEST_FIRMWARE_VERSION,
    };
    let mut wire = [0_u8; 64];
    let written = fixed_hardware::encode_abort_transition_request(&request, &mut wire).unwrap();
    let decoded = fixed_hardware::decode_abort_transition_request(&wire[..written]).unwrap();

    assert_eq!(decoded.ticket, request.ticket);
    assert_eq!(decoded.firmware_version, TEST_FIRMWARE_VERSION);
    let descriptor = fixed_hardware::protocol_descriptor();
    assert_eq!(descriptor.revision, 1);
    assert_eq!(descriptor.method_count, 13);
    assert_eq!(
        descriptor.method_bitmap[0] & (1_u64 << (fixed_hardware::METHOD_ABORT_TRANSITION - 1)),
        1 << 11
    );
}

#[cfg(feature = "alloc")]
#[test]
fn acpi_fixed_hardware_transition_owner_binding_round_trips_generation() {
    use naos_idl::acpi_fixed_hardware as fixed_hardware;

    let request = fixed_hardware::claim_transition_owner_request {
        firmware_version: TEST_FIRMWARE_VERSION,
    };
    let mut wire = [0_u8; 64];
    let written =
        fixed_hardware::encode_claim_transition_owner_request(&request, &mut wire).unwrap();
    let decoded = fixed_hardware::decode_claim_transition_owner_request(&wire[..written]).unwrap();

    assert_eq!(decoded.firmware_version, TEST_FIRMWARE_VERSION);
    assert_eq!(fixed_hardware::METHOD_CLAIM_TRANSITION_OWNER, 13);
    assert_eq!(fixed_hardware::protocol_descriptor().revision, 1);
}

#[cfg(feature = "alloc")]
#[test]
fn acpi_executor_binds_platform_generations_as_one_kernel_validated_lineage() {
    use naos_idl::acpi_resource_authority as authority;

    let request = authority::bind_platform_generation_request {
        firmware_version: TEST_FIRMWARE_VERSION,
        binding: authority::AcpiPlatformGenerationBinding {
            aml_namespace_generation: 13,
            platform_topology_generation: 14,
        },
    };
    let mut wire = [0_u8; 64];
    let written = authority::encode_bind_platform_generation_request(&request, &mut wire).unwrap();
    let decoded = authority::decode_bind_platform_generation_request(&wire[..written]).unwrap();
    assert_eq!(decoded.binding.aml_namespace_generation, 13);
    assert_eq!(decoded.binding.platform_topology_generation, 14);
    let descriptor = authority::protocol_descriptor();
    assert_eq!(descriptor.revision, 1);
    assert_eq!(descriptor.method_count, 10);
    assert_eq!(
        descriptor.method_bitmap[0] & (1_u64 << (authority::METHOD_BIND_PLATFORM_GENERATION - 1)),
        1 << 7
    );
}

#[cfg(feature = "alloc")]
#[test]
fn acpi_sci_event_and_ack_round_trip_the_complete_generation_lineage() {
    use naos_idl::acpi_fixed_hardware as fixed_hardware;

    let description = fixed_hardware::describe_response {
        firmware_version: TEST_FIRMWARE_VERSION,
        value: fixed_hardware::AcpiFixedHardwareInfo {
            sci_gsi: 9,
            pm1_event_bytes: 4,
            pm1_control_bytes: 2,
            pm2_control_bytes: 1,
            gpe0_bytes: 16,
            gpe1_bytes: 8,
            reset_supported: 1,
            sleep_supported: 1,
            sci_sequence: 2,
            sci_state: 0,
            gpe1_base: 64,
            aml_namespace_generation: 93,
            platform_topology_generation: 94,
            pci_fabric_generation: 95,
        },
    };
    let mut wire = [0_u8; 128];
    let written = fixed_hardware::encode_describe_response(&description, &mut wire).unwrap();
    let decoded = fixed_hardware::decode_describe_response(&wire[..written]).unwrap();
    assert_eq!(decoded.value.aml_namespace_generation, 93);
    assert_eq!(decoded.value.platform_topology_generation, 94);
    assert_eq!(decoded.value.pci_fabric_generation, 95);

    let event = fixed_hardware::drain_sci_response {
        firmware_version: TEST_FIRMWARE_VERSION,
        event: fixed_hardware::AcpiSciEvent {
            sequence: 77,
            flags: 1,
            pm1_event_status: 2,
            gpe0_status: 3,
            gpe1_status: 4,
            pm1_event_status_b: 5,
            aml_namespace_generation: 103,
            platform_topology_generation: 104,
            pci_fabric_generation: 105,
        },
    };
    let written = fixed_hardware::encode_drain_sci_response(&event, &mut wire).unwrap();
    let decoded = fixed_hardware::decode_drain_sci_response(&wire[..written]).unwrap();
    assert_eq!(decoded.firmware_version, TEST_FIRMWARE_VERSION);
    assert_eq!(decoded.event.aml_namespace_generation, 103);
    assert_eq!(decoded.event.platform_topology_generation, 104);
    assert_eq!(decoded.event.pci_fabric_generation, 105);

    let ack = fixed_hardware::ack_and_rearm_sci_response {
        firmware_version: TEST_FIRMWARE_VERSION,
        ack: fixed_hardware::AcpiSciAck {
            sequence: 77,
            state: 1,
            flags: 2,
            aml_namespace_generation: 103,
            platform_topology_generation: 104,
            pci_fabric_generation: 105,
        },
    };
    let written = fixed_hardware::encode_ack_and_rearm_sci_response(&ack, &mut wire).unwrap();
    let decoded = fixed_hardware::decode_ack_and_rearm_sci_response(&wire[..written]).unwrap();
    assert_eq!(decoded.firmware_version, TEST_FIRMWARE_VERSION);
    assert_eq!(decoded.ack.aml_namespace_generation, 103);
    assert_eq!(decoded.ack.platform_topology_generation, 104);
    assert_eq!(decoded.ack.pci_fabric_generation, 105);
    assert_eq!(fixed_hardware::protocol_descriptor().revision, 1);
}

#[cfg(feature = "alloc")]
#[test]
fn platform_power_control_round_trips_transition_outcomes() {
    use naos_idl::platform_power_control as power_control;

    let request = power_control::power_off_request {};
    let mut wire = [0_u8; 32];
    let written = power_control::encode_power_off_request(&request, &mut wire).unwrap();
    let _decoded_request = power_control::decode_power_off_request(&wire[..written]).unwrap();

    let response = power_control::power_off_response {
        result: power_control::PowerTransitionResult { outcome: 2 },
    };
    let written = power_control::encode_power_off_response(&response, &mut wire).unwrap();
    let decoded_response = power_control::decode_power_off_response(&wire[..written]).unwrap();
    assert_eq!(decoded_response.result.outcome, 2);

    let descriptor = power_control::protocol_descriptor();
    assert_eq!(descriptor.scope, 42);
    assert_eq!(descriptor.revision, 1);
    assert_eq!(descriptor.method_count, 2);
    assert_eq!(descriptor.method_bitmap[0] & 0b11, 0b11);
}

#[cfg(feature = "alloc")]
#[test]
fn acpi_pci_config_root_binding_carries_platform_generation_and_profile() {
    use naos_idl::acpi_resource_authority as authority;

    let request = authority::bind_pci_config_root_request {
        firmware_version: TEST_FIRMWARE_VERSION,
        request: authority::AcpiPciConfigRootRequest {
            namespace_generation: 13,
            root_id: 14,
            segment: 2,
            start_bus: 32,
            end_bus: 63,
            platform_topology_generation: 15,
            compatibility_profile_hash: 16,
        },
    };
    let mut wire = [0_u8; 128];
    let written = authority::encode_bind_pci_config_root_request(&request, &mut wire).unwrap();
    let decoded = authority::decode_bind_pci_config_root_request(&wire[..written]).unwrap();
    assert_eq!(decoded.request.platform_topology_generation, 15);
    assert_eq!(decoded.request.compatibility_profile_hash, 16);
}

#[cfg(feature = "alloc")]
#[test]
fn pci_dma_domain_schema_binds_mapping_to_a_memory_object_resource() {
    use naos_idl::pci_dma_domain as dma_domain;
    use naos_idl::pci_function;

    let request = dma_domain::map_page_request {
        iova: 0x4000,
        object_offset: 0x2000,
        direction: 0,
        backing: naos_idl::ResourceSlot::new(0).unwrap(),
    };
    let mut wire = [0_u8; 128];
    let written = dma_domain::encode_map_page_request(&request, &mut wire).unwrap();
    let decoded = dma_domain::decode_map_page_request(&wire[..written]).unwrap();
    assert_eq!(decoded.iova, request.iova);
    assert_eq!(decoded.object_offset, request.object_offset);
    assert_eq!(decoded.backing.index(), 0);

    let backing = naos_idl::transport::ResourceDescriptor {
        resource_id: 1,
        binding: naos_sys::BINDING_MEMORY_OBJECT,
        scope: 7,
        rights: naos_sys::RIGHT_TRANSFER | naos_sys::MEMORY_RIGHT_MAP | naos_sys::MEMORY_RIGHT_READ,
        view_length: 4096,
        ..naos_idl::transport::ResourceDescriptor::default()
    };
    assert!(
        dma_domain::validate_map_page_request_transport_resources(&request, &[backing]).is_ok()
    );

    let response = pci_function::create_dma_domain_response {
        domain: naos_idl::ResourceSlot::new(0).unwrap(),
        faults: naos_idl::ResourceSlot::new(1).unwrap(),
    };
    let mut response_wire = [0_u8; 32];
    let written =
        pci_function::encode_create_dma_domain_response(&response, &mut response_wire).unwrap();
    let decoded =
        pci_function::decode_create_dma_domain_response(&response_wire[..written]).unwrap();
    assert_eq!(decoded.domain.index(), 0);
    assert_eq!(decoded.faults.index(), 1);
    assert_eq!(pci_function::protocol_descriptor().revision, 1);
    assert_eq!(dma_domain::protocol_descriptor().scope, 37);

    let fault_source = naos_idl::pci_dma_fault_source::protocol_descriptor();
    assert_eq!(fault_source.scope, 41);
    assert_eq!(fault_source.method_count, 1);
    assert_eq!(fault_source.revision, 1);
}

#[cfg(feature = "alloc")]
#[test]
fn pci_dma_fault_source_schema_round_trips_bounded_records() {
    use naos_idl::pci_dma_fault_source as faults;

    let request = faults::take_request { limit: 16 };
    let mut request_wire = [0_u8; faults::TAKE_REQUEST_HEADER_BYTES];
    let written = faults::encode_take_request(&request, &mut request_wire).unwrap();
    assert_eq!(
        faults::decode_take_request(&request_wire[..written])
            .unwrap()
            .limit,
        16
    );

    let response = faults::take_response {
        sequence: 7,
        count: 2,
        dropped_records: 2,
        unit_overflow: 1,
        firmware_version: TEST_FIRMWARE_VERSION,
        aml_namespace_generation: 13,
        platform_topology_generation: 14,
        pci_fabric_generation: 15,
        records: alloc::vec![
            faults::PciDmaFaultRecord {
                iova: 0x5000,
                requester_id: 0x0018,
                reason: 0x05,
                direction: 1,
            },
            faults::PciDmaFaultRecord {
                iova: 0x6000,
                requester_id: 0x0018,
                reason: 0x02,
                direction: 2,
            }
        ],
    };
    let mut response_wire = [0_u8; faults::TAKE_RESPONSE_HEADER_BYTES + 24 + 32];
    let written = faults::encode_take_response(&response, &mut response_wire).unwrap();
    let decoded = faults::decode_take_response(&response_wire[..written]).unwrap();
    assert_eq!(decoded.sequence, 7);
    assert_eq!(decoded.count, 2);
    assert_eq!(decoded.dropped_records, 2);
    assert_eq!(decoded.unit_overflow, 1);
    assert_eq!(decoded.firmware_version, TEST_FIRMWARE_VERSION);
    assert_eq!(decoded.aml_namespace_generation, 13);
    assert_eq!(decoded.platform_topology_generation, 14);
    assert_eq!(decoded.pci_fabric_generation, 15);
    assert_eq!(decoded.records.len(), 2);
    assert_eq!(decoded.records[0].iova, 0x5000);
    assert_eq!(decoded.records[0].requester_id, 0x0018);
    assert_eq!(decoded.records[0].reason, 0x05);
    assert_eq!(decoded.records[0].direction, 1);
    assert_eq!(decoded.records[1].direction, 2);
}

#[test]
fn pci_interrupt_source_schema_is_owner_bound_and_rights_attenuated() {
    use naos_idl::interrupt_source;
    use naos_idl::pci_function;

    let request = pci_function::route_interrupt_request {};
    let mut request_wire = [0_u8; 8];
    let written =
        pci_function::encode_route_interrupt_request(&request, &mut request_wire).unwrap();
    assert!(pci_function::decode_route_interrupt_request(&request_wire[..written]).is_ok());

    let response = pci_function::route_interrupt_response {
        source: naos_idl::ResourceSlot::new(0).unwrap(),
    };
    let mut response_wire = [0_u8; 16];
    let written =
        pci_function::encode_route_interrupt_response(&response, &mut response_wire).unwrap();
    assert_eq!(
        pci_function::decode_route_interrupt_response(&response_wire[..written])
            .unwrap()
            .source
            .index(),
        0
    );
    let source = naos_idl::transport::ResourceDescriptor {
        resource_id: 7,
        binding: naos_sys::BINDING_NONE,
        scope: interrupt_source::PROTOCOL_SCOPE,
        rights: naos_sys::RIGHT_TRANSFER,
        ..naos_idl::transport::ResourceDescriptor::default()
    };
    assert!(
        pci_function::validate_route_interrupt_response_transport_resources(&response, &[source])
            .is_ok()
    );
    assert_eq!(pci_function::protocol_descriptor().revision, 1);
    assert_eq!(pci_function::protocol_descriptor().method_count, 7);

    let descriptor = interrupt_source::protocol_descriptor();
    assert_eq!(descriptor.scope, 38);
    assert_eq!(descriptor.revision, 1);
    assert_eq!(descriptor.method_count, 4);
    assert_ne!(descriptor.method_rights[0] & (1 << 29), 0);
    assert_ne!(descriptor.method_rights[1] & (1 << 30), 0);
    assert_ne!(descriptor.method_rights[2] & (1 << 31), 0);
    assert_ne!(descriptor.method_rights[3] & (1 << 32), 0);

    let wait_response = interrupt_source::wait_response {
        firmware_version: TEST_FIRMWARE_VERSION,
        event: interrupt_source::InterruptEvent {
            sequence: 11,
            flags: 3,
            aml_namespace_generation: 23,
            platform_topology_generation: 24,
            pci_fabric_generation: 25,
        },
    };
    let mut wait_wire = [0_u8; interrupt_source::WAIT_RESPONSE_HEADER_BYTES + 32];
    let wait_written =
        interrupt_source::encode_wait_response(&wait_response, &mut wait_wire).unwrap();
    let decoded_wait = interrupt_source::decode_wait_response(&wait_wire[..wait_written]).unwrap();
    assert_eq!(decoded_wait.firmware_version, TEST_FIRMWARE_VERSION);
    assert_eq!(decoded_wait.event.pci_fabric_generation, 25);

    let ack_response = interrupt_source::ack_response {
        firmware_version: decoded_wait.firmware_version,
        event: decoded_wait.event,
    };
    let mut ack_wire = [0_u8; interrupt_source::ACK_RESPONSE_HEADER_BYTES + 32];
    let ack_written = interrupt_source::encode_ack_response(&ack_response, &mut ack_wire).unwrap();
    let decoded_ack = interrupt_source::decode_ack_response(&ack_wire[..ack_written]).unwrap();
    assert_eq!(decoded_ack.event.platform_topology_generation, 24);
}

#[test]
fn pci_function_config_read_schema_round_trips_a_typed_scalar_request() {
    use naos_idl::pci_function;

    let request = pci_function::read_config_request {
        offset: 2,
        width: 2,
    };
    let mut request_wire = [0_u8; pci_function::READ_CONFIG_REQUEST_HEADER_BYTES];
    let written = pci_function::encode_read_config_request(&request, &mut request_wire).unwrap();
    let decoded = pci_function::decode_read_config_request(&request_wire[..written]).unwrap();
    assert_eq!(decoded.offset, 2);
    assert_eq!(decoded.width, 2);

    let response = pci_function::read_config_response { value: 0x11e8 };
    let mut response_wire = [0_u8; pci_function::READ_CONFIG_RESPONSE_HEADER_BYTES];
    let written = pci_function::encode_read_config_response(&response, &mut response_wire).unwrap();
    assert_eq!(
        pci_function::decode_read_config_response(&response_wire[..written])
            .unwrap()
            .value,
        0x11e8
    );
    assert_eq!(pci_function::protocol_descriptor().method_count, 7);
}

#[test]
fn pci_function_driver_grant_schema_binds_profile_to_a_process_and_private_directory() {
    use naos_idl::pci_function;

    let request = pci_function::grant_driver_request {
        profile: pci_function::PciDriverFunctionProfile {
            rights: 0x5,
            bar_mask: 0x3,
        },
        driver_process: naos_idl::ResourceSlot::new(0).unwrap(),
    };
    let mut request_wire = [0_u8; pci_function::GRANT_DRIVER_REQUEST_HEADER_BYTES];
    let written = pci_function::encode_grant_driver_request(&request, &mut request_wire).unwrap();
    let decoded = pci_function::decode_grant_driver_request(&request_wire[..written]).unwrap();
    assert_eq!(decoded.profile.rights, 0x5);
    assert_eq!(decoded.profile.bar_mask, 0x3);
    assert_eq!(decoded.driver_process.index(), 0);

    let response = pci_function::grant_driver_response {
        service_directory: naos_idl::ResourceSlot::new(0).unwrap(),
    };
    let mut response_wire = [0_u8; pci_function::GRANT_DRIVER_RESPONSE_HEADER_BYTES];
    let written =
        pci_function::encode_grant_driver_response(&response, &mut response_wire).unwrap();
    let decoded = pci_function::decode_grant_driver_response(&response_wire[..written]).unwrap();
    assert_eq!(decoded.service_directory.index(), 0);

    let descriptor = pci_function::protocol_descriptor();
    assert_eq!(descriptor.revision, 1);
    assert_eq!(descriptor.method_count, 7);
    assert_ne!(descriptor.method_rights[1] & (1 << 36), 0);
    assert_ne!(descriptor.method_rights[3] & (1 << 37), 0);
    assert_ne!(descriptor.method_rights[4] & (1 << 35), 0);
    assert_ne!(descriptor.method_rights[6] & (1 << 34), 0);
}

#[test]
fn pci_device_resource_attestation_schemas_have_distinct_revisions() {
    assert_eq!(
        naos_idl::pci_root_resource_attestation::protocol_descriptor().revision,
        1
    );
    assert_eq!(
        naos_idl::pci_device_resource_attestation::protocol_descriptor().revision,
        1
    );
}

#[test]
fn service_directory_create_view_schema_round_trips_a_single_uri_scope() {
    use naos_idl::service_directory;

    let request = service_directory::create_view_request {
        uri: "naos://system/pci/driver/0000-00-03-0",
    };
    let mut request_wire = [0_u8; 128];
    let written =
        service_directory::encode_create_view_request(&request, &mut request_wire).unwrap();
    let decoded = service_directory::decode_create_view_request(&request_wire[..written]).unwrap();
    assert_eq!(decoded.uri, request.uri);

    let response = service_directory::create_view_response {
        view: naos_idl::ResourceSlot::new(0).unwrap(),
    };
    let mut response_wire = [0_u8; 16];
    let written =
        service_directory::encode_create_view_response(&response, &mut response_wire).unwrap();
    let decoded =
        service_directory::decode_create_view_response(&response_wire[..written]).unwrap();
    assert_eq!(decoded.view.index(), 0);
    assert_eq!(service_directory::protocol_descriptor().revision, 1);
    assert_eq!(service_directory::protocol_descriptor().method_count, 8);
    assert_ne!(
        service_directory::protocol_descriptor().method_rights[7]
            & naos_sys::SERVICE_DIRECTORY_RIGHT_CREATE_VIEW,
        0
    );
}
