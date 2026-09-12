#![no_std]

use core::ffi::c_void;

pub type Status = u32;

pub const STATUS_OK: Status = 0;
pub const STATUS_INVALID_HANDLE: Status = 1;
pub const STATUS_WRONG_BINDING: Status = 2;
pub const STATUS_WRONG_SCOPE: Status = 3;
pub const STATUS_ACCESS_DENIED: Status = 4;
pub const STATUS_INVALID_ARGUMENT: Status = 5;
pub const STATUS_INVALID_MESSAGE: Status = 6;
pub const STATUS_BUFFER_TOO_SMALL: Status = 7;
pub const STATUS_WOULD_BLOCK: Status = 8;
pub const STATUS_WAIT_TIMED_OUT: Status = 9;
pub const STATUS_RESOURCE_EXHAUSTED: Status = 10;
pub const STATUS_FAULT: Status = 11;
pub const STATUS_OBJECT_REVOKED: Status = 12;
pub const STATUS_PEER_CLOSED: Status = 13;
pub const STATUS_ALREADY_CONSUMED: Status = 14;
pub const STATUS_NOT_SUPPORTED: Status = 15;
pub const STATUS_IO_ERROR: Status = 16;
pub const STATUS_WAIT_SET_INVALIDATED: Status = 17;

pub const SIGNAL_READABLE: u64 = 1 << 0;
pub const SIGNAL_WRITABLE: u64 = 1 << 1;
pub const SIGNAL_PEER_CLOSED: u64 = 1 << 2;
pub const SIGNAL_COMPLETED: u64 = 1 << 4;
pub const SIGNAL_CANCEL_REQUESTED: u64 = 1 << 5;

pub const EXECUTION_NONE: u32 = 0;
pub const EXECUTION_NOT_DELIVERED: u32 = 1;
pub const EXECUTION_OUTCOME_UNKNOWN: u32 = 2;

pub const OUTCOME_REASON_NONE: u32 = 0;
pub const OUTCOME_REASON_PEER_CLOSED: u32 = 1;
pub const OUTCOME_REASON_OBJECT_REVOKED: u32 = 2;
pub const OUTCOME_REASON_OPERATION_DEADLINE: u32 = 3;
pub const OUTCOME_REASON_CANCEL_REQUESTED: u32 = 4;
pub const OUTCOME_REASON_REQUEST_DISCARDED: u32 = 5;
pub const OUTCOME_REASON_RESPONDER_ABANDONED: u32 = 6;
pub const OUTCOME_REASON_BROKER_FAILURE: u32 = 7;
pub const OUTCOME_REASON_PROTOCOL_VIOLATION: u32 = 8;
pub const OUTCOME_REASON_UNSUPPORTED: u32 = 9;

pub type AllocateFn = unsafe extern "C" fn(*mut c_void, usize, usize) -> *mut c_void;
pub type DeallocateFn = unsafe extern "C" fn(*mut c_void, *mut c_void, usize, usize);

#[repr(C)]
#[derive(Clone, Copy)]
pub struct Allocator {
    pub context: *mut c_void,
    pub allocate: Option<AllocateFn>,
    pub deallocate: Option<DeallocateFn>,
}

pub type LockFn = unsafe extern "C" fn(*mut c_void);

#[repr(C)]
#[derive(Clone, Copy)]
pub struct Lock {
    pub context: *mut c_void,
    pub acquire: Option<LockFn>,
    pub release: Option<LockFn>,
}

pub type NotifyFn = unsafe extern "C" fn(*mut c_void);

#[repr(C)]
#[derive(Clone, Copy)]
pub struct WaitNotifier {
    pub context: *mut c_void,
    pub notify: Option<NotifyFn>,
}

pub type ClockNowFn = unsafe extern "C" fn(*mut c_void) -> u64;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct Clock {
    pub context: *mut c_void,
    pub now: Option<ClockNowFn>,
}

pub type ValidateWaitableFn = unsafe extern "C" fn(*mut c_void, u64) -> Status;
pub type BeginTransferFn =
    unsafe extern "C" fn(*mut c_void, *const u64, usize, *mut *mut c_void) -> Status;
pub type RestoreTransferFn = unsafe extern "C" fn(*mut c_void, *mut c_void) -> Status;
pub type CommitTransferFn = unsafe extern "C" fn(*mut c_void, *mut c_void);

#[repr(C)]
#[derive(Clone, Copy)]
pub struct HandleTable {
    pub context: *mut c_void,
    pub validate_waitable: Option<ValidateWaitableFn>,
    pub begin_transfer: Option<BeginTransferFn>,
    pub restore_transfer: Option<RestoreTransferFn>,
    pub commit_transfer: Option<CommitTransferFn>,
}

pub type ResourceReleaseFn = unsafe extern "C" fn(*mut c_void, *mut c_void);
pub type ResourceVisitorFn = unsafe extern "C" fn(*mut c_void, u64, *mut c_void, *mut c_void);
pub type MessageVisitorFn = unsafe extern "C" fn(*mut c_void, *mut Message);

