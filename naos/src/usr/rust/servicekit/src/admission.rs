//! Request admission accounting shared by both service transports.
//!
//! A service advertises `max_in_flight` because that is the number of requests
//! its dispatcher can actually hold at once.  This module makes the advertised
//! number the same value the transport enforces: [Server] admits a request,
//! attaches the returned permit to it, and the permit releases the slot when
//! the request is answered *or* dropped.  A dropped request therefore returns
//! its slot on peer close, cancellation and handler failure alike, so an
//! in-flight count can never leak.
//!
//! Refusal is explicit rather than implicit.  When a bounded queue is full the
//! peer must learn that the service is saturated instead of waiting for a
//! reply that the service has no capacity to produce.

use alloc::sync::Arc;
use core::fmt;
use core::sync::atomic::{AtomicU32, AtomicU64, Ordering};

/// The counters are atomics behind an `Arc` rather than `Cell`s behind an `Rc`
/// so a service's server stays `Send`: an async runtime may move it between
/// worker threads, and admission accounting must not be the thing that pins it
/// to one.

/// Why a request was refused admission.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum AdmissionRefusal {
    /// The service is already handling its advertised maximum.  The peer may
    /// retry, so this is a transient condition rather than an error.
    WouldBlock,
    /// The service has no capacity at all (its limit is zero).
    ResourceExhausted,
}

impl AdmissionRefusal {
    /// The errno a service reports for this refusal.
    ///
    /// EAGAIN is the retryable "busy" answer; ENOSPC is the permanent one.
    pub const fn errno(self) -> i64 {
        match self {
            AdmissionRefusal::WouldBlock => -11,
            AdmissionRefusal::ResourceExhausted => -28,
        }
    }
}

impl fmt::Display for AdmissionRefusal {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            AdmissionRefusal::WouldBlock => formatter.write_str("would block"),
            AdmissionRefusal::ResourceExhausted => formatter.write_str("resource exhausted"),
        }
    }
}

/// Counters for one admission domain.
#[derive(Clone, Debug)]
pub struct Admission {
    limit: Arc<AtomicU32>,
    in_flight: Arc<AtomicU32>,
    refused: Arc<AtomicU64>,
    peak: Arc<AtomicU32>,
}

impl Admission {
    /// Create an admission domain that accepts at most `limit` concurrent
    /// requests.  A limit of zero refuses everything, which is the honest
    /// description of a service that cannot dispatch at all.
    pub fn new(limit: u32) -> Self {
        Self {
            limit: Arc::new(AtomicU32::new(limit)),
            in_flight: Arc::new(AtomicU32::new(0)),
            refused: Arc::new(AtomicU64::new(0)),
            peak: Arc::new(AtomicU32::new(0)),
        }
    }

    /// Publish a new limit.  A service sets this from the same value it
    /// advertises, so the promised concurrency and the enforced concurrency
    /// cannot drift apart.
    pub fn set_limit(&self, limit: u32) {
        self.limit.store(limit, Ordering::Relaxed);
    }

    /// The advertised maximum number of concurrent requests.
    pub fn limit(&self) -> u32 {
        self.limit.load(Ordering::Relaxed)
    }

    /// Requests admitted and not yet released.
    pub fn in_flight(&self) -> u32 {
        self.in_flight.load(Ordering::Relaxed)
    }

    /// Requests refused since the domain was created.
    pub fn refused(&self) -> u64 {
        self.refused.load(Ordering::Relaxed)
    }

    /// Highest in-flight count observed.  A service that advertises concurrency
    /// it never reaches becomes visible here rather than staying a claim.
    pub fn peak_in_flight(&self) -> u32 {
        self.peak.load(Ordering::Relaxed)
    }

    /// Whether another request can be admitted right now.
    pub fn has_capacity(&self) -> bool {
        self.in_flight.load(Ordering::Relaxed) < self.limit.load(Ordering::Relaxed)
    }

    /// Reserve one slot, returning a permit that releases it on drop.
    ///
    /// This never waits: a transport must not park its reactor while a service
    /// drains, so saturation is reported to the caller instead.
    pub fn try_admit(&self) -> Result<AdmissionPermit, AdmissionRefusal> {
        let limit = self.limit.load(Ordering::Relaxed);
        let refusal = if limit == 0 {
            AdmissionRefusal::ResourceExhausted
        } else {
            AdmissionRefusal::WouldBlock
        };
        // A compare-exchange loop keeps the bound exact under concurrent
        // admission: two transports may race for the last slot.
        let mut current = self.in_flight.load(Ordering::Acquire);
        loop {
            if current >= limit {
                self.refused.fetch_add(1, Ordering::Relaxed);
                return Err(refusal);
            }
            match self.in_flight.compare_exchange_weak(
                current,
                current + 1,
                Ordering::AcqRel,
                Ordering::Acquire,
            ) {
                Ok(_) => {
                    let next = current + 1;
                    let peak = self.peak.load(Ordering::Relaxed);
                    if next > peak {
                        self.peak.fetch_max(next, Ordering::Relaxed);
                    }
                    return Ok(AdmissionPermit {
                        in_flight: Arc::clone(&self.in_flight),
                        held: true,
                    });
                }
                Err(observed) => current = observed,
            }
        }
    }
}

