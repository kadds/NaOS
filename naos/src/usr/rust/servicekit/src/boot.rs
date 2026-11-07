//! Early-boot adapters shared by services.
//!
//! Daemon code asks for kernel-published boot modules and asks the platform to
//! start an ordinary child. NaOS implements those operations with its
//! MemoryObject/process ABI; Linux reports that an external host runner owns
//! those concerns.

#[cfg(target_os = "linux")]
use alloc::vec::Vec;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum BootError {
    Unavailable,
    Invalid,
    Io,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum SpawnError {
    Unavailable,
    Invalid,
    Io,
    Status(naos_sys::Status),
}

fn start_outcome_is_definitive_rejection(execution: u32, reason: u32, protocol_error: i64) -> bool {
    execution == 0 && reason == 0 && protocol_error != 0
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
struct BootstrapMessage {
    struct_size: u32,
    flags: u32,
    version: u32,
    resource_count: u32,
    root_directory: u32,
    current_directory: u32,
    service_directory: u32,
    stdin_stream: u32,
    stdout_stream: u32,
    stderr_stream: u32,
    argc: u64,
    envc: u64,
    reserved0: u64,
    reserved1: u64,
}

const _: () = assert!(core::mem::size_of::<BootstrapMessage>() == 72);

fn early_service_bootstrap_message() -> BootstrapMessage {
    BootstrapMessage {
        struct_size: core::mem::size_of::<BootstrapMessage>() as u32,
        flags: naos_sys::BOOTSTRAP_FLAG_EARLY_SERVICE,
        version: 5,
        resource_count: 4,
        root_directory: naos_sys::BOOTSTRAP_RESOURCE_NONE,
        current_directory: naos_sys::BOOTSTRAP_RESOURCE_NONE,
        service_directory: 0,
        stdin_stream: 1,
        stdout_stream: 2,
        stderr_stream: 3,
        argc: 1,
        envc: 0,
        reserved0: 0,
        reserved1: 0,
    }
}

#[cfg(target_os = "linux")]
pub fn read_module(_context: &crate::Context, _uri: &str) -> Result<Vec<u8>, BootError> {
    Err(BootError::Unavailable)
}

#[cfg(target_os = "linux")]
pub fn spawn_init(
    _context: &crate::Context,
    _executable: &[u8],
    _root: &mut crate::server::EndpointResource,
    _cwd: &mut crate::server::EndpointResource,
) -> Result<u64, SpawnError> {
    Err(SpawnError::Unavailable)
}

#[cfg(target_os = "linux")]
pub fn spawn_early_service(
    _context: &crate::Context,
    _executable: &[u8],
    _path: &str,
) -> Result<u64, SpawnError> {
    Err(SpawnError::Unavailable)
}

#[cfg(target_os = "naos")]
mod naos_boot {
    use super::{BootError, BootstrapMessage, SpawnError, early_service_bootstrap_message};
    use crate::naos::Channel;
    use crate::server::EndpointResource;
    use crate::{Context, memory};
    use alloc::vec::Vec;
    use core::mem::size_of;
    use naos_idl::process as process_idl;
    use naos_idl::{OwnedHandle, ProtocolClientEndpoint, ResourceTransferBuilder};
    use naos_sys as sys;

    const BOOT_MODULE_MAX_BYTES: usize = 64 << 20;
    const BOOT_MODULE_COPY_CHUNK_BYTES: usize = 1 << 20;
    const PROCESS_START_REPLY_TIMEOUT_US: u64 = 1_000_000;

    /// Open a kernel-published boot module without copying its bytes.
    ///
    /// The caller owns the returned capability and may map bounded windows as
    /// it consumes the module.  This is the path for large data-plane modules
    /// such as the prepared root image; executable boot modules still use
    /// `read_module` because their child loader needs an owned byte slice.
    pub fn open_module(context: &Context, uri: &str) -> Result<memory::MemoryObject, BootError> {
        let module = crate::naos::resolve_resource(context.service_directory_handle(), uri)
            .map_err(|_| BootError::Unavailable)?;
        let object = memory::MemoryObject::from_owned(module);
        let size = object.size().ok_or(BootError::Invalid)?;
        if size == 0 || size > BOOT_MODULE_MAX_BYTES as u64 {
            return Err(BootError::Invalid);
        }
        Ok(object)
    }

    /// Read a kernel-published boot module through ServiceDirectory.  The
    /// module is deliberately obtained as a normal MemoryObject resource so
    /// the manager, rather than kernel bootstrap code, controls when the
    /// child process is created and started.
    pub fn read_module(context: &Context, uri: &str) -> Result<Vec<u8>, BootError> {
        read_memory_resource(context, uri)
    }

    fn read_memory_resource(context: &Context, uri: &str) -> Result<Vec<u8>, BootError> {
        let module = open_module(context, uri)?;
        let best = usize::try_from(module.size().ok_or(BootError::Invalid)?)
            .map_err(|_| BootError::Invalid)?;
        // Boot modules are external MemoryObjects. Their non-page-backed view
        // is faulted through a temporary shadow page, so mapping the complete
        // image while copying it into the owning Vec doubles the module's
        // resident footprint. Keep only a bounded source window faulted at a
        // time; the destination remains one owned copy for ramdiskd/process
        // startup, while each source window is unmapped before the next one.
        let mut image = Vec::with_capacity(best);
        let mut offset = 0usize;
        while offset < best {
            let length = core::cmp::min(BOOT_MODULE_COPY_CHUNK_BYTES, best - offset);
            let mapping = crate::memory::map_read_at(module.as_handle(), offset as u64, length)
                .map_err(|_| BootError::Io)?;
            image.extend_from_slice(mapping.as_slice());
            offset += length;
        }
        Ok(image)
    }

    fn duplicate(handle: sys::Handle) -> Result<sys::Handle, SpawnError> {
        let mut duplicate = sys::HANDLE_INVALID;
        let status = unsafe { sys::_na_handle_duplicate(handle, 0, &mut duplicate) };
        if status != sys::STATUS_OK || duplicate == sys::HANDLE_INVALID {
            return Err(SpawnError::Status(status));
        }
        Ok(duplicate)
    }

    fn wait_start(process: &OwnedHandle) -> Result<(), SpawnError> {
        let endpoint = unsafe { ProtocolClientEndpoint::from_raw(duplicate(process.get())?) };
        let mut request_wire = [0u8; 64];
        let mut invocation = process_idl::submit_start(
            &endpoint,
            &process_idl::start_request {},
            ResourceTransferBuilder::new(),
            &mut request_wire,
            0,
        )
        .map_err(|_| SpawnError::Io)?;
        if !crate::naos::wait_for_completion(invocation.get(), u64::MAX) {
            return Err(SpawnError::Status(sys::STATUS_IO_ERROR));
        }
        let mut response_wire = [0u8; 64];
        process_idl::take_start(&mut invocation, &mut response_wire)
            .map(|_| ())
            .map_err(|_| SpawnError::Io)
    }

    pub fn spawn_init(
        context: &Context,
        executable_bytes: &[u8],
        root: &mut EndpointResource,
        cwd: &mut EndpointResource,
    ) -> Result<u64, SpawnError> {
        if executable_bytes.is_empty() {
            return Err(SpawnError::Invalid);
        }
        let executable =
            memory::create_and_fill_read_only(executable_bytes).map_err(|_| SpawnError::Io)?;
        let root = root.take_handle().ok_or(SpawnError::Invalid)?;
        let cwd = cwd.take_handle().ok_or(SpawnError::Invalid)?;
        let options = sys::ChannelOptions {
            struct_size: size_of::<sys::ChannelOptions>() as u32,
            max_messages: 16,
            max_bytes: 65536,
            max_resources: 64,
            ..sys::ChannelOptions::default()
        };
        let (parent, child) = Channel::create(Some(&options)).map_err(SpawnError::Status)?;
        let mut process = sys::HANDLE_INVALID;
        let mut pid = 0;
        let path = b"/bin/init\0";
        let argv = [path.as_ptr(), core::ptr::null()];
        let mut frame = sys::ProcessSpawnFrame {
            struct_size: size_of::<sys::ProcessSpawnFrame>() as u32,
            flags: sys::PROCESS_SPAWN_DEFERRED_START | sys::PROCESS_SPAWN_KLOG_STDIO,
            executable: executable.get(),
            bootstrap_endpoint: child.get(),
            path: path.as_ptr() as u64,
            argv: argv.as_ptr() as u64,
            envp: core::ptr::null::<u8>() as u64,
            process: (&mut process as *mut sys::Handle) as u64,
            pid: (&mut pid as *mut u64) as u64,
            ..sys::ProcessSpawnFrame::default()
        };
        let status = unsafe { sys::_na_process_spawn(&mut frame) };
        if status != sys::STATUS_OK {
            return Err(SpawnError::Status(status));
        }
        let process = unsafe { OwnedHandle::from_raw(process) };
        let service_directory =
            unsafe { OwnedHandle::from_raw(duplicate(context.service_directory_handle())?) };
        let restriction = sys::HandleRestriction {
            struct_size: size_of::<sys::HandleRestriction>() as u32,
            flags: sys::RESTRICTION_PROTOCOL_RIGHTS,
            protocol_rights: sys::PROTOCOL_RIGHT_INVOKE
                | sys::SERVICE_DIRECTORY_RIGHT_SYSTEM_MANAGER,
            ..sys::HandleRestriction::default()
        };
        let service_directory = service_directory
            .restrict(&restriction)
            .map_err(SpawnError::Status)?;
        let stdin = unsafe { OwnedHandle::from_raw(duplicate(context.stdin_stream())?) };
        let stdout = unsafe { OwnedHandle::from_raw(duplicate(context.stdout_stream())?) };
        let stderr = unsafe { OwnedHandle::from_raw(duplicate(context.stderr_stream())?) };
        let message = BootstrapMessage {
            struct_size: size_of::<BootstrapMessage>() as u32,
            flags: 0,
            version: 5,
            resource_count: 6,
            root_directory: 0,
            current_directory: 1,
            service_directory: 2,
            stdin_stream: 3,
            stdout_stream: 4,
            stderr_stream: 5,
            argc: 1,
            envc: 0,
            reserved0: 0,
            reserved1: 0,
        };
        let mut resources = ResourceTransferBuilder::new();
        resources
            .push_move(root)
            .map_err(|_| SpawnError::Status(sys::STATUS_RESOURCE_EXHAUSTED))?;
        resources
            .push_move(cwd)
            .map_err(|_| SpawnError::Status(sys::STATUS_RESOURCE_EXHAUSTED))?;
        resources
            .push_move(service_directory)
            .map_err(|_| SpawnError::Status(sys::STATUS_RESOURCE_EXHAUSTED))?;
        resources
            .push_move(stdin)
            .map_err(|_| SpawnError::Status(sys::STATUS_RESOURCE_EXHAUSTED))?;
        resources
            .push_move(stdout)
            .map_err(|_| SpawnError::Status(sys::STATUS_RESOURCE_EXHAUSTED))?;
        resources
            .push_move(stderr)
            .map_err(|_| SpawnError::Status(sys::STATUS_RESOURCE_EXHAUSTED))?;
        let message_bytes = unsafe {
            core::slice::from_raw_parts(
                (&message as *const BootstrapMessage).cast::<u8>(),
                size_of::<BootstrapMessage>(),
            )
        };
        parent
            .send(message_bytes, resources)
            .map_err(SpawnError::Status)?;
        core::mem::forget(executable);
        core::mem::forget(child);
        // Bootstrap is fully queued before starting the child, so the child
        // cannot race std initialization.  Process.start is a kernel request;
        // wait for its short response concurrently with the VFS server.  The
        // child's first Directory request must be served by vfsd, so waiting
        // for the response inline here would deadlock the early bootstrap.
        std::thread::spawn(move || {
            if let Err(error) = wait_start(&process) {
                log::error!("child process start failed: {error:?}");
            }
        });
        Ok(pid)
    }

    /// A process image loaded with deferred start and an open bootstrap
    /// channel. The owner may grant the child narrowly scoped capabilities
    /// before `start` queues the standard ServiceDirectory + stdio contract.
    /// Releasing its final Process capability before start asks the kernel to
    /// abort the deferred child, so failed setup does not leave an unstarted
    /// process behind.
    #[must_use = "start the prepared process or drop it to abort before start"]
    pub struct PreparedEarlyService {
        pid: u64,
        process: OwnedHandle,
        parent: Channel,
    }

    impl PreparedEarlyService {
        /// Duplicate the process capability for a protocol operation that
        /// must target this child before it starts, such as PciFunction grant.
        pub fn process_capability(&self) -> Result<OwnedHandle, SpawnError> {
            self.process.duplicate(0).map_err(SpawnError::Status)
        }

        fn queue_bootstrap(
            self,
            context: &Context,
            service_directory: OwnedHandle,
        ) -> Result<(u64, OwnedHandle), SpawnError> {
            let Self {
                pid,
                process,
                parent,
            } = self;
            let stdin = unsafe { OwnedHandle::from_raw(duplicate(context.stdin_stream())?) };
            let stdout = unsafe { OwnedHandle::from_raw(duplicate(context.stdout_stream())?) };
            let stderr = unsafe { OwnedHandle::from_raw(duplicate(context.stderr_stream())?) };
            let message = early_service_bootstrap_message();
            let mut resources = ResourceTransferBuilder::new();
            resources
                .push_move(service_directory)
                .map_err(|_| SpawnError::Status(sys::STATUS_RESOURCE_EXHAUSTED))?;
            resources
                .push_move(stdin)
                .map_err(|_| SpawnError::Status(sys::STATUS_RESOURCE_EXHAUSTED))?;
            resources
                .push_move(stdout)
                .map_err(|_| SpawnError::Status(sys::STATUS_RESOURCE_EXHAUSTED))?;
            resources
                .push_move(stderr)
                .map_err(|_| SpawnError::Status(sys::STATUS_RESOURCE_EXHAUSTED))?;
            let message_bytes = unsafe {
                core::slice::from_raw_parts(
                    (&message as *const BootstrapMessage).cast::<u8>(),
                    size_of::<BootstrapMessage>(),
                )
            };
            parent
                .send(message_bytes, resources)
                .map_err(SpawnError::Status)?;
            Ok((pid, process))
        }

        /// Queue bootstrap resources and start this process. `service_directory`
        /// is the ordinary bootstrap ServiceDirectory capability, which may be
        /// a restricted view prepared for this child. The start reply is
        /// observed by a helper thread; callers that need the result should use
        /// `start_confirmed`.
        pub fn start(
            self,
            context: &Context,
            service_directory: OwnedHandle,
        ) -> Result<u64, SpawnError> {
            let (pid, process) = self.queue_bootstrap(context, service_directory)?;
            std::thread::spawn(move || {
                if let Err(error) = wait_start(&process) {
                    log::error!("child service start failed: {error:?}");
                }
            });
            Ok(pid)
        }

        /// Queue bootstrap resources and await the kernel's Process.start
        /// result without blocking the Tokio executor. A caller can retain a
        /// duplicate Process capability beforehand to observe the child's
        /// eventual exit independently.
        pub async fn start_confirmed(
            self,
            context: &Context,
            service_directory: OwnedHandle,
        ) -> Result<u64, SpawnError> {
            let (pid, process) = self.queue_bootstrap(context, service_directory)?;
            let retry_process = process.duplicate(0).map_err(SpawnError::Status)?;
            let endpoint = unsafe { ProtocolClientEndpoint::from_raw(process.into_raw()) };
            let retry_endpoint =
                unsafe { ProtocolClientEndpoint::from_raw(retry_process.into_raw()) };
            let mut request_wire = [0u8; 64];
            let endpoints = [&endpoint, &retry_endpoint];
            let mut attempt = 0_usize;
            let mut consecutive_submit_failures = 0_u8;
            let mut submitted_any = false;
            loop {
                let target = endpoints[attempt % endpoints.len()];
                attempt = attempt.wrapping_add(1);
                let mut invocation = match process_idl::submit_start(
                    target,
                    &process_idl::start_request {},
                    ResourceTransferBuilder::new(),
                    &mut request_wire,
                    0,
                ) {
                    Ok(invocation) => {
                        submitted_any = true;
                        consecutive_submit_failures = 0;
                        invocation
                    }
                    Err(error) => {
                        consecutive_submit_failures = consecutive_submit_failures.saturating_add(1);
                        if !submitted_any && consecutive_submit_failures >= 3 {
                            return Err(SpawnError::Io);
                        }
                        log::warn!(
                            "Process.start submission was not accepted for pid={pid}: {error:?}"
                        );
                        start_retry_backoff().await;
                        continue;
                    }
                };

                let readiness = crate::naos::wait_ready_async(
                    &[invocation.get()],
                    Some(PROCESS_START_REPLY_TIMEOUT_US),
                )
                .await;
                if let Ok(event) = readiness {
                    if event.readable {
                        let mut response_wire = [0u8; 64];
                        match process_idl::take_start(&mut invocation, &mut response_wire) {
                            Ok(_) => return Ok(pid),
                            Err(naos_idl::CallError::Outcome {
                                execution,
                                reason,
                                protocol_error,
                            }) if super::start_outcome_is_definitive_rejection(
                                execution,
                                reason,
                                protocol_error,
                            ) =>
                            {
                                log::error!(
                                    "Process.start was definitively rejected for pid={pid}: {protocol_error}"
                                );
                                return Err(SpawnError::Io);
                            }
                            Err(error) => {
                                log::warn!(
                                    "Process.start reply remains uncertain for pid={pid}: {error:?}"
                                );
                            }
                        }
                    }
                }

                // Once a start request has been admitted, a lost readiness or
                // reply cannot distinguish "not started" from "started but
                // unacknowledged". Process.start is idempotent in the kernel,
                // so keep the child capability alive and retry until a reply
                // confirms that the child entered the started state.
                log::warn!(
                    "Process.start reply was not confirmed for pid={pid}; retrying idempotently"
                );
                start_retry_backoff().await;
            }
        }
    }

    async fn start_retry_backoff() {
        if tokio::task::spawn_blocking(|| std::thread::sleep(core::time::Duration::from_millis(1)))
            .await
            .is_err()
        {
            tokio::task::yield_now().await;
        }
    }

    /// Prepare a deferred child without sending bootstrap or starting it.
    /// This creates the insertion point for process-scoped grants.
    pub fn prepare_early_service(
        executable_bytes: &[u8],
        path: &str,
    ) -> Result<PreparedEarlyService, SpawnError> {
        if executable_bytes.is_empty() || path.is_empty() || path.as_bytes().contains(&0) {
            return Err(SpawnError::Invalid);
        }
        let executable =
            memory::create_and_fill_read_only(executable_bytes).map_err(|_| SpawnError::Io)?;
        let options = sys::ChannelOptions {
            struct_size: size_of::<sys::ChannelOptions>() as u32,
            max_messages: 16,
            max_bytes: 65536,
            max_resources: 64,
            ..sys::ChannelOptions::default()
        };
        let (parent, child) = Channel::create(Some(&options)).map_err(SpawnError::Status)?;
        let path_bytes = alloc::format!("{path}\0");
        let argv = [path_bytes.as_ptr(), core::ptr::null()];
        let mut process = sys::HANDLE_INVALID;
        let mut pid = 0;
        let mut frame = sys::ProcessSpawnFrame {
            struct_size: size_of::<sys::ProcessSpawnFrame>() as u32,
            flags: sys::PROCESS_SPAWN_DEFERRED_START,
            executable: executable.get(),
            bootstrap_endpoint: child.get(),
            path: path_bytes.as_ptr() as u64,
            argv: argv.as_ptr() as u64,
            envp: core::ptr::null::<u8>() as u64,
            process: (&mut process as *mut sys::Handle) as u64,
            pid: (&mut pid as *mut u64) as u64,
            ..sys::ProcessSpawnFrame::default()
        };
        let status = unsafe { sys::_na_process_spawn(&mut frame) };
        if status != sys::STATUS_OK {
            return Err(SpawnError::Status(status));
        }
        let process = unsafe { OwnedHandle::from_raw(process) };
        // process_spawn moves the executable and bootstrap-end handles into
        // the child. Their Rust wrappers now contain stale numeric handles.
        core::mem::forget(executable);
        core::mem::forget(child);
        Ok(PreparedEarlyService {
            pid,
            process,
            parent,
        })
    }

    fn inherited_service_directory(context: &Context) -> Result<OwnedHandle, SpawnError> {
        let service_directory =
            unsafe { OwnedHandle::from_raw(duplicate(context.service_directory_handle())?) };
        let restriction = sys::HandleRestriction {
            struct_size: size_of::<sys::HandleRestriction>() as u32,
            flags: sys::RESTRICTION_PROTOCOL_RIGHTS,
            protocol_rights: sys::PROTOCOL_RIGHT_INVOKE
                | sys::SERVICE_DIRECTORY_RIGHT_SYSTEM_MANAGER,
            ..sys::HandleRestriction::default()
        };
        let service_directory = service_directory
            .restrict(&restriction)
            .map_err(SpawnError::Status)?;
        Ok(service_directory)
    }

    /// Start a service with the inherited manager directory and stdio.
    /// Filesystem workers acquire their additional resources through published
    /// protocols rather than service-specific bootstrap handles.
    pub fn spawn_early_service(
        context: &Context,
        executable_bytes: &[u8],
        path: &str,
    ) -> Result<u64, SpawnError> {
        let prepared = prepare_early_service(executable_bytes, path)?;
        prepared.start(context, inherited_service_directory(context)?)
    }
}

#[cfg(target_os = "naos")]
pub use naos_boot::{
    PreparedEarlyService, open_module, prepare_early_service, read_module, spawn_early_service,
    spawn_init,
};

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn early_service_bootstrap_keeps_service_directory_and_stdio_contract() {
        let message = early_service_bootstrap_message();

        assert_eq!(message.version, 5);
        assert_eq!(message.flags, naos_sys::BOOTSTRAP_FLAG_EARLY_SERVICE);
        assert_eq!(message.resource_count, 4);
        assert_eq!(message.root_directory, naos_sys::BOOTSTRAP_RESOURCE_NONE);
        assert_eq!(message.current_directory, naos_sys::BOOTSTRAP_RESOURCE_NONE);
        assert_eq!(message.service_directory, 0);
        assert_eq!(message.stdin_stream, 1);
        assert_eq!(message.stdout_stream, 2);
        assert_eq!(message.stderr_stream, 3);
        assert_eq!(message.argc, 1);
        assert_eq!(message.envc, 0);
    }

    #[test]
    fn process_start_retries_only_outcomes_that_are_not_definitive_rejections() {
        assert!(start_outcome_is_definitive_rejection(0, 0, 16));
        assert!(!start_outcome_is_definitive_rejection(1, 10, 16));
        assert!(!start_outcome_is_definitive_rejection(0, 1, 16));
        assert!(!start_outcome_is_definitive_rejection(0, 0, 0));
    }
}
