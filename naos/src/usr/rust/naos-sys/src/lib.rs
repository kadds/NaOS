#![no_std]

pub type Handle = u64;
pub type Status = u32;

pub const HANDLE_INVALID: Handle = 0;

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
pub const RESOURCE_MOVE: u32 = 1;
pub const RESOURCE_DUPLICATE: u32 = 2;
pub const BINDING_NONE: u32 = 0;
pub const BINDING_RAW_CHANNEL_END: u32 = 1;
pub const BINDING_CLIENT_END: u32 = 2;
pub const BINDING_SERVER_END: u32 = 3;
pub const BINDING_KERNEL_VIEW: u32 = 4;
pub const BINDING_INVOCATION: u32 = 5;
pub const BINDING_RESPONDER: u32 = 6;
pub const BINDING_MEMORY_OBJECT: u32 = 7;
pub const BINDING_SHARED_RING: u32 = 8;
pub const RIGHT_DUPLICATE: u64 = 1 << 0;
pub const RIGHT_TRANSFER: u64 = 1 << 1;
pub const RIGHT_WAIT: u64 = 1 << 2;
pub const RIGHT_INSPECT: u64 = 1 << 3;
pub const SIGNAL_READABLE: u64 = 1 << 0;
pub const SIGNAL_WRITABLE: u64 = 1 << 1;
pub const SIGNAL_PEER_CLOSED: u64 = 1 << 2;
pub const SIGNAL_OBJECT_REVOKED: u64 = 1 << 3;
pub const SIGNAL_COMPLETED: u64 = 1 << 4;
pub const SIGNAL_CANCEL_REQUESTED: u64 = 1 << 5;
pub const PROTOCOL_RIGHT_INVOKE: u64 = 1 << 0;
pub const TERMINAL_RIGHT_READ: u64 = 1 << 8;
pub const TERMINAL_RIGHT_WRITE: u64 = 1 << 9;
pub const TERMINAL_RIGHT_CONTROL: u64 = 1 << 10;
pub const TERMINAL_RIGHT_WATCH: u64 = 1 << 11;
pub const TERMINAL_RIGHT_ADMIN: u64 = 1 << 12;

pub const MEMORY_MAP_READ: u32 = 1 << 0;
pub const MEMORY_MAP_WRITE: u32 = 1 << 1;
pub const MEMORY_MAP_EXEC: u32 = 1 << 2;
pub const MEMORY_MAP_SHARED: u32 = 1 << 3;
pub const MEMORY_MAP_MAX_BYTES: u64 = 1 << 30;
pub const TLS_ABI_VERSION: u32 = 1;
pub const TLS_MAX_SIZE: usize = 1 << 20;
pub const TLS_MAX_ALIGN: usize = 1 << 20;

pub const MAX_BOOTSTRAP_CAPABILITIES: usize = 8;
pub const BOOTSTRAP_FLAG_REBIND_CONSOLE: u32 = 1;