/// One admitted request.  Dropping it returns the slot, so answering a request
/// and abandoning one are the same release path.
#[derive(Debug)]
pub struct AdmissionPermit {
    in_flight: Arc<AtomicU32>,
    held: bool,
}

impl AdmissionPermit {
    /// Release the slot early.  The per-request paths use `Drop`; a transport
    /// that must hand the count to another owner can call this instead.
    pub fn release(&mut self) {
        if self.held {
            self.held = false;
            self.in_flight.fetch_sub(1, Ordering::AcqRel);
        }
    }

    /// Consume the permit without releasing it, transferring the release duty.
    pub fn into_count(mut self) -> Arc<AtomicU32> {
        self.held = false;
        Arc::clone(&self.in_flight)
    }
}

impl Drop for AdmissionPermit {
    fn drop(&mut self) {
        self.release();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn admits_up_to_its_limit_and_then_refuses() {
        let admission = Admission::new(2);
        let first = admission.try_admit().expect("first request admitted");
        let second = admission.try_admit().expect("second request admitted");
        assert_eq!(admission.in_flight(), 2);
        assert_eq!(admission.peak_in_flight(), 2);
        assert!(!admission.has_capacity());

        // The bound is what makes the queue bounded: the third request is
        // refused instead of queued.
        assert_eq!(admission.try_admit().unwrap_err(), AdmissionRefusal::WouldBlock);
        assert_eq!(admission.refused(), 1);

        // Releasing a permit frees exactly one slot.
        drop(first);
        assert_eq!(admission.in_flight(), 1);
        assert!(admission.has_capacity());
        let third = admission.try_admit().expect("slot was released");
        assert_eq!(admission.in_flight(), 2);
        // The peak does not grow past the limit even after churn.
        assert_eq!(admission.peak_in_flight(), 2);

        drop(second);
        drop(third);
        assert_eq!(admission.in_flight(), 0);
    }

    #[test]
    fn dropped_requests_release_their_slot() {
        // This is the peer-close and cancellation path: a request that is
        // admitted and then abandoned must not consume capacity forever.
        let admission = Admission::new(1);
        for _ in 0..1000 {
            let permit = admission.try_admit().expect("single slot is free again");
            drop(permit);
        }
        assert_eq!(admission.in_flight(), 0);
        assert_eq!(admission.peak_in_flight(), 1);
        assert_eq!(admission.refused(), 0);
    }

    #[test]
    fn a_zero_limit_refuses_permanently() {
        let admission = Admission::new(0);
        assert_eq!(
            admission.try_admit().unwrap_err(),
            AdmissionRefusal::ResourceExhausted
        );
        assert_eq!(admission.in_flight(), 0);
        assert_eq!(AdmissionRefusal::ResourceExhausted.errno(), -28);
        assert_eq!(AdmissionRefusal::WouldBlock.errno(), -11);
    }

    #[test]
    fn concurrency_is_observable_up_to_the_limit() {
        // A service that advertises N must be able to hold N requests at once;
        // this is the counter a pipeline assertion reads.
        let admission = Admission::new(4);
        let mut permits = alloc::vec::Vec::new();
        for expected in 1..=4u32 {
            permits.push(admission.try_admit().expect("capacity remains"));
            assert_eq!(admission.in_flight(), expected);
        }
        assert_eq!(admission.peak_in_flight(), admission.limit());
        assert_eq!(admission.try_admit().unwrap_err(), AdmissionRefusal::WouldBlock);
    }

    #[test]
    fn permits_share_one_counter_across_clones() {
        // The server keeps the `Admission` while each request owns a permit, so
        // the counter must be shared rather than copied.
        let admission = Admission::new(2);
        let observer = admission.clone();
        let first = admission.try_admit().expect("first");
        let second = admission.try_admit().expect("second");
        assert_eq!(observer.in_flight(), 2);
        assert_eq!(observer.refused(), 0);

        // Releasing through one handle is visible through the other, and the
        // freed slot is reusable.
        drop(first);
        assert_eq!(observer.in_flight(), 1);
        let third = admission.try_admit().expect("released slot is reusable");
        assert_eq!(observer.in_flight(), 2);

        // Now the domain is genuinely full, and the refusal is counted once.
        assert!(admission.try_admit().is_err());
        assert_eq!(observer.refused(), 1);
        assert_eq!(observer.peak_in_flight(), 2);

        drop(second);
        drop(third);
        assert_eq!(observer.in_flight(), 0);
    }

    #[test]
    fn a_raised_limit_takes_effect_immediately() {
        let admission = Admission::new(1);
        let permit = admission.try_admit().expect("first");
        assert!(admission.try_admit().is_err());
        admission.set_limit(2);
        let second = admission.try_admit().expect("raised limit admits again");
        assert_eq!(admission.limit(), 2);
        assert_eq!(admission.in_flight(), 2);
        drop(permit);
        drop(second);
        assert_eq!(admission.in_flight(), 0);
    }
}
