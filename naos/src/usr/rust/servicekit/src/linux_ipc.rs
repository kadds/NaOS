//! Linux adapter for the platform-neutral IPC core.
//!
//! The queue, claim/commit protocol, accounting, and endpoint lifecycle live
//! in `naos/libipc`. This module supplies hosted allocation, locking, clock,
//! notification, and resource callbacks, then exposes an idiomatic Rust
//! endpoint to servicekit.

use std::sync::Arc;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::vec;
use std::vec::Vec;

use core::ffi::c_void;
use core::mem::size_of;
use core::ptr;
use naos_ipc_core_sys as raw;

struct HostContext {
    allocator: raw::Allocator,
    domain_lock: raw::Lock,
    channel_lock: raw::Lock,
    notifier: raw::WaitNotifier,
    clock: raw::Clock,
    handles: raw::HandleTable,
    domain_lock_state: AtomicBool,
    channel_lock_state: AtomicBool,
    notifications: AtomicU64,
    clock_ticks: AtomicU64,
}

impl HostContext {
    fn new() -> Box<Self> {
        let mut context = Box::new(Self {
            allocator: raw::Allocator {
                context: ptr::null_mut(),
                allocate: Some(host_allocate),
                deallocate: Some(host_deallocate),
            },
            domain_lock: raw::Lock {
                context: ptr::null_mut(),
                acquire: Some(host_lock_acquire),
                release: Some(host_lock_release),
            },
            channel_lock: raw::Lock {
                context: ptr::null_mut(),
                acquire: Some(host_lock_acquire),
                release: Some(host_lock_release),
            },
            notifier: raw::WaitNotifier {
                context: ptr::null_mut(),
                notify: Some(host_notify),
            },
            clock: raw::Clock {
                context: ptr::null_mut(),
                now: Some(host_clock_now),
            },
            handles: raw::HandleTable {
                context: ptr::null_mut(),
                validate_waitable: Some(host_validate_waitable),
                begin_transfer: Some(host_begin_transfer),
                restore_transfer: Some(host_restore_transfer),
                commit_transfer: Some(host_commit_transfer),
            },
            domain_lock_state: AtomicBool::new(false),
            channel_lock_state: AtomicBool::new(false),
            notifications: AtomicU64::new(0),
            clock_ticks: AtomicU64::new(0),
        });

        context.allocator.context = context_ptr(&*context);
        context.domain_lock.context = context_ptr(&context.domain_lock_state);
        context.channel_lock.context = context_ptr(&context.channel_lock_state);
        context.notifier.context = context_ptr(&context.notifications);
        context.clock.context = context_ptr(&context.clock_ticks);
        context.handles.context = context_ptr(&*context);
        context
    }
}

fn context_ptr<T>(value: &T) -> *mut c_void {
    value as *const T as *mut T as *mut c_void
}

unsafe extern "C" fn host_allocate(
    _context: *mut c_void,
    size: usize,
    alignment: usize,
) -> *mut c_void {
    let alignment = alignment.max(size_of::<*mut c_void>());
    if size == 0 || !alignment.is_power_of_two() {
        return ptr::null_mut();
    }
    let mut pointer = ptr::null_mut();
    let result = unsafe { libc::posix_memalign(&mut pointer, alignment, size) };
    if result == 0 {
        pointer
    } else {
        ptr::null_mut()
    }
}

unsafe extern "C" fn host_deallocate(
    _context: *mut c_void,
    pointer: *mut c_void,
    _size: usize,
    _alignment: usize,
) {
    if !pointer.is_null() {
        unsafe { libc::free(pointer) };
    }
}

unsafe extern "C" fn host_lock_acquire(context: *mut c_void) {
    let lock = unsafe { &*context.cast::<AtomicBool>() };
    while lock.swap(true, Ordering::Acquire) {
        std::hint::spin_loop();
    }
}

unsafe extern "C" fn host_lock_release(context: *mut c_void) {
    let lock = unsafe { &*context.cast::<AtomicBool>() };
    lock.store(false, Ordering::Release);
}

unsafe extern "C" fn host_notify(context: *mut c_void) {
    let notifications = unsafe { &*context.cast::<AtomicU64>() };
    notifications.fetch_add(1, Ordering::Relaxed);
}

