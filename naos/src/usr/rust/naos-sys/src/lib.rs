#![no_std]

pub type Handle = u64;
pub type Status = u32;

pub const HANDLE_INVALID: Handle = 0;

pub const STATUS_OK: Status = 0;
pub const STATUS_INVALID_HANDLE: Status = 1;
pub const STATUS_INVALID_ARGUMENT: Status = 5;
pub const STATUS_INVALID_MESSAGE: Status = 6;
pub const STATUS_RESOURCE_EXHAUSTED: Status = 10;
pub const STATUS_FAULT: Status = 11;
pub const STATUS_NOT_SUPPORTED: Status = 15;

pub const MAX_BOOTSTRAP_CAPABILITIES: usize = 8;
pub const BOOTSTRAP_FLAG_REBIND_CONSOLE: u32 = 1;

pub const SYSCALL_LOG: u64 = 1;
pub const SYSCALL_EXIT: u64 = 4;
pub const SYSCALL_HANDLE_CLOSE: u64 = 17;
pub const SYSCALL_CHANNEL_CREATE: u64 = 18;
pub const SYSCALL_CHANNEL_SEND: u64 = 19;
pub const SYSCALL_CHANNEL_RECEIVE: u64 = 20;
pub const SYSCALL_HANDLE_WAIT_MANY: u64 = 22;
pub const SYSCALL_HANDLE_DUPLICATE: u64 = 23;
pub const SYSCALL_HANDLE_GET_INFO: u64 = 25;
pub const SYSCALL_BOOTSTRAP: u64 = 34;

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

unsafe extern "C" {
    pub fn _s_log(message: *const u8);
    pub fn _s_exit(status: i64) -> !;

    pub fn _na_handle_close(handle: Handle) -> Status;
    pub fn _na_channel_create(options: *const ChannelOptions, left: *mut Handle, right: *mut Handle) -> Status;
    pub fn _na_channel_send(endpoint: Handle, frame: *const ChannelSendFrame) -> Status;
    pub fn _na_channel_receive(endpoint: Handle, frame: *mut ChannelReceiveFrame) -> Status;
    pub fn _na_handle_wait_many(items: *mut WaitItem, count: u64, deadline: *const u8) -> Status;
    pub fn _na_handle_duplicate(source: Handle, rights: u64, result: *mut Handle) -> Status;
    pub fn _na_handle_get_info(handle: Handle, result: *mut HandleInfo) -> Status;
    pub fn _na_bootstrap(frame: *mut BootstrapFrame) -> Status;
}

core::arch::global_asm!(include_str!("syscalls.S"), options(att_syntax));
