//! Device admission sequencing and the flush ordering domain (VFS ADR §6.1).
//!
//! `BlockDevice`'s I/O methods are `@concurrent`, so several requests on one
//! endpoint may be in flight at once and their handlers may complete out of
//! order.  `flush` must still observe every `write` admitted before it, which is
//! what the admission sequence is for: the device assigns a monotonically
//! increasing sequence before any side effect, each accepted `write` is tracked
//! from admission to its completion point, and a `flush` is satisfied only once
//! every smaller-sequence write got there.
//!
//! A write that ends without reaching the device poisons the ordering domain
//! instead of being waited for: `flush` then completes with that write's errno
//! (`EIO` for a device or data error, `ENODEV` for medium removal) and keeps
//! failing, because the failures it stands for remain unresolved.
//!
//! Everything here is host-testable: no syscalls, no capability handles, no
//! runtime.  The sequence space is per physical device, so a caller that serves
//! more than one medium keeps one domain per `medium_id`; a ramdiskd process
//! owns one medium and therefore one domain.

use alloc::collections::BTreeMap;

/// What a `flush` must do about the writes admitted before it.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum FlushDecision {
    /// Every earlier write reached its completion point; the flush may complete.
    Satisfied,
    /// An earlier write has not reached its completion point yet.  A caller
    /// whose backend completes writes asynchronously must wait for `oldest`
    /// and then ask again.
    Waiting { oldest_sequence: u64 },
    /// An earlier write ended without reaching the device, so this flush must
    /// fail with `errno` rather than wait for a durability guarantee that can
    /// no longer be made.
    Failed { errno: i64 },
}

/// The admission-sequence ledger for one physical device.
#[derive(Debug, Default)]
pub struct FlushDomain {
    next_sequence: u64,
    /// Writes admitted and not yet at their completion point, keyed by
    /// sequence so "every write admitted before N" is a range query.
    outstanding_writes: BTreeMap<u64, ()>,
    /// The earliest admitted write that ended without reaching the device,
    /// together with the errno every later flush must report.
    poisoned_from: Option<(u64, i64)>,
}

impl FlushDomain {
    pub fn new() -> Self {
        Self::default()
    }

    /// Allocate the next sequence.
    ///
    /// Called before any device side effect, so the order of allocation is the
    /// order of admission.
    fn take_sequence(&mut self) -> u64 {
        // Sequences are only ever compared with each other, so wrapping is not
        // a correctness problem; saturating keeps the value total instead of
        // panicking on a decades-long counter.
        let sequence = self.next_sequence;
        self.next_sequence = self.next_sequence.saturating_add(1);
        sequence
    }

    /// Admit a `write`: allocate its sequence and track it until completion.
    pub fn admit_write(&mut self) -> u64 {
        let sequence = self.take_sequence();
        self.outstanding_writes.insert(sequence, ());
        sequence
    }

    /// Admit a `flush`: allocate its sequence so "writes admitted before it" is
    /// well defined, without adding a ledger entry of its own.
    pub fn admit_flush(&mut self) -> u64 {
        self.take_sequence()
    }

    /// Record that an admitted write reached its completion point, or that it
    /// ended as `errno` without reaching it.
    ///
    /// `Ok` is the normal case: the write is no longer in the way of any flush.
    /// `Err` poisons every later flush.  A sequence that was never admitted is
    /// ignored, so a duplicated completion cannot invent a failure.
    pub fn complete_write(&mut self, sequence: u64, outcome: Result<(), i64>) {
        if self.outstanding_writes.remove(&sequence).is_none() {
            return;
        }
        if let Err(errno) = outcome {
            // Keep the *earliest* poisoned write: a flush only answers for the
            // writes admitted before it, so the first one that failed is the
            // one whose errno it must report.
            let replace = match self.poisoned_from {
                Some((existing, _)) => sequence < existing,
                None => true,
            };
            if replace {
                self.poisoned_from = Some((sequence, errno));
            }
        }
    }