#[repr(C)]
#[derive(Clone, Copy)]
pub struct Resource {
    pub context: *mut c_void,
    pub value: *mut c_void,
    pub release: Option<ResourceReleaseFn>,
}

#[repr(C)]
pub struct Domain {
    _private: [u8; 0],
}

#[repr(C)]
pub struct Channel {
    _private: [u8; 0],
}

#[repr(C)]
pub struct Message {
    _private: [u8; 0],
}

#[repr(C)]
pub struct Invocation {
    _private: [u8; 0],
}

#[repr(C)]
pub struct DomainConfig {
    pub memory: *const Allocator,
    pub synchronization: *const Lock,
    pub max_messages: u64,
    pub max_bytes: u64,
    pub max_resources: u64,
}

#[repr(C)]
pub struct ChannelConfig {
    pub owner_domain: *mut Domain,
    pub control_memory: *const Allocator,
    pub payload_memory: *const Allocator,
    pub synchronization: *const Lock,
    pub notifier: *const WaitNotifier,
    pub clock: *const Clock,
    pub handles: *const HandleTable,
    pub max_messages: u64,
    pub max_bytes: u64,
    pub max_resources: u64,
}

pub type InvocationRemoveQueuedFn = unsafe extern "C" fn(*mut c_void) -> i32;
pub type InvocationWakeExecutionFn = unsafe extern "C" fn(*mut c_void);

#[repr(C)]
#[derive(Clone, Copy)]
pub struct InvocationCallbacks {
    pub context: *mut c_void,
    pub remove_queued: Option<InvocationRemoveQueuedFn>,
    pub wake_execution: Option<InvocationWakeExecutionFn>,
}

