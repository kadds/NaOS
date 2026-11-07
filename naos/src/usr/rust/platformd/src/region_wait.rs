/// Bound ordinary synchronous kernel IPC calls. Global Lock acquisition uses
/// the firmware timeout (already capped by the AML work-unit deadline) plus a
/// small reply-delivery grace so the caller never abandons a still-running
/// kernel acquisition.
pub(crate) const REGION_INVOCATION_TIMEOUT_US: u64 = 1_000_000;
pub(crate) const GLOBAL_LOCK_RESULT_GRACE_US: u64 = 100_000;

pub(crate) fn global_lock_kernel_wait_timeout(timeout_us: u64) -> Option<u64> {
    if timeout_us == u64::MAX {
        None
    } else {
        Some(timeout_us.saturating_add(GLOBAL_LOCK_RESULT_GRACE_US))
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum KernelCallWait {
    Completed,
    TimedOut,
    PeerClosed,
    ReadinessError,
    UnexpectedEvent,
    Failed(naos_sys::Status),
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum KernelCallFailure {
    Outcome {
        execution: u32,
        reason: u32,
        protocol_error: i64,
    },
    Status(naos_sys::Status),
    InvalidInvocation,
    InvalidResult,
}

pub(crate) fn wait_for_kernel_call(
    handle: naos_sys::Handle,
    wait: impl FnOnce(
        &[naos_sys::Handle],
        Option<u64>,
    ) -> Result<servicekit::ReadyEvent, naos_sys::Status>,
) -> KernelCallWait {
    wait_for_kernel_call_with_timeout(handle, Some(REGION_INVOCATION_TIMEOUT_US), wait)
}

pub(crate) fn wait_for_kernel_call_with_timeout(
    handle: naos_sys::Handle,
    timeout_us: Option<u64>,
    wait: impl FnOnce(
        &[naos_sys::Handle],
        Option<u64>,
    ) -> Result<servicekit::ReadyEvent, naos_sys::Status>,
) -> KernelCallWait {
    match wait(&[handle], timeout_us) {
        Ok(event) if event.readable => KernelCallWait::Completed,
        Ok(event) if event.read_closed || event.write_closed => KernelCallWait::PeerClosed,
        Ok(event) if event.error => KernelCallWait::ReadinessError,
        Ok(_) => KernelCallWait::UnexpectedEvent,
        Err(naos_sys::STATUS_WOULD_BLOCK) => KernelCallWait::TimedOut,
        Err(status) => KernelCallWait::Failed(status),
    }
}

pub(crate) fn classify_call_error(error: naos_idl::CallError) -> KernelCallFailure {
    match error {
        naos_idl::CallError::Outcome {
            execution,
            reason,
            protocol_error,
        } => KernelCallFailure::Outcome {
            execution,
            reason,
            protocol_error,
        },
        naos_idl::CallError::Status(status) => KernelCallFailure::Status(status),
        naos_idl::CallError::InvalidHandle | naos_idl::CallError::InvalidInvocation => {
            KernelCallFailure::InvalidInvocation
        }
        naos_idl::CallError::Codec(_) | naos_idl::CallError::Resource(_) => {
            KernelCallFailure::InvalidResult
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn kernel_region_wait_distinguishes_completion_timeout_and_peer_close() {
        let ready = servicekit::ReadyEvent {
            index: 0,
            readable: true,
            writable: false,
            read_closed: false,
            write_closed: false,
            error: false,
        };
        let completed = wait_for_kernel_call(naos_sys::HANDLE_INVALID, |handles, timeout_us| {
            assert_eq!(handles, &[naos_sys::HANDLE_INVALID]);
            assert_eq!(timeout_us, Some(REGION_INVOCATION_TIMEOUT_US));
            Ok(ready)
        });
        assert_eq!(completed, KernelCallWait::Completed);

        let timed_out = wait_for_kernel_call(naos_sys::HANDLE_INVALID, |_, _| {
            Err(naos_sys::STATUS_WOULD_BLOCK)
        });
        assert_eq!(timed_out, KernelCallWait::TimedOut);

        let peer_closed = wait_for_kernel_call(naos_sys::HANDLE_INVALID, |_, _| {
            Ok(servicekit::ReadyEvent {
                read_closed: true,
                readable: false,
                ..ready
            })
        });
        assert_eq!(peer_closed, KernelCallWait::PeerClosed);

        let wait_failed = wait_for_kernel_call(naos_sys::HANDLE_INVALID, |_, _| {
            Err(naos_sys::STATUS_NOT_SUPPORTED)
        });
        assert_eq!(
            wait_failed,
            KernelCallWait::Failed(naos_sys::STATUS_NOT_SUPPORTED)
        );
    }

    #[test]
    fn invocation_outcomes_and_transport_failures_remain_distinct() {
        assert_eq!(
            classify_call_error(naos_idl::CallError::Outcome {
                execution: naos_idl::EXECUTION_OUTCOME_UNKNOWN,
                reason: naos_idl::REASON_CANCEL_REQUESTED,
                protocol_error: -5,
            }),
            KernelCallFailure::Outcome {
                execution: naos_idl::EXECUTION_OUTCOME_UNKNOWN,
                reason: naos_idl::REASON_CANCEL_REQUESTED,
                protocol_error: -5,
            }
        );
        assert_eq!(
            classify_call_error(naos_idl::CallError::Status(naos_sys::STATUS_PEER_CLOSED)),
            KernelCallFailure::Status(naos_sys::STATUS_PEER_CLOSED)
        );
    }

    #[test]
    fn global_lock_wait_covers_kernel_deadline_and_reply_delivery() {
        let timeout = global_lock_kernel_wait_timeout(1_000_000);
        assert_eq!(timeout, Some(1_000_000 + GLOBAL_LOCK_RESULT_GRACE_US));
        let timed_out = wait_for_kernel_call_with_timeout(
            naos_sys::HANDLE_INVALID,
            timeout,
            |handles, timeout_us| {
                assert_eq!(handles, &[naos_sys::HANDLE_INVALID]);
                assert_eq!(timeout_us, Some(1_100_000));
                Err(naos_sys::STATUS_WOULD_BLOCK)
            },
        );
        assert_eq!(timed_out, KernelCallWait::TimedOut);
        assert_eq!(global_lock_kernel_wait_timeout(u64::MAX), None);
    }
}
