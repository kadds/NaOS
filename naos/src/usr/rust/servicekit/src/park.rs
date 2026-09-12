//! Yield-based terminal park loop for single-threaded early services.

use naos_sys as sys;

/// Park the calling thread forever.
///
/// Scheduler semantics (v1): `_s_yield` donates the remainder of the thread's
/// slice and re-queues it behind every runnable peer, so a parked service
/// costs exactly one pass through the run queue per scheduling round. There
/// is no blocking-on-handle primitive a terminal service could use for "wait
/// for nothing", and spinning on a timer would burn real CPU in the emulator;
/// yield-parking keeps boot deterministic while never starving other
/// processes. Services call this once their startup contract is complete and
/// their state lives in kernel objects (published listeners, held leases)
/// that outlive the idle thread.
pub fn park() -> ! {
    loop {
        // SAFETY: plain yield syscall; the return value carries no contract
        // a parked thread can act on.
        let _ = unsafe { sys::_s_yield() };
    }
}
