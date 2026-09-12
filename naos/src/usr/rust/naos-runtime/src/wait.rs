use naos_sys as sys;

/// A single readiness result for the low-level, synchronous runtime wait.
///
/// Async services should use Tokio's selector through servicekit.  This
/// fallback exists for the bootstrap/runtime paths that must synchronously
/// wait before an async executor is available.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ReadyEvent {
    pub index: usize,
    pub readable: bool,
    pub writable: bool,
    pub read_closed: bool,
    pub write_closed: bool,
    pub error: bool,
}

fn deadline_after(timeout_us: Option<u64>) -> Result<Option<sys::TimeClock>, sys::Status> {
    let Some(timeout_us) = timeout_us else {
        return Ok(None);
    };
    // The existing synchronous callers use u64::MAX to mean no deadline.
    if timeout_us == u64::MAX {
        return Ok(None);
    }

    let mut now = sys::TimeClock::default();
    if unsafe { sys::_s_clock(1, &mut now) } != 0 || now.tv_sec < 0 || now.tv_nsec < 0 {
        return Err(sys::STATUS_IO_ERROR);
    }

    let timeout_sec = timeout_us / 1_000_000;
    let timeout_nsec = (timeout_us % 1_000_000) * 1_000;
    let max_sec = i64::MAX as u64;
    let mut seconds = now.tv_sec.saturating_add(timeout_sec.min(max_sec) as i64);
    let mut nanos = now.tv_nsec.saturating_add(timeout_nsec as i64);
    if nanos >= 1_000_000_000 {
        seconds = seconds.saturating_add(1);
        nanos -= 1_000_000_000;
    }
    Ok(Some(sys::TimeClock {
        tv_sec: seconds,
        tv_nsec: nanos,
    }))
}

/// Wait for one handle to become ready through the kernel runtime adapter.
///
/// This is deliberately below servicekit. It is not the service event loop;
/// it is only the synchronous compatibility path used by startup and legacy
/// blocking client helpers.
pub fn wait_ready(
    handles: &[sys::Handle],
    timeout_us: Option<u64>,
) -> Result<ReadyEvent, sys::Status> {
    if handles.is_empty() {
        return Err(sys::STATUS_INVALID_ARGUMENT);
    }

    let deadline = deadline_after(timeout_us)?;
    let mut epoll = sys::HANDLE_INVALID;
    let mut status = unsafe { sys::_na_epoll_create(&mut epoll) };
    if status != sys::STATUS_OK {
        return Err(status);
    }

    for (index, handle) in handles.iter().copied().enumerate() {
        let event = sys::EpollEvent {
            events: sys::EPOLL_EVENT_READABLE | sys::EPOLL_EVENT_WRITABLE,
            data: index as u64,
        };
        status = unsafe { sys::_na_epoll_ctl(epoll, sys::EPOLL_CTL_ADD, handle, &event) };
        if status != sys::STATUS_OK {
            unsafe { sys::_na_handle_close(epoll) };
            return Err(status);
        }
    }

    let mut returned = [sys::EpollEvent::default(); 1];
    let mut actual = 0;
    status = unsafe {
        sys::_na_epoll_wait(
            epoll,
            returned.as_mut_ptr(),
            returned.len() as u64,
            &mut actual,
            deadline.as_ref().map_or(core::ptr::null(), |value| {
                value as *const sys::TimeClock as *const u8
            }),
        )
    };
    unsafe { sys::_na_handle_close(epoll) };

    if status != sys::STATUS_OK {
        return Err(status);
    }
    if actual == 0 {
        return Err(sys::STATUS_WOULD_BLOCK);
    }

    let event = returned[0];
    Ok(ReadyEvent {
        index: event.data as usize,
        readable: event.events & sys::EPOLL_EVENT_READABLE != 0,
        writable: event.events & sys::EPOLL_EVENT_WRITABLE != 0,
        read_closed: event.events & sys::EPOLL_EVENT_HANGUP != 0,
        write_closed: event.events & sys::EPOLL_EVENT_HANGUP != 0,
        error: event.events & sys::EPOLL_EVENT_ERROR != 0,
    })
}

pub fn wait_for_completion(handle: sys::Handle, timeout_us: u64) -> bool {
    wait_ready(&[handle], Some(timeout_us)).is_ok_and(|event| event.readable)
}