unsafe extern "C" fn host_clock_now(context: *mut c_void) -> u64 {
    let ticks = unsafe { &*context.cast::<AtomicU64>() };
    ticks.fetch_add(1, Ordering::Relaxed) + 1
}

unsafe extern "C" fn host_validate_waitable(_context: *mut c_void, handle: u64) -> raw::Status {
    if handle == 0 {
        raw::STATUS_INVALID_HANDLE
    } else {
        raw::STATUS_OK
    }
}

unsafe extern "C" fn host_begin_transfer(
    _context: *mut c_void,
    handles: *const u64,
    count: usize,
    transaction: *mut *mut c_void,
) -> raw::Status {
    if transaction.is_null() || (count != 0 && handles.is_null()) {
        return raw::STATUS_INVALID_ARGUMENT;
    }
    unsafe { *transaction = ptr::null_mut() };
    raw::STATUS_OK
}

unsafe extern "C" fn host_restore_transfer(
    _context: *mut c_void,
    _transaction: *mut c_void,
) -> raw::Status {
    raw::STATUS_OK
}

unsafe extern "C" fn host_commit_transfer(_context: *mut c_void, _transaction: *mut c_void) {}

unsafe extern "C" fn release_host_resource(_context: *mut c_void, value: *mut c_void) {
    if !value.is_null() {
        unsafe { libc::free(value) };
    }
}

struct HostChannel {
    context: Box<HostContext>,
    domain: *mut raw::Domain,
    channel: *mut raw::Channel,
}

// The callback contexts are immutable after construction. The core invokes
// the channel lock around all mutable channel state and the remaining host
// state is atomic.
unsafe impl Send for HostChannel {}
unsafe impl Sync for HostChannel {}

impl Drop for HostChannel {
    fn drop(&mut self) {
        unsafe {
            raw::naos_ipc_channel_destroy(self.channel);
            raw::naos_ipc_domain_destroy(self.domain);
        }
    }
}

pub struct ChannelEndpoint {
    owner: Arc<HostChannel>,
    side: u8,
    max_bytes: usize,
    max_resources: usize,
}

// The C core serializes operations with the injected host lock. Endpoint
// handles can therefore be shared across threads like a service endpoint.
unsafe impl Send for ChannelEndpoint {}
unsafe impl Sync for ChannelEndpoint {}

#[derive(Debug, PartialEq, Eq)]
pub struct ReceivedMessage {
    pub bytes: Vec<u8>,
    pub resources: Vec<u64>,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ReceiveError {
    Status(raw::Status),
    BufferTooSmall {
        required_bytes: usize,
        required_resources: usize,
    },
}

impl ChannelEndpoint {
    pub fn pair(
        max_messages: u64,
        max_bytes: usize,
        max_resources: usize,
    ) -> Result<(Self, Self), raw::Status> {
        let mut owner = Arc::new(HostChannel {
            context: HostContext::new(),
            domain: ptr::null_mut(),
            channel: ptr::null_mut(),
        });
        {
            let owner_mut = Arc::get_mut(&mut owner).expect("new IPC owner is unique");
            let context = &*owner_mut.context;
            let domain_config = raw::DomainConfig {
                memory: &context.allocator,
                synchronization: &context.domain_lock,
                max_messages: max_messages.saturating_mul(8),
                max_bytes: (max_bytes as u64).saturating_mul(8),
                max_resources: (max_resources as u64).saturating_mul(8),
            };
            owner_mut.domain = unsafe { raw::naos_ipc_domain_create(&domain_config) };
            if owner_mut.domain.is_null() {
                return Err(raw::STATUS_RESOURCE_EXHAUSTED);
            }
            let channel_config = raw::ChannelConfig {
                owner_domain: owner_mut.domain,
                control_memory: &context.allocator,
                payload_memory: &context.allocator,
                synchronization: &context.channel_lock,
                notifier: &context.notifier,
                clock: &context.clock,
                handles: &context.handles,
                max_messages,
                max_bytes: max_bytes as u64,
                max_resources: max_resources as u64,
            };
            owner_mut.channel = unsafe { raw::naos_ipc_channel_create(&channel_config) };
            if owner_mut.channel.is_null() {
                unsafe { raw::naos_ipc_domain_destroy(owner_mut.domain) };
                owner_mut.domain = ptr::null_mut();
                return Err(raw::STATUS_RESOURCE_EXHAUSTED);
            }
        }

        unsafe {
            raw::naos_ipc_channel_side_reference_acquired(owner.channel, 0);
            raw::naos_ipc_channel_side_reference_acquired(owner.channel, 1);
        }
        Ok((
            Self {
                owner: Arc::clone(&owner),
                side: 0,
                max_bytes,
                max_resources,
            },
            Self {
                owner,
                side: 1,
                max_bytes,
                max_resources,
            },
        ))
    }

