#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum TransitionRequest {
    Sleep { type_a: u32, type_b: u32 },
    Reset,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum TransitionKind {
    Sleep,
    Reset,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub(crate) struct TransitionTicket {
    pub kind: TransitionKind,
    pub ticket: u64,
    pub firmware_version: String,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum TransitionOutcome {
    Committed,
    Failed,
    Unknown,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum CommitError<E> {
    Rejected(E),
    Unknown(E),
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum TransitionFailure<E> {
    Prepare(E),
    CommitRejected(E),
    CommitUnknown(E),
    InvalidOutcome(u32),
}

pub(crate) trait TransitionBackend {
    type Error;

    async fn prepare(&self, request: TransitionRequest) -> Result<TransitionTicket, Self::Error>;
    async fn commit(&self, ticket: &TransitionTicket) -> Result<u32, CommitError<Self::Error>>;
    async fn abort(&self, ticket: &TransitionTicket) -> Result<(), Self::Error>;
}

pub(crate) async fn perform_transition<B: TransitionBackend>(
    backend: &B,
    request: TransitionRequest,
) -> Result<TransitionOutcome, TransitionFailure<B::Error>> {
    let ticket = backend
        .prepare(request)
        .await
        .map_err(TransitionFailure::Prepare)?;
    let result = match backend.commit(&ticket).await {
        Ok(0) => Ok(TransitionOutcome::Committed),
        Ok(1) => Ok(TransitionOutcome::Failed),
        Ok(2) => Ok(TransitionOutcome::Unknown),
        Ok(outcome) => Err(TransitionFailure::InvalidOutcome(outcome)),
        Err(CommitError::Rejected(error)) => {
            let _ = backend.abort(&ticket).await;
            Err(TransitionFailure::CommitRejected(error))
        }
        Err(CommitError::Unknown(error)) => Err(TransitionFailure::CommitUnknown(error)),
    };
    result
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::Mutex;

    struct FakeBackend {
        calls: Mutex<Vec<&'static str>>,
        prepare_result: Mutex<Option<Result<TransitionTicket, &'static str>>>,
        commit_result: Mutex<Option<Result<u32, CommitError<&'static str>>>>,
    }

    impl TransitionBackend for FakeBackend {
        type Error = &'static str;

        async fn prepare(
            &self,
            _request: TransitionRequest,
        ) -> Result<TransitionTicket, Self::Error> {
            self.calls.lock().unwrap().push("prepare");
            self.prepare_result.lock().unwrap().take().unwrap()
        }

        async fn commit(
            &self,
            _ticket: &TransitionTicket,
        ) -> Result<u32, CommitError<Self::Error>> {
            self.calls.lock().unwrap().push("commit");
            self.commit_result.lock().unwrap().take().unwrap()
        }

        async fn abort(&self, _ticket: &TransitionTicket) -> Result<(), Self::Error> {
            self.calls.lock().unwrap().push("abort");
            Ok(())
        }
    }

    fn backend(
        prepare_result: Result<TransitionTicket, &'static str>,
        commit_result: Result<u32, CommitError<&'static str>>,
    ) -> FakeBackend {
        FakeBackend {
            calls: Mutex::new(Vec::new()),
            prepare_result: Mutex::new(Some(prepare_result)),
            commit_result: Mutex::new(Some(commit_result)),
        }
    }

    fn ticket() -> TransitionTicket {
        TransitionTicket {
            kind: TransitionKind::Sleep,
            ticket: 7,
            firmware_version: "test-firmware-v1".to_owned(),
        }
    }

    #[tokio::test]
    async fn successful_transition_prepares_then_commits_without_abort() {
        let backend = backend(Ok(ticket()), Ok(0));

        assert_eq!(
            perform_transition(
                &backend,
                TransitionRequest::Sleep {
                    type_a: 5,
                    type_b: 6,
                },
            )
            .await,
            Ok(TransitionOutcome::Committed)
        );
        assert_eq!(*backend.calls.lock().unwrap(), ["prepare", "commit"]);
    }

    #[tokio::test]
    async fn rejected_commit_aborts_the_prepared_ticket() {
        let backend = backend(Ok(ticket()), Err(CommitError::Rejected("stale")));

        assert_eq!(
            perform_transition(&backend, TransitionRequest::Reset).await,
            Err(TransitionFailure::CommitRejected("stale"))
        );
        assert_eq!(
            *backend.calls.lock().unwrap(),
            ["prepare", "commit", "abort"]
        );
    }

    #[tokio::test]
    async fn uncertain_commit_is_reported_as_unknown_without_abort() {
        let backend = backend(Ok(ticket()), Err(CommitError::Unknown("transport")));

        assert_eq!(
            perform_transition(&backend, TransitionRequest::Reset).await,
            Err(TransitionFailure::CommitUnknown("transport"))
        );
        assert_eq!(*backend.calls.lock().unwrap(), ["prepare", "commit"]);
    }

    #[tokio::test]
    async fn failed_prepare_never_attempts_commit_or_abort() {
        let backend = backend(Err("not ready"), Ok(0));

        assert_eq!(
            perform_transition(&backend, TransitionRequest::Reset).await,
            Err(TransitionFailure::Prepare("not ready"))
        );
        assert_eq!(*backend.calls.lock().unwrap(), ["prepare"]);
    }

    #[tokio::test]
    async fn committed_unknown_outcome_is_not_claimed_as_a_rollback() {
        let backend = backend(Ok(ticket()), Ok(2));

        assert_eq!(
            perform_transition(&backend, TransitionRequest::Reset).await,
            Ok(TransitionOutcome::Unknown)
        );
        assert_eq!(*backend.calls.lock().unwrap(), ["prepare", "commit"]);
    }
}