pub const SYSCALL_LOG: u64 = 1;
pub const SYSCALL_EXIT: u64 = 4;
pub const SYSCALL_HANDLE_CLOSE: u64 = 17;
pub const SYSCALL_CHANNEL_CREATE: u64 = 18;
pub const SYSCALL_CHANNEL_SEND: u64 = 19;
pub const SYSCALL_CHANNEL_RECEIVE: u64 = 20;
pub const SYSCALL_CHANNEL_DISCARD: u64 = 21;
pub const SYSCALL_HANDLE_WAIT_MANY: u64 = 22;
pub const SYSCALL_HANDLE_DUPLICATE: u64 = 23;
pub const SYSCALL_HANDLE_RESTRICT: u64 = 24;
pub const SYSCALL_HANDLE_GET_INFO: u64 = 25;
pub const SYSCALL_PROTOCOL_DESCRIPTOR_CREATE: u64 = 26;
pub const SYSCALL_PROTOCOL_ENDPOINT_CREATE: u64 = 27;
pub const SYSCALL_INVOKE_SUBMIT: u64 = 28;
pub const SYSCALL_INVOKE_ONEWAY: u64 = 29;
pub const SYSCALL_INVOCATION_CANCEL: u64 = 30;
pub const SYSCALL_INVOCATION_TAKE_RESULT: u64 = 31;
pub const SYSCALL_RESPONDER_REPLY: u64 = 32;
pub const SYSCALL_RESPONDER_FAIL: u64 = 33;
pub const SYSCALL_BOOTSTRAP: u64 = 34;
pub const SYSCALL_MEMORY_MAP: u64 = 36;
pub const SYSCALL_MEMORY_UNMAP: u64 = 37;
pub const SYSCALL_FUTEX: u64 = 3;
pub const SYSCALL_EXIT_THREAD: u64 = 5;
pub const SYSCALL_SET_TCB: u64 = 11;
pub const SYSCALL_CLONE: u64 = 13;

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct Uuid {
    pub bytes: [u8; 16],
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct BootstrapCapability {
    pub kind: u32,
    pub handle: Handle,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct BootstrapFrame {
    pub struct_size: u32,
    pub flags: u32,
    pub root_directory: Handle,
    pub current_directory: Handle,
    pub service_directory: Handle,
    pub stdin_stream: Handle,
    pub stdout_stream: Handle,
    pub stderr_stream: Handle,
    pub capability_count: u32,
    pub reserved0: u32,
    pub capabilities: [BootstrapCapability; MAX_BOOTSTRAP_CAPABILITIES],
    pub reserved1: u64,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct ChannelOptions {
    pub struct_size: u32,
    pub flags: u32,
    pub max_messages: u64,
    pub max_bytes: u64,
    pub max_resources: u64,
    pub reserved0: u64,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct ChannelSendFrame {
    pub struct_size: u32,
    pub flags: u32,
    pub bytes: u64,
    pub byte_count: u64,
    pub resources: u64,
    pub resource_count: u64,
    pub reserved0: u64,
    pub reserved1: u64,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct ChannelReceiveFrame {
    pub struct_size: u32,
    pub flags: u32,
    pub method_id: u64,
    pub bytes: u64,
    pub byte_capacity: u64,
    pub resources: u64,
    pub resource_capacity: u64,
    pub responder: Handle,
    pub actual_bytes: u64,
    pub actual_resources: u64,
    pub required_bytes: u64,
    pub required_resources: u64,
    pub caller_pid: u64,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct WaitItem {
    pub handle: Handle,
    pub signals: u64,
    pub observed: u64,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct HandleInfo {
    pub struct_size: u32,
    pub binding: u32,
    pub scope: u64,
    pub revision: u64,
    pub features: u64,
    pub meta_rights: u64,
    pub protocol_rights: u64,
    pub signals: u64,
    pub generation: u64,
    pub object_state: u64,
    pub protocol_uuid: Uuid,
    pub reserved0: u64,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct HandleRestriction {
    pub struct_size: u32,
    pub flags: u32,
    pub scope: u64,
    pub revision: u64,
    pub features: u64,
    pub meta_rights: u64,
    pub protocol_rights: u64,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct ResourceDisposition {
    pub handle: Handle,
    pub operation: u32,
    pub flags: u32,
    pub rights: u64,
    pub scope: u64,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct ProtocolDescriptor {
    pub struct_size: u32,
    pub flags: u32,
    pub uuid: Uuid,
    pub scope: u64,
    pub revision: u64,
    pub features: u64,
    pub protocol_rights: u64,
    pub method_count: u64,
    pub max_request_bytes: u64,
    pub max_response_bytes: u64,
    pub max_resources: u64,
    pub reserved0: u64,
    pub reserved1: u64,
    pub method_bitmap: [u64; 4],
    pub oneway_bitmap: [u64; 4],
    pub method_rights: [u64; 256],
}

impl Default for ProtocolDescriptor {
    fn default() -> Self {
        Self {
            struct_size: 0,
            flags: 0,
            uuid: Uuid::default(),
            scope: 0,
            revision: 0,
            features: 0,
            protocol_rights: 0,
            method_count: 0,
            max_request_bytes: 0,
            max_response_bytes: 0,
            max_resources: 0,
            reserved0: 0,
            reserved1: 0,
            method_bitmap: [0; 4],
            oneway_bitmap: [0; 4],
            method_rights: [0; 256],
        }
    }
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct ProtocolEndpointOptions {
    pub struct_size: u32,
    pub flags: u32,
    pub client_meta_rights: u64,
    pub server_meta_rights: u64,
    pub max_messages: u64,
    pub max_bytes: u64,
    pub max_resources: u64,
    pub reserved0: u64,
    pub client_protocol_rights: u64,
    pub server_protocol_rights: u64,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct SubmitFrame {
    pub struct_size: u32,
    pub flags: u32,
    pub method_id: u64,
    pub request: u64,
    pub request_bytes: u64,
    pub resources: u64,
    pub resource_count: u64,
    pub operation_budget: u64,
    pub reserved0: u64,
    pub reserved1: u64,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct ResultFrame {
    pub struct_size: u32,
    pub flags: u32,
    pub method_id: u64,
    pub bytes: u64,
    pub byte_capacity: u64,
    pub resources: u64,
    pub resource_capacity: u64,
    pub actual_bytes: u64,
    pub actual_resources: u64,
    pub required_bytes: u64,
    pub required_resources: u64,
    pub execution_outcome: u32,
    pub outcome_reason: u32,
    pub protocol_error: i64,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct ReplyFrame {
    pub struct_size: u32,
    pub flags: u32,
    pub bytes: u64,
    pub byte_count: u64,
    pub resources: u64,
    pub resource_count: u64,
    pub reserved0: u64,
    pub reserved1: u64,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct FailFrame {
    pub struct_size: u32,
    pub flags: u32,
    pub execution_outcome: u32,
    pub outcome_reason: u32,
    pub protocol_error: i64,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct MemoryMapFrame {
    pub struct_size: u32,
    pub flags: u32,
    pub hint: u64,
    pub object: Handle,
    pub offset: u64,
    pub length: u64,
    pub address: u64,
    pub reserved0: u64,
    pub reserved1: u64,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct MemoryUnmapFrame {
    pub struct_size: u32,
    pub flags: u32,
    pub address: u64,
    pub length: u64,
    pub reserved0: u64,
    pub reserved1: u64,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct TimeClock {
    pub tv_sec: i64,
    pub tv_nsec: i64,
}

unsafe extern "C" {
    pub fn _s_log(message: *const u8);
    pub fn _s_exit(status: i64) -> !;
    pub fn _s_exit_thread(status: i64) -> !;
    pub fn _s_clock(clock_index: i32, clock: *mut TimeClock) -> i32;
    pub fn _s_futex(pointer: *mut i32, operation: i32, value: i32, timeout: *const u8) -> i32;
    pub fn _s_sleep(clock: *const TimeClock) -> i32;
    pub fn _s_current_pid() -> i64;
    pub fn _s_current_tid() -> i64;
    pub fn _s_yield() -> i32;
    pub fn _s_tcb_set(pointer: *mut u8) -> i32;
    pub fn _s_clone(entry: *mut u8, argument: *mut u8, tcb: *mut u8) -> i32;

    pub fn _na_handle_close(handle: Handle) -> Status;
    pub fn _na_channel_create(
        options: *const ChannelOptions,
        left: *mut Handle,
        right: *mut Handle,
    ) -> Status;
    pub fn _na_channel_send(endpoint: Handle, frame: *const ChannelSendFrame) -> Status;
    pub fn _na_channel_receive(endpoint: Handle, frame: *mut ChannelReceiveFrame) -> Status;
    pub fn _na_channel_discard(endpoint: Handle) -> Status;
    pub fn _na_handle_wait_many(items: *mut WaitItem, count: u64, deadline: *const u8) -> Status;
    pub fn _na_handle_duplicate(source: Handle, rights: u64, result: *mut Handle) -> Status;
    pub fn _na_handle_restrict(
        source: Handle,
        restriction: *const HandleRestriction,
        result: *mut Handle,
    ) -> Status;
    pub fn _na_handle_get_info(handle: Handle, result: *mut HandleInfo) -> Status;
    pub fn _na_protocol_descriptor_create(
        input: *const ProtocolDescriptor,
        result: *mut Handle,
    ) -> Status;
    pub fn _na_protocol_endpoint_create(
        descriptor: Handle,
        options: *const ProtocolEndpointOptions,
        client: *mut Handle,
        server: *mut Handle,
    ) -> Status;
    pub fn _na_invoke_submit(
        target: Handle,
        frame: *const SubmitFrame,
        invocation: *mut Handle,
    ) -> Status;
    pub fn _na_invoke_oneway(target: Handle, frame: *const SubmitFrame) -> Status;
    pub fn _na_invocation_cancel(invocation: Handle) -> Status;
    pub fn _na_invocation_take_result(invocation: Handle, frame: *mut ResultFrame) -> Status;
    pub fn _na_responder_reply(responder: Handle, frame: *const ReplyFrame) -> Status;
    pub fn _na_responder_fail(responder: Handle, frame: *const FailFrame) -> Status;
    pub fn _na_bootstrap(frame: *mut BootstrapFrame) -> Status;
    pub fn _na_memory_map(frame: *mut MemoryMapFrame) -> Status;
    pub fn _na_memory_unmap(frame: *mut MemoryUnmapFrame) -> Status;
}

core::arch::global_asm!(include_str!("syscalls.S"), options(att_syntax));