    pub fn send(&self, bytes: &[u8], resources: &[u64]) -> Result<(), raw::Status> {
        if bytes.len() > self.max_bytes || resources.len() > self.max_resources {
            return Err(raw::STATUS_INVALID_ARGUMENT);
        }
        let message = unsafe {
            raw::naos_ipc_message_create(
                self.owner.channel,
                bytes.len() as u64,
                resources.len() as u64,
            )
        };
        if message.is_null() {
            return Err(raw::STATUS_RESOURCE_EXHAUSTED);
        }
        if !bytes.is_empty() {
            unsafe {
                ptr::copy_nonoverlapping(
                    bytes.as_ptr(),
                    raw::naos_ipc_message_bytes(message),
                    bytes.len(),
                );
            }
        }

        let mut native_resources = Vec::with_capacity(resources.len());
        for value in resources {
            let storage = unsafe { libc::malloc(size_of::<u64>()) }.cast::<u64>();
            if storage.is_null() {
                for resource in &mut native_resources {
                    unsafe { raw::naos_ipc_resource_reset(resource) };
                }
                unsafe { raw::naos_ipc_message_destroy(message) };
                return Err(raw::STATUS_RESOURCE_EXHAUSTED);
            }
            unsafe { *storage = *value };
            native_resources.push(raw::Resource {
                context: ptr::null_mut(),
                value: storage.cast::<c_void>(),
                release: Some(release_host_resource),
            });
        }

        let status = unsafe {
            raw::naos_ipc_channel_enqueue(
                self.owner.channel,
                self.side,
                message,
                native_resources.as_mut_ptr(),
                native_resources.len(),
                0,
            )
        };
        for resource in &mut native_resources {
            if unsafe { raw::naos_ipc_resource_valid(resource) } != 0 {
                unsafe { raw::naos_ipc_resource_reset(resource) };
            }
        }
        if status != raw::STATUS_OK {
            unsafe { raw::naos_ipc_message_destroy(message) };
            return Err(status);
        }
        Ok(())
    }

    pub fn receive(&self) -> Result<ReceivedMessage, ReceiveError> {
        let mut bytes = vec![0; self.max_bytes];
        let mut resources = vec![0; self.max_resources];
        let (actual_bytes, actual_resources) = self.receive_into(&mut bytes, &mut resources)?;
        bytes.truncate(actual_bytes);
        resources.truncate(actual_resources);
        Ok(ReceivedMessage { bytes, resources })
    }