#[repr(C)]
pub struct InvocationConfig {
    pub owner_domain: *mut Domain,
    pub control_memory: *const Allocator,
    pub payload_memory: *const Allocator,
    pub synchronization: *const Lock,
    pub notifier: *const WaitNotifier,
    pub clock: *const Clock,
    pub callbacks: *const InvocationCallbacks,
    pub method_id: u64,
    pub operation_deadline: u64,
    pub max_response_bytes: u64,
    pub max_response_resources: u64,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct InvocationResultInfo {
    pub method_id: u64,
    pub actual_bytes: u64,
    pub actual_resources: u64,
    pub required_bytes: u64,
    pub required_resources: u64,
    pub execution_outcome: u32,
    pub outcome_reason: u32,
    pub protocol_error: i64,
}

unsafe extern "C" {
    pub fn naos_ipc_domain_create(config: *const DomainConfig) -> *mut Domain;
    pub fn naos_ipc_domain_destroy(domain: *mut Domain);

    pub fn naos_ipc_channel_create(config: *const ChannelConfig) -> *mut Channel;
    pub fn naos_ipc_channel_destroy(channel: *mut Channel);
    pub fn naos_ipc_channel_valid(channel: *const Channel) -> i32;
    pub fn naos_ipc_channel_max_messages(channel: *const Channel) -> u64;
    pub fn naos_ipc_channel_queued_messages(channel: *const Channel, side: u8) -> u64;
    pub fn naos_ipc_channel_signals(channel: *const Channel, side: u8) -> u64;
    pub fn naos_ipc_channel_can_reap(channel: *const Channel) -> i32;

    pub fn naos_ipc_channel_side_reference_acquired(channel: *mut Channel, side: u8);
    pub fn naos_ipc_channel_side_reference_released(channel: *mut Channel, side: u8);
    pub fn naos_ipc_channel_begin_operation(channel: *mut Channel);
    pub fn naos_ipc_channel_end_operation(channel: *mut Channel);

    pub fn naos_ipc_message_create(
        channel: *mut Channel,
        byte_count: u64,
        resource_capacity: u64,
    ) -> *mut Message;
    pub fn naos_ipc_message_destroy(message: *mut Message);
    pub fn naos_ipc_message_valid(message: *const Message) -> i32;
    pub fn naos_ipc_message_bytes(message: *mut Message) -> *mut u8;
    pub fn naos_ipc_message_const_bytes(message: *const Message) -> *const u8;
    pub fn naos_ipc_message_byte_count(message: *const Message) -> u64;
    pub fn naos_ipc_message_resource_count(message: *const Message) -> u64;
    pub fn naos_ipc_message_resource_capacity(message: *const Message) -> u64;
    pub fn naos_ipc_message_set_user_context(message: *mut Message, context: *mut c_void);
    pub fn naos_ipc_message_user_context(message: *const Message) -> *mut c_void;
    pub fn naos_ipc_message_visit_resources(
        message: *const Message,
        visitor: Option<ResourceVisitorFn>,
        context: *mut c_void,
    );
    pub fn naos_ipc_message_take_resource(
        message: *mut Message,
        index: u64,
        resource: *mut Resource,
    ) -> Status;
    pub fn naos_ipc_message_restore_resource(
        message: *mut Message,
        index: u64,
        resource: *mut Resource,
    ) -> Status;

    pub fn naos_ipc_resource_reset(resource: *mut Resource);
    pub fn naos_ipc_resource_valid(resource: *const Resource) -> i32;

    pub fn naos_ipc_channel_enqueue(
        channel: *mut Channel,
        sender: u8,
        message: *mut Message,
        resources: *mut Resource,
        resource_count: usize,
        queue_message_limit: u64,
    ) -> Status;
    pub fn naos_ipc_channel_claim_receive(
        channel: *mut Channel,
        side: u8,
        message: *mut *mut Message,
    ) -> Status;
    pub fn naos_ipc_channel_cancel_receive(
        channel: *mut Channel,
        side: u8,
        message: *mut Message,
    ) -> i32;
    pub fn naos_ipc_channel_commit_receive(
        channel: *mut Channel,
        side: u8,
        message: *mut Message,
    ) -> i32;
    pub fn naos_ipc_channel_discard(
        channel: *mut Channel,
        side: u8,
        message: *mut *mut Message,
    ) -> i32;
    pub fn naos_ipc_channel_visit_queued_messages(
        channel: *const Channel,
        visitor: Option<MessageVisitorFn>,
        context: *mut c_void,
    );

    pub fn naos_ipc_validate_wait_set(
        handles: *const HandleTable,
        values: *const u64,
        count: usize,
    ) -> Status;
    pub fn naos_ipc_clock_now(clock: *const Clock) -> u64;

    pub fn naos_ipc_invocation_create(config: *const InvocationConfig) -> *mut Invocation;
    pub fn naos_ipc_invocation_destroy(invocation: *mut Invocation);
    pub fn naos_ipc_invocation_valid(invocation: *const Invocation) -> i32;
    pub fn naos_ipc_invocation_method_id(invocation: *const Invocation) -> u64;
    pub fn naos_ipc_invocation_operation_deadline(invocation: *const Invocation) -> u64;
    pub fn naos_ipc_invocation_signals(invocation: *const Invocation) -> u64;
    pub fn naos_ipc_invocation_begin_receive(invocation: *mut Invocation) -> i32;
    pub fn naos_ipc_invocation_rollback_receive(invocation: *mut Invocation);
    pub fn naos_ipc_invocation_finish_dispatch(invocation: *mut Invocation) -> i32;
    pub fn naos_ipc_invocation_mark_dispatched(invocation: *mut Invocation);
    pub fn naos_ipc_invocation_cancellation_requested(invocation: *const Invocation) -> i32;
    pub fn naos_ipc_invocation_execution_interrupted(invocation: *const Invocation) -> i32;
    pub fn naos_ipc_invocation_cancel(invocation: *mut Invocation) -> i32;
    pub fn naos_ipc_invocation_expire_if_due(invocation: *mut Invocation) -> i32;
    pub fn naos_ipc_invocation_close_client(invocation: *mut Invocation);
    pub fn naos_ipc_invocation_abandon_responder(invocation: *mut Invocation);
    pub fn naos_ipc_invocation_consume_responder(invocation: *mut Invocation) -> i32;
    pub fn naos_ipc_invocation_reserve_result_budget(invocation: *mut Invocation) -> i32;
    pub fn naos_ipc_invocation_response_within_limits(
        invocation: *const Invocation,
        bytes: u64,
        resources: u64,
    ) -> i32;
    pub fn naos_ipc_invocation_complete_reply(
        invocation: *mut Invocation,
        bytes: *const u8,
        byte_count: u64,
        resources: *mut Resource,
        resource_count: usize,
        protocol_error: i64,
    ) -> i32;
    pub fn naos_ipc_invocation_failure_valid(
        execution_outcome: u32,
        outcome_reason: u32,
        protocol_error: i64,
    ) -> i32;
    pub fn naos_ipc_invocation_complete_failure(
        invocation: *mut Invocation,
        execution_outcome: u32,
        outcome_reason: u32,
        protocol_error: i64,
    ) -> i32;
    pub fn naos_ipc_invocation_complete_not_delivered(
        invocation: *mut Invocation,
        outcome_reason: u32,
    ) -> i32;
    pub fn naos_ipc_invocation_claim_result(
        invocation: *mut Invocation,
        byte_capacity: u64,
        resource_capacity: u64,
        info: *mut InvocationResultInfo,
    ) -> Status;
    pub fn naos_ipc_invocation_take_result_bytes(
        invocation: *mut Invocation,
        bytes: *mut *mut u8,
        byte_count: *mut u64,
    ) -> Status;
    pub fn naos_ipc_invocation_take_result_resource(
        invocation: *mut Invocation,
        index: u64,
        resource: *mut Resource,
    ) -> Status;
    pub fn naos_ipc_invocation_restore_result(
        invocation: *mut Invocation,
        bytes: *mut u8,
        byte_count: u64,
        resources: *mut Resource,
        resource_count: usize,
    ) -> Status;
    pub fn naos_ipc_invocation_commit_result(invocation: *mut Invocation) -> Status;
}
