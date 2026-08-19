extern crate alloc;

#[cfg(feature = "alloc")]
use naos_idl::CodecError;
use naos_idl::{Decoder, Encoder, OwnedHandle, ResourceError, ResourceTable, echo};

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
    let mut table = ResourceTable::new();
    let move_slot = table.push_move(moved).unwrap();
    let duplicate_slot = table.push_duplicate(&borrowed).unwrap();
    assert_eq!(move_slot.index(), 0);
    assert_eq!(duplicate_slot.index(), 1);
    assert_eq!(table.as_slice()[0].operation, naos_sys::RESOURCE_MOVE);
    assert_eq!(table.as_slice()[1].operation, naos_sys::RESOURCE_DUPLICATE);
    table.commit_move();
    core::mem::forget(borrowed);

    assert_eq!(
        ResourceTable::new().push_move(OwnedHandle::invalid()),
        Err(ResourceError::InvalidHandle)
    );
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