    pub fn receive_into(
        &self,
        bytes: &mut [u8],
        resources: &mut [u64],
    ) -> Result<(usize, usize), ReceiveError> {
        let mut message = ptr::null_mut();
        let status = unsafe {
            raw::naos_ipc_channel_claim_receive(self.owner.channel, self.side, &mut message)
        };
        if status != raw::STATUS_OK {
            return Err(ReceiveError::Status(status));
        }
        if message.is_null() {
            return Err(ReceiveError::Status(raw::STATUS_IO_ERROR));
        }
        let byte_count = unsafe { raw::naos_ipc_message_byte_count(message) } as usize;
        let resource_count = unsafe { raw::naos_ipc_message_resource_count(message) } as usize;
        if bytes.len() < byte_count || resources.len() < resource_count {
            unsafe {
                raw::naos_ipc_channel_cancel_receive(self.owner.channel, self.side, message);
            }
            return Err(ReceiveError::BufferTooSmall {
                required_bytes: byte_count,
                required_resources: resource_count,
            });
        }
        if byte_count != 0 {
            unsafe {
                ptr::copy_nonoverlapping(
                    raw::naos_ipc_message_const_bytes(message),
                    bytes.as_mut_ptr(),
                    byte_count,
                );
            }
        }
        for index in 0..resource_count {
            let mut resource = raw::Resource {
                context: ptr::null_mut(),
                value: ptr::null_mut(),
                release: None,
            };
            let resource_status = unsafe {
                raw::naos_ipc_message_take_resource(message, index as u64, &mut resource)
            };
            if resource_status != raw::STATUS_OK || resource.value.is_null() {
                let committed = unsafe {
                    raw::naos_ipc_channel_commit_receive(self.owner.channel, self.side, message)
                };
                if committed != 0 {
                    unsafe { raw::naos_ipc_message_destroy(message) };
                }
                return Err(ReceiveError::Status(if resource_status == raw::STATUS_OK {
                    raw::STATUS_IO_ERROR
                } else {
                    resource_status
                }));
            }
            resources[index] = unsafe { *resource.value.cast::<u64>() };
            unsafe { raw::naos_ipc_resource_reset(&mut resource) };
        }
        let committed =
            unsafe { raw::naos_ipc_channel_commit_receive(self.owner.channel, self.side, message) };
        if committed == 0 {
            return Err(ReceiveError::Status(raw::STATUS_IO_ERROR));
        }
        unsafe { raw::naos_ipc_message_destroy(message) };
        Ok((byte_count, resource_count))
    }

    pub fn signals(&self) -> u64 {
        unsafe { raw::naos_ipc_channel_signals(self.owner.channel, self.side) }
    }

