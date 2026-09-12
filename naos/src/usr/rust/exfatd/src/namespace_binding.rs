//! Private NamespaceBinding/MutationTicket client used by the external worker.
//!
//! The internal protocols are deliberately not part of the application IDL
//! set.  This small adapter mirrors the frozen wire shape used by vfsd and
//! keeps the worker's transaction boundary explicit: reserve before FAT
//! metadata changes, then release with commit or abort.

extern crate alloc;

use alloc::vec::Vec;

use crate::mount_control::NodeKey;
use naos_idl::{
    CallError, CodecError, Encoder, Invocation, ProtocolClientEndpoint, RawInvocationResult,
    ResourceTable,
};
use naos_sys as sys;

pub const METHOD_BEGIN_MUTATION: u64 = 5;
pub const METHOD_COMMIT: u64 = 1;
pub const METHOD_ABORT: u64 = 2;
pub const METHOD_STATUS: u64 = 3;

pub const OP_RENAME: u32 = 0;
pub const OP_LINK: u32 = 1;
pub const OP_SYMLINK: u32 = 2;
pub const OP_CREATE: u32 = 3;
pub const OP_MKDIR: u32 = 4;
pub const OP_UNLINK: u32 = 5;
pub const OP_RMDIR: u32 = 6;

const BEGIN_MUTATION_HEADER: usize = 84;
const STATUS_COMMITTED: u32 = 2;

fn encode_begin_mutation(
    operation: u32,
    old_parent: NodeKey,
    old_target: NodeKey,
    new_parent: NodeKey,
    new_target: NodeKey,
    old_name: &[u8],
    new_name: &[u8],
) -> Result<Vec<u8>, CodecError> {
    if old_name.len() > 255 || new_name.len() > 255 {
        return Err(CodecError::BoundExceeded);
    }
    let dynamic = old_name
        .len()
        .checked_add(new_name.len())
        .ok_or(CodecError::Overflow)?;
    let size = BEGIN_MUTATION_HEADER
        .checked_add(dynamic)
        .ok_or(CodecError::Overflow)?;
    let mut wire = vec![0_u8; size];
    let mut encoder = Encoder::new(&mut wire);
    encoder.put_u32(operation)?;
    encoder.put_u64(old_parent.node_id)?;
    encoder.put_u64(old_parent.generation)?;
    encoder.put_u64(old_target.node_id)?;
    encoder.put_u64(old_target.generation)?;
    encoder.put_u64(new_parent.node_id)?;
    encoder.put_u64(new_parent.generation)?;
    encoder.put_u64(new_target.node_id)?;
    encoder.put_u64(new_target.generation)?;
    // Both lengths are in the fixed header.  This is the private vfsd wire
    // contract, even though the source IDL interleaves each length with its
    // bytes field.
    encoder.put_u64(old_name.len() as u64)?;
    encoder.put_u64(new_name.len() as u64)?;
    encoder.put_bounded_bytes(old_name, 255)?;
    encoder.put_bounded_bytes(new_name, 255)?;
    if encoder.written() != size {
        return Err(CodecError::InvalidMessage);
    }
    Ok(wire)
}

fn wait(invocation: &Invocation) -> Result<(), CallError> {
    if servicekit::wait_for_completion(invocation.get(), u64::MAX) {
        Ok(())
    } else {
        Err(CallError::Status(sys::STATUS_WAIT_TIMED_OUT))
    }
}

fn completed_result(
    invocation: &mut Invocation,
    expected_method: u64,
    wire: &mut [u8],
) -> Result<RawInvocationResult, CallError> {
    let result = naos_idl::take_invocation_result(invocation, wire)?;
    if result.method_id != expected_method {
        return Err(CallError::Codec(CodecError::InvalidMessage));
    }
    if result.execution != 0 || result.protocol_error != 0 {
        return Err(CallError::Outcome {
            execution: result.execution,
            reason: result.reason,
            protocol_error: result.protocol_error,
        });
    }
    Ok(result)
}

