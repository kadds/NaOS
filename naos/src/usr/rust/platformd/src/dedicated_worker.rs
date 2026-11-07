use std::sync::mpsc::{self, SyncSender, TrySendError};
use std::thread::{self, JoinHandle};
use tokio::sync::oneshot;

const WORKER_QUEUE_CAPACITY: usize = 16;

enum Message<Request, Response> {
    Call {
        request: Request,
        reply: SyncSender<Response>,
    },
    AsyncCall {
        request: Request,
        reply: oneshot::Sender<Response>,
    },
    Shutdown,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum WorkerError {
    Disconnected,
    Busy,
}

pub(crate) struct DedicatedWorker<Request: Send + 'static, Response: Send + 'static> {
    sender: SyncSender<Message<Request, Response>>,
    thread: Option<JoinHandle<()>>,
}

impl<Request, Response> DedicatedWorker<Request, Response>
where
    Request: Send + 'static,
    Response: Send + 'static,
{
    #[cfg(test)]
    pub(crate) fn spawn(
        name: &str,
        mut handler: impl FnMut(Request) -> Response + Send + 'static,
    ) -> std::io::Result<Self> {
        Self::spawn_with_state(name, || (), move |_, request| handler(request))
    }

    pub(crate) fn spawn_with_state<State: 'static>(
        name: &str,
        initialize: impl FnOnce() -> State + Send + 'static,
        mut handler: impl FnMut(&mut State, Request) -> Response + Send + 'static,
    ) -> std::io::Result<Self> {
        let (sender, receiver) = mpsc::sync_channel(WORKER_QUEUE_CAPACITY);
        let thread = thread::Builder::new()
            .name(name.to_string())
            .spawn(move || {
                let mut state = initialize();
                while let Ok(message) = receiver.recv() {
                    match message {
                        Message::Call { request, reply } => {
                            let _ = reply.send(handler(&mut state, request));
                        }
                        Message::AsyncCall { request, reply } => {
                            if !reply.is_closed() {
                                let _ = reply.send(handler(&mut state, request));
                            }
                        }
                        Message::Shutdown => break,
                    }
                }
            })?;
        Ok(Self {
            sender,
            thread: Some(thread),
        })
    }

    /// Start a worker, report its kernel identity to the caller for one-time
    /// setup, and do not construct or use worker state until that setup is
    /// accepted by the caller.
    pub(crate) fn spawn_with_setup<State: 'static, Startup: Send + 'static>(
        name: &str,
        startup: impl FnOnce() -> Result<Startup, String> + Send + 'static,
        setup: impl FnOnce(Startup) -> Result<(), String>,
        initialize: impl FnOnce() -> State + Send + 'static,
        mut handler: impl FnMut(&mut State, Request) -> Response + Send + 'static,
    ) -> Result<Self, String> {
        let (sender, receiver) = mpsc::sync_channel(WORKER_QUEUE_CAPACITY);
        let (startup_sender, startup_receiver) = mpsc::sync_channel(1);
        let (setup_sender, setup_receiver) = mpsc::sync_channel(1);
        let thread = thread::Builder::new()
            .name(name.to_string())
            .spawn(move || {
                if startup_sender.send(startup()).is_err() {
                    return;
                }
                if !matches!(setup_receiver.recv(), Ok(Ok(()))) {
                    return;
                }
                let mut state = initialize();
                while let Ok(message) = receiver.recv() {
                    match message {
                        Message::Call { request, reply } => {
                            let _ = reply.send(handler(&mut state, request));
                        }
                        Message::AsyncCall { request, reply } => {
                            if !reply.is_closed() {
                                let _ = reply.send(handler(&mut state, request));
                            }
                        }
                        Message::Shutdown => break,
                    }
                }
            })
            .map_err(|error| format!("worker spawn failed: {error}"))?;

        let mut thread = Some(thread);
        let startup = startup_receiver
            .recv()
            .map_err(|_| "worker exited before startup setup".to_string())?;
        let setup_result = match startup {
            Ok(startup) => setup(startup),
            Err(error) => Err(error),
        };
        let _ = setup_sender.send(setup_result.clone());
        if let Err(error) = setup_result {
            if let Some(thread) = thread.take() {
                let _ = thread.join();
            }
            return Err(error);
        }
        Ok(Self { sender, thread })
    }

    pub(crate) fn call(&self, request: Request) -> Result<Response, WorkerError> {
        let (reply, response) = mpsc::sync_channel(1);
        self.sender
            .send(Message::Call { request, reply })
            .map_err(|_| WorkerError::Disconnected)?;
        response.recv().map_err(|_| WorkerError::Disconnected)
    }

    /// Enqueue work without blocking the caller's async executor thread. A
    /// full bounded queue is reported immediately so callers can preserve
    /// their own deadline and failure policy rather than blocking on send.
    pub(crate) async fn call_async(&self, request: Request) -> Result<Response, WorkerError> {
        let (reply, response) = oneshot::channel();
        match self.sender.try_send(Message::AsyncCall { request, reply }) {
            Ok(()) => response.await.map_err(|_| WorkerError::Disconnected),
            Err(TrySendError::Full(_)) => Err(WorkerError::Busy),
            Err(TrySendError::Disconnected(_)) => Err(WorkerError::Disconnected),
        }
    }
}