    pub fn validate_wait_set(&self, values: &[u64]) -> Result<(), raw::Status> {
        let context = &*self.owner.context;
        let status = unsafe {
            raw::naos_ipc_validate_wait_set(&context.handles, values.as_ptr(), values.len())
        };
        (status == raw::STATUS_OK).then_some(()).ok_or(status)
    }
}

impl Drop for ChannelEndpoint {
    fn drop(&mut self) {
        unsafe {
            raw::naos_ipc_channel_side_reference_released(self.owner.channel, self.side);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::{ChannelEndpoint, ReceiveError};
    use crate::sys;
    use naos_ipc_core_sys as raw;
    use std::sync::Arc;
    use std::thread;

    #[test]
    fn shared_core_transfers_payload_and_resources() {
        let (sender, receiver) = ChannelEndpoint::pair(2, 128, 4).unwrap();
        sender.send(b"ping", &[42]).unwrap();
        let message = receiver.receive().unwrap();
        assert_eq!(message.bytes, b"ping");
        assert_eq!(message.resources, [42]);
    }

    #[test]
    fn backpressure_and_claim_cancel_are_shared_with_kernel_core() {
        let (sender, receiver) = ChannelEndpoint::pair(1, 128, 4).unwrap();
        sender.send(b"one", &[]).unwrap();
        assert_eq!(sender.send(b"two", &[]), Err(sys::STATUS_WOULD_BLOCK));
        let mut too_small = [0; 1];
        assert_eq!(
            receiver.receive_into(&mut too_small, &mut []),
            Err(ReceiveError::BufferTooSmall {
                required_bytes: 3,
                required_resources: 0,
            })
        );
        let message = receiver.receive().unwrap();
        assert_eq!(message.bytes, b"one");
    }

    #[test]
    fn concurrent_send_receive_uses_the_same_core_state_machine() {
        let (sender, receiver) = ChannelEndpoint::pair(8, 64, 1).unwrap();
        let sender = Arc::new(sender);
        let receiver = Arc::new(receiver);
        let producer = {
            let sender = Arc::clone(&sender);
            thread::spawn(move || {
                for value in 0..128u8 {
                    loop {
                        match sender.send(&[value], &[]) {
                            Ok(()) => break,
                            Err(sys::STATUS_WOULD_BLOCK) => thread::yield_now(),
                            Err(status) => panic!("send failed: {status}"),
                        }
                    }
                }
            })
        };
        let consumer = {
            let receiver = Arc::clone(&receiver);
            thread::spawn(move || {
                for expected in 0..128u8 {
                    loop {
                        match receiver.receive() {
                            Ok(message) => {
                                assert_eq!(message.bytes, [expected]);
                                break;
                            }
                            Err(ReceiveError::Status(sys::STATUS_WOULD_BLOCK)) => {
                                thread::yield_now()
                            }
                            Err(error) => panic!("receive failed: {error:?}"),
                        }
                    }
                }
            })
        };
        producer.join().unwrap();
        consumer.join().unwrap();
    }

    #[test]
    fn wait_set_validation_stays_in_the_adapter_boundary() {
        let (endpoint, _) = ChannelEndpoint::pair(1, 64, 1).unwrap();
        endpoint.validate_wait_set(&[1, 2]).unwrap();
        assert_eq!(
            endpoint.validate_wait_set(&[0]),
            Err(raw::STATUS_INVALID_HANDLE)
        );
    }

    unsafe extern "C" fn invalidate_waitable(
        _context: *mut core::ffi::c_void,
        handle: u64,
    ) -> raw::Status {
        if handle == 0 {
            raw::STATUS_WAIT_SET_INVALIDATED
        } else {
            raw::STATUS_OK
        }
    }

    #[test]
    fn wait_set_invalidation_is_propagated_without_capability_logic_in_core() {
        let handles = raw::HandleTable {
            context: core::ptr::null_mut(),
            validate_waitable: Some(invalidate_waitable),
            begin_transfer: None,
            restore_transfer: None,
            commit_transfer: None,
        };
        let values = [1, 0];
        let status =
            unsafe { raw::naos_ipc_validate_wait_set(&handles, values.as_ptr(), values.len()) };
        assert_eq!(status, raw::STATUS_WAIT_SET_INVALIDATED);
    }

    #[test]
    fn peer_close_is_reported_by_the_shared_state_machine() {
        let (sender, receiver) = ChannelEndpoint::pair(1, 64, 1).unwrap();
        drop(sender);
        assert_ne!(receiver.signals() & raw::SIGNAL_PEER_CLOSED, 0);
        assert_eq!(
            receiver.receive(),
            Err(ReceiveError::Status(raw::STATUS_PEER_CLOSED))
        );
    }

    #[test]
    fn invocation_state_is_bound_through_the_linux_rust_adapter() {
        let (endpoint, _peer) = ChannelEndpoint::pair(1, 64, 1).unwrap();
        let context = &*endpoint.owner.context;
        let config = raw::InvocationConfig {
            owner_domain: endpoint.owner.domain,
            control_memory: &context.allocator,
            payload_memory: &context.allocator,
            synchronization: &context.channel_lock,
            notifier: &context.notifier,
            clock: &context.clock,
            callbacks: core::ptr::null(),
            method_id: 9,
            operation_deadline: 0,
            max_response_bytes: 64,
            max_response_resources: 1,
        };
        let invocation = unsafe { raw::naos_ipc_invocation_create(&config) };
        assert!(!invocation.is_null());
        unsafe {
            assert_ne!(
                raw::naos_ipc_invocation_reserve_result_budget(invocation),
                0
            );
            assert_ne!(raw::naos_ipc_invocation_begin_receive(invocation), 0);
            assert_ne!(raw::naos_ipc_invocation_finish_dispatch(invocation), 0);
            assert_ne!(
                raw::naos_ipc_invocation_complete_failure(
                    invocation,
                    raw::EXECUTION_OUTCOME_UNKNOWN,
                    raw::OUTCOME_REASON_BROKER_FAILURE,
                    0,
                ),
                0
            );
            let mut info = raw::InvocationResultInfo::default();
            assert_eq!(
                raw::naos_ipc_invocation_claim_result(invocation, 0, 0, &mut info),
                raw::STATUS_OK
            );
            assert_eq!(info.method_id, 9);
            assert_eq!(info.execution_outcome, raw::EXECUTION_OUTCOME_UNKNOWN);
            assert_eq!(info.outcome_reason, raw::OUTCOME_REASON_BROKER_FAILURE);
            assert_eq!(
                raw::naos_ipc_invocation_commit_result(invocation),
                raw::STATUS_OK
            );
            raw::naos_ipc_invocation_destroy(invocation);
        }
    }
}
