//! Private NamespaceBinding/MutationTicket client used by the external worker.
//!
//! The generated bindings own the wire format and method routing. This adapter
//! keeps the worker's transaction boundary explicit: reserve before FAT
//! metadata changes, then release with commit or abort.

extern crate alloc;

use crate::mount_control::NodeKey;
use naos_idl::{
    CallError, Invocation, ProtocolClientEndpoint, ResourceTransferBuilder, mutation_ticket,
    namespace_binding as namespace_wire,
};
use naos_sys as sys;

pub const OP_RENAME: u32 = 0;
pub const OP_LINK: u32 = 1;
pub const OP_SYMLINK: u32 = 2;
pub const OP_CREATE: u32 = 3;
pub const OP_MKDIR: u32 = 4;
pub const OP_UNLINK: u32 = 5;
pub const OP_RMDIR: u32 = 6;

const STATUS_COMMITTED: u32 = 2;

fn wait(invocation: &Invocation) -> Result<(), CallError> {
    if servicekit::wait_for_completion(invocation.get(), u64::MAX) {
        Ok(())
    } else {
        Err(CallError::Status(sys::STATUS_WAIT_TIMED_OUT))
    }
}

fn wire_node(value: NodeKey) -> namespace_wire::NodeKey {
    namespace_wire::NodeKey {
        node_id: value.node_id,
        generation: value.generation,
    }
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
    let request = namespace_wire::begin_mutation_request {
        operation,
        old_parent: wire_node(old_parent),
        old_target: wire_node(old_target),
        new_parent: wire_node(new_parent),
        new_target: wire_node(new_target),
        old_name_size: old_name.len() as u64,
        old_name,
        new_name_size: new_name.len() as u64,
        new_name,
    };
    let mut request_wire = [0_u8; namespace_wire::BEGIN_MUTATION_REQUEST_HEADER_BYTES + 2 * 255];
    let mut invocation = namespace_wire::submit_begin_mutation(
        namespace,
        &request,
        ResourceTransferBuilder::new(),
        &mut request_wire,
        0,
    )?;
    wait(&invocation)?;

    let mut response_wire = [0_u8; namespace_wire::BEGIN_MUTATION_RESPONSE_HEADER_BYTES];
    let (response, mut resources) =
        namespace_wire::take_begin_mutation(&mut invocation, &mut response_wire)?;
    let ticket = resources
        .take(response.ticket)
        .ok_or(CallError::Codec(naos_idl::CodecError::InvalidResource))?;
    Ok(unsafe { ProtocolClientEndpoint::from_raw(ticket.into_raw()) })
}

pub fn abort(ticket: &ProtocolClientEndpoint) -> Result<(), CallError> {
    let mut request_wire = [];
    let mut invocation = mutation_ticket::submit_abort(
        ticket,
        &mutation_ticket::abort_request {},
        ResourceTransferBuilder::new(),
        &mut request_wire,
        0,
    )?;
    wait(&invocation)?;
    let mut response_wire = [];
    mutation_ticket::take_abort(&mut invocation, &mut response_wire).map(|_| ())
}

fn commit(ticket: &ProtocolClientEndpoint) -> Result<(), CallError> {
    let mut request_wire = [];
    let mut invocation = mutation_ticket::submit_commit(
        ticket,
        &mutation_ticket::commit_request {},
        ResourceTransferBuilder::new(),
        &mut request_wire,
        0,
    )?;
    wait(&invocation)?;
    let mut response_wire = [];
    mutation_ticket::take_commit(&mut invocation, &mut response_wire).map(|_| ())
}

fn status(ticket: &ProtocolClientEndpoint) -> Result<u32, CallError> {
    let mut request_wire = [];
    let mut invocation = mutation_ticket::submit_status(
        ticket,
        &mutation_ticket::status_request {},
        ResourceTransferBuilder::new(),
        &mut request_wire,
        0,
    )?;
    wait(&invocation)?;
    let mut response_wire = [0_u8; mutation_ticket::STATUS_RESPONSE_HEADER_BYTES];
    mutation_ticket::take_status(&mut invocation, &mut response_wire).map(|response| response.state)
}

/// Commit the local metadata change. If the reply is unavailable, reconcile
/// through the idempotent status method instead of resending the commit.
pub fn commit_or_reconcile(ticket: &ProtocolClientEndpoint) -> Result<(), CallError> {
    match commit(ticket) {
        Ok(()) => Ok(()),
        Err(commit_error) => match status(ticket) {
            Ok(STATUS_COMMITTED) => Ok(()),
            Ok(_) | Err(_) => Err(commit_error),
        },
    }
}