impl<Request, Response> Drop for DedicatedWorker<Request, Response>
where
    Request: Send + 'static,
    Response: Send + 'static,
{
    fn drop(&mut self) {
        let _ = self.sender.send(Message::Shutdown);
        if let Some(thread) = self.thread.take() {
            if thread.thread().id() != thread::current().id() {
                let _ = thread.join();
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::DedicatedWorker;

    #[test]
    fn requests_execute_on_one_stable_worker_thread() {
        let caller = std::thread::current().id();
        let worker = DedicatedWorker::spawn("test-aml-io", |value: u64| {
            (std::thread::current().id(), value + 1)
        })
        .unwrap();

        let (first_thread, first_value) = worker.call(41).unwrap();
        let (second_thread, second_value) = worker.call(99).unwrap();
        assert_ne!(first_thread, caller);
        assert_eq!(second_thread, first_thread);
        assert_eq!(first_value, 42);
        assert_eq!(second_value, 100);
    }

    #[test]
    fn worker_state_is_initialized_and_used_on_the_worker_thread() {
        use std::rc::Rc;

        let caller = std::thread::current().id();
        let worker = DedicatedWorker::spawn_with_state(
            "test-aml-executor",
            || (std::thread::current().id(), Rc::new(())),
            |(owner, local_state), value: u64| {
                (
                    *owner,
                    std::thread::current().id(),
                    Rc::strong_count(local_state),
                    value,
                )
            },
        )
        .unwrap();

        let (owner_thread, request_thread, state_references, value) = worker.call(17).unwrap();
        assert_ne!(owner_thread, caller);
        assert_eq!(request_thread, owner_thread);
        assert_eq!(state_references, 1);
        assert_eq!(value, 17);
    }

    #[test]
    fn startup_setup_runs_on_parent_before_worker_state_initialization() {
        use std::sync::{
            Arc,
            atomic::{AtomicBool, Ordering},
        };

        let caller = std::thread::current().id();
        let setup_complete = Arc::new(AtomicBool::new(false));
        let setup_complete_for_init = setup_complete.clone();
        let worker = DedicatedWorker::spawn_with_setup(
            "test-aml-thread-registration",
            || Ok(std::thread::current().id()),
            {
                let setup_complete = setup_complete.clone();
                move |worker_thread| {
                    assert_ne!(worker_thread, caller);
                    assert_eq!(std::thread::current().id(), caller);
                    setup_complete.store(true, Ordering::Release);
                    Ok(())
                }
            },
            move || {
                assert!(setup_complete_for_init.load(Ordering::Acquire));
                std::thread::current().id()
            },
            |owner, value: u32| (*owner, value),
        )
        .unwrap();

        let (owner, value) = worker.call(23).unwrap();
        assert_ne!(owner, caller);
        assert_eq!(value, 23);
    }

    #[tokio::test(flavor = "current_thread")]
    async fn async_call_receives_worker_result_without_blocking_the_runtime_thread() {
        let worker =
            DedicatedWorker::spawn("test-async-aml-executor", |value: u64| value + 1).unwrap();

        assert_eq!(worker.call_async(41).await, Ok(42));
    }

    #[tokio::test(flavor = "current_thread")]
    async fn canceled_queued_async_call_is_not_dispatched() {
        use std::{
            future::Future,
            sync::{Arc, Mutex, mpsc},
            task::Poll,
        };

        let (started_sender, started_receiver) = mpsc::channel();
        let (release_sender, release_receiver) = mpsc::channel();
        let requests = Arc::new(Mutex::new(Vec::new()));
        let worker_requests = Arc::clone(&requests);
        let worker = DedicatedWorker::spawn("test-cancel-queued-async-call", move |value: u64| {
            worker_requests.lock().unwrap().push(value);
            if value == 0 {
                started_sender.send(()).unwrap();
                release_receiver.recv().unwrap();
            }
            value
        })
        .unwrap();

        let mut first_call = Box::pin(worker.call_async(0));
        std::future::poll_fn(|context| {
            assert!(matches!(first_call.as_mut().poll(context), Poll::Pending));
            Poll::Ready(())
        })
        .await;
        started_receiver.recv().unwrap();

        let mut canceled_call = Box::pin(worker.call_async(1));
        std::future::poll_fn(|context| {
            assert!(matches!(
                canceled_call.as_mut().poll(context),
                Poll::Pending
            ));
            Poll::Ready(())
        })
        .await;
        drop(canceled_call);

        release_sender.send(()).unwrap();
        assert_eq!(worker.call_async(2).await, Ok(2));
        assert_eq!(first_call.await, Ok(0));
        assert_eq!(*requests.lock().unwrap(), [0, 2]);
    }
}