/// Reserve all affected namespace components and adopt the returned ticket.
pub fn begin_mutation(
    namespace: &ProtocolClientEndpoint,
    operation: u32,
    old_parent: NodeKey,
    old_target: NodeKey,
    new_parent: NodeKey,
    new_target: NodeKey,
    old_name: &[u8],
    new_name: &[u8],
) -> Result<ProtocolClientEndpoint, CallError> {
    let request = encode_begin_mutation(
        operation, old_parent, old_target, new_parent, new_target, old_name, new_name,
    )
    .map_err(CallError::Codec)?;
    let mut invocation = naos_idl::submit_invocation(
        namespace,
        METHOD_BEGIN_MUTATION,
        &request,
        ResourceTable::new(),
        0,
    )?;
    wait(&invocation)?;
    let mut response_wire = [0_u8; 8];
    let mut result = completed_result(&mut invocation, METHOD_BEGIN_MUTATION, &mut response_wire)?;
    if result.bytes != 4 || result.resources.len() != 1 {
        return Err(CallError::Codec(CodecError::InvalidMessage));
    }
    let slot = u32::from_le_bytes(
        response_wire[..4]
            .try_into()
            .map_err(|_| CallError::Codec(CodecError::InvalidMessage))?,
    );
    let slot =
        naos_idl::ResourceSlot::new(slot).ok_or(CallError::Codec(CodecError::InvalidResource))?;
    if naos_idl::validate_received_resource(
        &result.resources,
        slot,
        sys::BINDING_CLIENT_END,
        22,
        sys::RIGHT_TRANSFER,
        (1 << 26) | 1,
    )
    .is_err()
    {
        return Err(CallError::Codec(CodecError::InvalidResource));
    }
    let ticket = result
        .resources
        .take(slot)
        .ok_or(CallError::Codec(CodecError::InvalidResource))?;
    Ok(unsafe { ProtocolClientEndpoint::from_raw(ticket.into_raw()) })
}

fn invoke_empty(ticket: &ProtocolClientEndpoint, method: u64) -> Result<(), CallError> {
    let mut invocation = naos_idl::submit_invocation(ticket, method, &[], ResourceTable::new(), 0)?;
    wait(&invocation)?;
    let mut response_wire = [];
    let result = completed_result(&mut invocation, method, &mut response_wire)?;
    if result.bytes != 0 || !result.resources.is_empty() {
        return Err(CallError::Codec(CodecError::InvalidMessage));
    }
    Ok(())
}

pub fn abort(ticket: &ProtocolClientEndpoint) -> Result<(), CallError> {
    invoke_empty(ticket, METHOD_ABORT)
}

fn status(ticket: &ProtocolClientEndpoint) -> Result<u32, CallError> {
    let mut invocation =
        naos_idl::submit_invocation(ticket, METHOD_STATUS, &[], ResourceTable::new(), 0)?;
    wait(&invocation)?;
    let mut response_wire = [0_u8; 8];
    let result = completed_result(&mut invocation, METHOD_STATUS, &mut response_wire)?;
    if result.bytes != 4 || !result.resources.is_empty() {
        return Err(CallError::Codec(CodecError::InvalidMessage));
    }
    Ok(u32::from_le_bytes(response_wire[..4].try_into().map_err(
        |_| CallError::Codec(CodecError::InvalidMessage),
    )?))
}

/// Commit the local metadata change.  If the reply itself is unavailable,
/// reconcile through the idempotent status method instead of resending the
/// non-idempotent commit request.
pub fn commit_or_reconcile(ticket: &ProtocolClientEndpoint) -> Result<(), CallError> {
    match invoke_empty(ticket, METHOD_COMMIT) {
        Ok(()) => Ok(()),
        Err(commit_error) => match status(ticket) {
            Ok(STATUS_COMMITTED) => Ok(()),
            Ok(_) | Err(_) => Err(commit_error),
        },
    }
}