    /// Decide whether a flush admitted at `sequence` may complete.
    pub fn flush_decision(&self, sequence: u64) -> FlushDecision {
        if let Some((poisoned, errno)) = self.poisoned_from
            && poisoned < sequence
        {
            return FlushDecision::Failed { errno };
        }
        if let Some((oldest, _)) = self.outstanding_writes.range(..sequence).next() {
            return FlushDecision::Waiting {
                oldest_sequence: *oldest,
            };
        }
        FlushDecision::Satisfied
    }

    /// Writes admitted and not yet known to have reached the device.
    pub fn in_flight_writes(&self) -> usize {
        self.outstanding_writes.len()
    }

    /// The earliest admitted write that has not reached its completion point.
    ///
    /// A write dispatcher that lets requests overlap uses this to stay in
    /// arrival order without a lock: a write may apply only once it is the
    /// oldest outstanding one, so the medium sees writes in the order they were
    /// admitted even though several are in flight.
    pub fn oldest_outstanding(&self) -> Option<u64> {
        self.outstanding_writes.keys().next().copied()
    }

    /// Whether `sequence` is still waiting for its completion point.
    pub fn is_outstanding(&self, sequence: u64) -> bool {
        self.outstanding_writes.contains_key(&sequence)
    }

    /// Whether an earlier write failed terminally, and with which errno.
    pub fn poisoned_with(&self) -> Option<i64> {
        self.poisoned_from.map(|(_, errno)| errno)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The sequence is what makes "admitted before" meaningful, so it must
    /// never repeat or go backwards across mixes of writes and flushes.
    #[test]
    fn sequences_increase_across_writes_and_flushes() {
        let mut domain = FlushDomain::new();
        let first = domain.admit_write();
        let second = domain.admit_flush();
        let third = domain.admit_write();
        assert!(first < second && second < third);
    }

    /// A write that reached the device is not in the way of a later flush.
    #[test]
    fn flush_is_satisfied_when_every_earlier_write_completed() {
        let mut domain = FlushDomain::new();
        let write = domain.admit_write();
        domain.complete_write(write, Ok(()));
        let flush = domain.admit_flush();
        assert_eq!(domain.flush_decision(flush), FlushDecision::Satisfied);
        assert_eq!(domain.in_flight_writes(), 0);
    }

    /// The ordering guarantee itself: a flush must not pass a write that has
    /// not reached the device, which is the case that only exists once handlers
    /// can complete out of order.
    #[test]
    fn flush_waits_for_an_earlier_write_that_has_not_completed() {
        let mut domain = FlushDomain::new();
        let write = domain.admit_write();
        let flush = domain.admit_flush();
        assert_eq!(
            domain.flush_decision(flush),
            FlushDecision::Waiting {
                oldest_sequence: write
            }
        );
        // Completing the write releases it.
        domain.complete_write(write, Ok(()));
        assert_eq!(domain.flush_decision(flush), FlushDecision::Satisfied);
    }

    /// A write admitted *after* the flush is not the flush's business: waiting
    /// on it would make every flush wait for traffic that arrived later.
    #[test]
    fn flush_ignores_writes_admitted_after_it() {
        let mut domain = FlushDomain::new();
        let flush = domain.admit_flush();
        let _later_write = domain.admit_write();
        assert_eq!(domain.flush_decision(flush), FlushDecision::Satisfied);
    }

    /// A write that never reached the device cannot be waited for, so the flush
    /// carries the failure instead of claiming durability.
    #[test]
    fn flush_fails_with_the_errno_of_a_failed_earlier_write() {
        let mut domain = FlushDomain::new();
        let write = domain.admit_write();
        domain.complete_write(write, Err(-5)); // EIO
        let flush = domain.admit_flush();
        assert_eq!(domain.flush_decision(flush), FlushDecision::Failed { errno: -5 });
        assert_eq!(domain.poisoned_with(), Some(-5));
    }

    /// The terminal state is retained: a filesystem that retries flush after a
    /// device error must keep seeing the error rather than a false success.
    #[test]
    fn a_failed_write_keeps_failing_later_flushes() {
        let mut domain = FlushDomain::new();
        let write = domain.admit_write();
        domain.complete_write(write, Err(-19)); // ENODEV
        for _ in 0..3 {
            let flush = domain.admit_flush();
            assert_eq!(
                domain.flush_decision(flush),
                FlushDecision::Failed { errno: -19 }
            );
        }
    }

    /// Medium removal is reported as itself rather than folded into EIO, which
    /// is what lets a filesystem tell "the disk is gone" from "the disk erred".
    #[test]
    fn medium_removal_errno_is_reported_unchanged() {
        let mut domain = FlushDomain::new();
        let write = domain.admit_write();
        domain.complete_write(write, Err(-19));
        let flush = domain.admit_flush();
        assert_eq!(domain.flush_decision(flush), FlushDecision::Failed { errno: -19 });
    }

    /// Only writes admitted before the flush poison it; a failure that happened
    /// after it cannot retroactively invalidate an already-satisfied flush.
    #[test]
    fn a_write_failing_after_the_flush_does_not_poison_it() {
        let mut domain = FlushDomain::new();
        let flush = domain.admit_flush();
        let later_write = domain.admit_write();
        domain.complete_write(later_write, Err(-5));
        assert_eq!(domain.flush_decision(flush), FlushDecision::Satisfied);
    }

    /// The earliest failure is the one reported, so the errno a flush carries
    /// corresponds to the first unresolved write rather than an arbitrary one.
    #[test]
    fn the_earliest_failure_is_the_reported_one() {
        let mut domain = FlushDomain::new();
        let first = domain.admit_write();
        let second = domain.admit_write();
        // The later write fails first in wall-clock order; the earlier failure
        // must still win because it is the earlier admission.
        domain.complete_write(second, Err(-19));
        domain.complete_write(first, Err(-5));
        let flush = domain.admit_flush();
        assert_eq!(domain.flush_decision(flush), FlushDecision::Failed { errno: -5 });
    }

    /// A completion for a sequence that was never admitted must not invent
    /// state; a spurious duplicate completion would otherwise poison flushes.
    #[test]
    fn completing_an_unknown_sequence_changes_nothing() {
        let mut domain = FlushDomain::new();
        domain.complete_write(999, Err(-5));
        assert_eq!(domain.poisoned_with(), None);
        assert_eq!(domain.in_flight_writes(), 0);
        let flush = domain.admit_flush();
        assert_eq!(domain.flush_decision(flush), FlushDecision::Satisfied);
    }

    /// The in-flight count is the observability hook a service reports, so it
    /// must track admission and completion exactly.
    #[test]
    fn in_flight_writes_tracks_admission_and_completion() {
        let mut domain = FlushDomain::new();
        let first = domain.admit_write();
        let second = domain.admit_write();
        assert_eq!(domain.in_flight_writes(), 2);
        domain.complete_write(first, Ok(()));
        assert_eq!(domain.in_flight_writes(), 1);
        domain.complete_write(second, Ok(()));
        assert_eq!(domain.in_flight_writes(), 0);
    }

    /// Overlapping write dispatch stays in arrival order by letting only the
    /// oldest outstanding write apply, so this query is what the dispatcher
    /// gates on: it must name the earliest admitted write and advance as each
    /// one completes, including when they complete out of order.
    #[test]
    fn oldest_outstanding_names_the_earliest_unfinished_write() {
        let mut domain = FlushDomain::new();
        assert_eq!(domain.oldest_outstanding(), None);

        let first = domain.admit_write();
        let second = domain.admit_write();
        let third = domain.admit_write();
        assert_eq!(domain.oldest_outstanding(), Some(first));
        assert!(domain.is_outstanding(second));

        // The middle write finishing first must not let `third` jump the queue:
        // the oldest is still `first`.
        domain.complete_write(second, Ok(()));
        assert_eq!(domain.oldest_outstanding(), Some(first));
        assert!(!domain.is_outstanding(second));

        domain.complete_write(first, Ok(()));
        assert_eq!(domain.oldest_outstanding(), Some(third));
        domain.complete_write(third, Ok(()));
        assert_eq!(domain.oldest_outstanding(), None);
    }
}
