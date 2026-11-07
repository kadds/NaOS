//! Level-sensitive SCI lifecycle contract.
//!
//! The kernel ISR only masks/coalesces and emits sequence numbers.  AML
//! status draining and clearing happen outside interrupt context; a stale or
//! failed acknowledgement must never reopen the source.

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct SciEvent {
    pub sequence: u64,
    pub coalesced: bool,
    pub lost: bool,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum SciState {
    Unmasked,
    Masked {
        sequence: u64,
        pending: bool,
        lost: bool,
    },
    Degraded {
        sequence: u64,
        lost: bool,
    },
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum SciError {
    StaleSequence,
    NotMasked,
    AlreadyDegraded,
    SequenceExhausted,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum AckOutcome {
    Unmasked,
    Resignal(SciEvent),
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct SciLifecycle {
    state: SciState,
    next_sequence: u64,
}

impl Default for SciLifecycle {
    fn default() -> Self {
        Self {
            state: SciState::Unmasked,
            next_sequence: 1,
        }
    }
}

impl SciLifecycle {
    pub fn state(&self) -> SciState {
        self.state
    }

    /// ISR-side transition.  A second edge while masked is coalesced and
    /// marks loss/overflow rather than enqueueing an unbounded event stream.
    pub fn interrupt(&mut self) -> Result<SciEvent, SciError> {
        match self.state {
            SciState::Unmasked => {
                let sequence = match self.take_sequence() {
                    Ok(sequence) => sequence,
                    Err(error) => {
                        self.state = SciState::Degraded {
                            sequence: u64::MAX,
                            lost: true,
                        };
                        return Err(error);
                    }
                };
                self.state = SciState::Masked {
                    sequence,
                    pending: false,
                    lost: false,
                };
                Ok(SciEvent {
                    sequence,
                    coalesced: false,
                    lost: false,
                })
            }
            SciState::Masked {
                sequence,
                pending,
                lost,
            } => {
                self.state = SciState::Masked {
                    sequence,
                    pending: true,
                    lost: lost || pending,
                };
                Ok(SciEvent {
                    sequence,
                    coalesced: true,
                    lost: lost || pending,
                })
            }
            SciState::Degraded { .. } => Err(SciError::AlreadyDegraded),
        }
    }

    /// Validate the event currently owned by the platform worker and return
    /// its coalesced/lost diagnostic bits for the fixed-hardware drain.
    pub fn drain(&self, sequence: u64) -> Result<(bool, bool), SciError> {
        match self.state {
            SciState::Masked {
                sequence: current,
                pending,
                lost,
            } if current == sequence => Ok((pending, lost)),
            SciState::Masked { .. } => Err(SciError::StaleSequence),
            _ => Err(SciError::NotMasked),
        }
    }

    /// Recheck fixed status after the worker cleared handled bits.  Only the
    /// latest sequence may rearm; a newly pending level is re-signalled under
    /// a new sequence while the source remains masked until the next drain.
    pub fn ack_and_rearm(
        &mut self,
        sequence: u64,
        pending_after_drain: bool,
    ) -> Result<AckOutcome, SciError> {
        let (lost, current) = match self.state {
            SciState::Masked {
                sequence: current,
                lost,
                ..
            } => (lost, current),
            SciState::Unmasked => return Err(SciError::NotMasked),
            SciState::Degraded { .. } => return Err(SciError::AlreadyDegraded),
        };
        if current != sequence {
            return Err(SciError::StaleSequence);
        }
        if pending_after_drain {
            let next = match self.take_sequence() {
                Ok(sequence) => sequence,
                Err(error) => {
                    self.state = SciState::Degraded {
                        sequence: current,
                        lost: true,
                    };
                    return Err(error);
                }
            };
            let event = SciEvent {
                sequence: next,
                coalesced: true,
                lost,
            };
            self.state = SciState::Masked {
                sequence: next,
                pending: false,
                lost,
            };
            return Ok(AckOutcome::Resignal(event));
        }
        self.state = SciState::Unmasked;
        Ok(AckOutcome::Unmasked)
    }

    /// Keep SCI masked when the platform worker crashes, times out, or cannot
    /// complete a rearm transaction.  This is intentionally sticky.
    pub fn fail_degraded(&mut self) -> Result<(), SciError> {
        match self.state {
            SciState::Masked { sequence, lost, .. } => {
                self.state = SciState::Degraded { sequence, lost };
                Ok(())
            }
            SciState::Degraded { .. } => Err(SciError::AlreadyDegraded),
            SciState::Unmasked => Err(SciError::NotMasked),
        }
    }

    fn take_sequence(&mut self) -> Result<u64, SciError> {
        let sequence = self.next_sequence;
        if sequence == 0 {
            return Err(SciError::SequenceExhausted);
        }
        self.next_sequence = if sequence == u64::MAX {
            0
        } else {
            sequence + 1
        };
        Ok(sequence)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn coalesces_level_events_and_rearms_only_after_clear() {
        let mut sci = SciLifecycle::default();
        let first = sci.interrupt().unwrap();
        assert_eq!(sci.drain(first.sequence), Ok((false, false)));
        let second = sci.interrupt().unwrap();
        assert_eq!(
            second,
            SciEvent {
                sequence: first.sequence,
                coalesced: true,
                lost: false
            }
        );
        assert_eq!(
            sci.ack_and_rearm(first.sequence, true),
            Ok(AckOutcome::Resignal(SciEvent {
                sequence: 2,
                coalesced: true,
                lost: false,
            }))
        );
        assert_eq!(sci.ack_and_rearm(2, false), Ok(AckOutcome::Unmasked));
        assert_eq!(sci.state(), SciState::Unmasked);
    }

    #[test]
    fn stale_ack_does_not_unmask_and_overflow_is_observable() {
        let mut sci = SciLifecycle::default();
        let first = sci.interrupt().unwrap();
        let _ = sci.interrupt().unwrap();
        let _ = sci.interrupt().unwrap();
        assert_eq!(sci.drain(first.sequence), Ok((true, true)));
        assert_eq!(
            sci.ack_and_rearm(first.sequence + 1, false),
            Err(SciError::StaleSequence)
        );
        assert!(matches!(sci.state(), SciState::Masked { .. }));
    }

    #[test]
    fn sequence_exhaustion_degrades_without_reusing_a_stale_sequence() {
        let mut sci = SciLifecycle::default();
        sci.next_sequence = u64::MAX;
        let last = sci.interrupt().unwrap();
        assert_eq!(last.sequence, u64::MAX);
        assert_eq!(
            sci.ack_and_rearm(last.sequence, false),
            Ok(AckOutcome::Unmasked)
        );

        assert_eq!(sci.interrupt(), Err(SciError::SequenceExhausted));
        assert_eq!(
            sci.state(),
            SciState::Degraded {
                sequence: u64::MAX,
                lost: true,
            }
        );
        assert_eq!(sci.interrupt(), Err(SciError::AlreadyDegraded));
    }

    #[test]
    fn resignal_sequence_exhaustion_degrades_the_masked_source() {
        let mut sci = SciLifecycle::default();
        sci.next_sequence = u64::MAX;
        let last = sci.interrupt().unwrap();

        assert_eq!(
            sci.ack_and_rearm(last.sequence, true),
            Err(SciError::SequenceExhausted)
        );
        assert_eq!(
            sci.state(),
            SciState::Degraded {
                sequence: u64::MAX,
                lost: true,
            }
        );
    }

    #[test]
    fn worker_failure_is_sticky_and_masks_future_interrupts() {
        let mut sci = SciLifecycle::default();
        let _ = sci.interrupt().unwrap();
        assert_eq!(sci.fail_degraded(), Ok(()));
        assert_eq!(sci.interrupt(), Err(SciError::AlreadyDegraded));
        assert_eq!(sci.fail_degraded(), Err(SciError::AlreadyDegraded));
    }
}
