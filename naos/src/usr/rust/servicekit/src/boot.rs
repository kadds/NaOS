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
    use super::{BootError, SpawnError};
    use crate::naos::Channel;
    use crate::server::EndpointResource;
    use crate::{Context, memory};
    use alloc::vec::Vec;
    use core::mem::size_of;
    use naos_idl::process as process_idl;
    use naos_idl::{OwnedHandle, ProtocolClientEndpoint, ResourceTable};
    use naos_sys as sys;

    const BOOT_MODULE_MAX_BYTES: usize = 64 << 20;

    #[repr(C)]
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

    const _: () = assert!(size_of::<BootstrapMessage>() == 72);

    /// Read a kernel-published boot module through ServiceDirectory.  The
    /// module is deliberately obtained as a normal MemoryObject resource so
    /// the manager, rather than kernel bootstrap code, controls when the
    /// child process is created and started.
    pub fn read_module(context: &Context, uri: &str) -> Result<Vec<u8>, BootError> {
        read_memory_resource(context, uri)
    }

    fn read_memory_resource(context: &Context, uri: &str) -> Result<Vec<u8>, BootError> {
        let module = crate::naos::resolve_resource(context.service_directory_handle(), uri)
            .map_err(|_| BootError::Unavailable)?;
        let mut low = 1usize;
        let mut high = BOOT_MODULE_MAX_BYTES;
        let mut best = 0usize;
        while low <= high {
            let middle = low + (high - low) / 2;
            match crate::memory::map_read(&module, middle) {
                Ok(_) => {
                    best = middle;
                    low = middle.saturating_add(1);
                }
                Err(_) => high = middle.saturating_sub(1),
            }
        }
        if best == 0 {
            return Err(BootError::Invalid);
        }
        let mapping = crate::memory::map_read(&module, best).map_err(|_| BootError::Io)?;
        Ok(mapping.as_slice().to_vec())
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
            ResourceTable::new(),
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
        let mut resources = ResourceTable::new();
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

    /// Start a service that only needs the common ServiceDirectory and stdio
    /// contract. Filesystem workers use this shape: their block lease and
    /// mount-control resources are acquired through published protocols,
    /// never smuggled through bootstrap.
    pub fn spawn_early_service(
        context: &Context,
        executable_bytes: &[u8],
        path: &str,
    ) -> Result<u64, SpawnError> {
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
            flags: sys::BOOTSTRAP_FLAG_EARLY_SERVICE,
            version: 5,
            resource_count: 4,
            root_directory: sys::BOOTSTRAP_RESOURCE_NONE,
            current_directory: sys::BOOTSTRAP_RESOURCE_NONE,
            service_directory: 0,
            stdin_stream: 1,
            stdout_stream: 2,
            stderr_stream: 3,
            argc: 1,
            envc: 0,
            reserved0: 0,
            reserved1: 0,
        };
        let mut resources = ResourceTable::new();
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
        std::thread::spawn(move || {
            if let Err(error) = wait_start(&process) {
                log::error!("child service start failed: {error:?}");
            }
        });
        Ok(pid)
    }
}

#[cfg(target_os = "naos")]
pub use naos_boot::{read_module, spawn_early_service, spawn_init};
