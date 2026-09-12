#![no_std]

#[cfg(test)]
extern crate std;

#[cfg(feature = "alloc")]
extern crate alloc;

#[cfg(feature = "alloc")]
use core::cell::UnsafeCell;
#[cfg(feature = "alloc")]
use core::sync::atomic::{AtomicBool, Ordering};

#[cfg(feature = "alloc")]
mod allocator;

#[cfg(feature = "alloc")]
mod thread;

#[cfg(feature = "alloc")]
mod startup;

mod wait;

pub use wait::{ReadyEvent, wait_for_completion, wait_ready};

#[cfg(feature = "alloc")]
pub use thread::{
    JoinHandle, ThreadError, ThreadLocal, TlsError, TlsTcbPrefix, TlsTemplate, discover_static_tls,
    initialize_main_tls, spawn,
};

#[cfg(feature = "alloc")]
#[global_allocator]
#[cfg(not(test))]
static NAOS_ALLOCATOR: allocator::NativeAllocator = allocator::NativeAllocator::new();

#[cfg(all(feature = "alloc", not(test)))]
use core::alloc::{GlobalAlloc, Layout};
use core::mem::size_of;
use naos_sys as sys;

/// Invoke the native allocator without the compiler's `alloc` intrinsic
/// assumptions. This is intentionally a low-level unsafe hook for runtime
/// smoke tests and future allocator-aware wrappers.
#[cfg(all(feature = "alloc", not(test)))]
#[inline(never)]
pub unsafe fn try_native_alloc(layout: Layout) -> *mut u8 {
    unsafe { NAOS_ALLOCATOR.alloc(layout) }
}

#[cfg(all(feature = "alloc", not(test)))]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn __naos_runtime_alloc(size: usize, align: usize) -> *mut u8 {
    let Ok(layout) = Layout::from_size_align(size, align) else {
        return core::ptr::null_mut();
    };
    unsafe { NAOS_ALLOCATOR.alloc(layout) }
}

#[cfg(all(feature = "alloc", not(test)))]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn __naos_runtime_alloc_zeroed(size: usize, align: usize) -> *mut u8 {
    let Ok(layout) = Layout::from_size_align(size, align) else {
        return core::ptr::null_mut();
    };
    unsafe { NAOS_ALLOCATOR.alloc_zeroed(layout) }
}

#[cfg(all(feature = "alloc", not(test)))]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn __naos_runtime_dealloc(pointer: *mut u8, size: usize, align: usize) {
    if pointer.is_null() {
        return;
    }
    let Ok(layout) = Layout::from_size_align(size, align) else {
        return;
    };
    unsafe { NAOS_ALLOCATOR.dealloc(pointer, layout) };
}

#[cfg(all(feature = "alloc", not(test)))]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn __naos_runtime_realloc(
    pointer: *mut u8,
    old_size: usize,
    align: usize,
    new_size: usize,
) -> *mut u8 {
    let Ok(layout) = Layout::from_size_align(old_size, align) else {
        return core::ptr::null_mut();
    };
    unsafe { NAOS_ALLOCATOR.realloc(pointer, layout, new_size) }
}

#[cfg(all(feature = "alloc", not(test)))]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn __naos_runtime_exit(status: i64) -> ! {
    unsafe { sys::_s_exit(status) }
}

#[cfg(all(feature = "alloc", not(test)))]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn __naos_runtime_clock(clock_index: i32, clock: *mut sys::TimeClock) -> i32 {
    unsafe { sys::_s_clock(clock_index, clock) }
}

#[cfg(all(feature = "alloc", not(test)))]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn __naos_runtime_sleep(seconds: i64, nanoseconds: i64) -> i32 {
    let clock = sys::TimeClock {
        tv_sec: seconds,
        tv_nsec: nanoseconds,
    };
    unsafe { sys::_s_sleep(&clock) }
}

#[cfg(all(feature = "alloc", not(test)))]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn __naos_runtime_yield() -> i32 {
    unsafe { sys::_s_yield() }
}

#[cfg(all(feature = "alloc", not(test)))]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn __naos_runtime_current_tid() -> i64 {
    unsafe { sys::_s_current_tid() }
}

#[cfg(all(feature = "alloc", not(test)))]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn __naos_runtime_current_pid() -> i64 {
    unsafe { sys::_s_current_pid() }
}

/// Fill `buffer` using the kernel's hardware random source. This is the
/// narrow ABI used by the NaOS `std` PAL; failures stay visible to `std`,
/// which has no recoverable error path for its random source and aborts.
#[cfg(all(feature = "alloc", not(test)))]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn __naos_runtime_getrandom(buffer: *mut u8, length: usize) -> i32 {
    unsafe { sys::_s_getrandom(buffer, length, 0) }
}

core::arch::global_asm!(include_str!("memory.S"), options(att_syntax));

pub const MAX_ARGUMENTS: usize = 4096;
const MAX_ENVIRONMENT: usize = 4096;
const MAX_AUXV_ENTRIES: usize = 64;
const AT_NULL: usize = 0;
pub const AT_PHDR: usize = 3;
pub const AT_PHENT: usize = 4;
pub const AT_PHNUM: usize = 5;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum StackError {
    NullPointer,
    TooManyArguments,
    MissingArgumentTerminator,
    TooManyEnvironmentEntries,
    MissingEnvironmentTerminator,
    TooManyAuxiliaryEntries,
    MissingAuxiliaryTerminator,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct InitialStack {
    argc: usize,
    argv: *const *const u8,
    envp: *const *const u8,
    auxv: *const usize,
    env_count: usize,
    auxv_count: usize,
}

impl InitialStack {
    /// Parse the stack image produced by the NaOS ELF loader.
    ///
    /// The kernel owns the memory behind this pointer and supplies the same
    /// System V vector shape as the existing C/C++ startup. The bounded scans
    /// keep malformed startup data from becoming an unbounded read.
    pub unsafe fn parse(stack: *const usize) -> Result<Self, StackError> {
        if stack.is_null() {
            return Err(StackError::NullPointer);
        }

        let argc = unsafe { *stack };
        if argc > MAX_ARGUMENTS {
            return Err(StackError::TooManyArguments);
        }

        let argv = unsafe { stack.add(1) } as *const *const u8;
        if unsafe { *argv.add(argc) } != core::ptr::null() {
            return Err(StackError::MissingArgumentTerminator);
        }

        let envp = unsafe { argv.add(argc + 1) };
        let mut env_count = 0;
        while env_count < MAX_ENVIRONMENT && !(unsafe { *envp.add(env_count) }).is_null() {
            env_count += 1;
        }
        if env_count == MAX_ENVIRONMENT {
            return Err(StackError::TooManyEnvironmentEntries);
        }

        let auxv = unsafe { envp.add(env_count + 1) } as *const usize;
        let mut auxv_count = 0;
        while auxv_count < MAX_AUXV_ENTRIES && unsafe { *auxv.add(auxv_count * 2) } != AT_NULL {
            auxv_count += 1;
        }
        if auxv_count == MAX_AUXV_ENTRIES {
            return Err(StackError::TooManyAuxiliaryEntries);
        }

        Ok(Self {
            argc,
            argv,
            envp,
            auxv,
            env_count,
            auxv_count,
        })
    }

    pub const fn argc(&self) -> usize {
        self.argc
    }

    pub const fn argv_count(&self) -> usize {
        self.argc
    }

    pub const fn env_count(&self) -> usize {
        self.env_count
    }

    pub fn argv_at(&self, index: usize) -> Option<*const u8> {
        if index < self.argc {
            Some(unsafe { *self.argv.add(index) })
        } else {
            None
        }
    }

    pub fn env_at(&self, index: usize) -> Option<*const u8> {
        if index < self.env_count {
            Some(unsafe { *self.envp.add(index) })
        } else {
            None
        }
    }

    pub fn auxv_value(&self, kind: usize) -> Option<usize> {
        for index in 0..self.auxv_count {
            let pair = unsafe { self.auxv.add(index * 2) };
            if unsafe { *pair } == kind {
                return Some(unsafe { *pair.add(1) });
            }
        }
        None
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum RuntimeError {
    Stack(StackError),
    BootstrapStatus(sys::Status),
    InvalidBootstrap,
}

#[repr(C)]
pub struct Bootstrap {
    root_directory: BootstrapHandle,
    current_directory: BootstrapHandle,
    service_directory: BootstrapHandle,
    stdin_stream: BootstrapHandle,
    stdout_stream: BootstrapHandle,
    stderr_stream: BootstrapHandle,
}

impl Bootstrap {
    fn from_frame(frame: sys::BootstrapFrame) -> Self {
        Self {
            root_directory: unsafe { BootstrapHandle::from_raw(frame.root_directory) },
            current_directory: unsafe { BootstrapHandle::from_raw(frame.current_directory) },
            service_directory: unsafe { BootstrapHandle::from_raw(frame.service_directory) },
            stdin_stream: unsafe { BootstrapHandle::from_raw(frame.stdin_stream) },
            stdout_stream: unsafe { BootstrapHandle::from_raw(frame.stdout_stream) },
            stderr_stream: unsafe { BootstrapHandle::from_raw(frame.stderr_stream) },
        }
    }

    pub fn root_directory(&self) -> &BootstrapHandle {
        &self.root_directory
    }

    pub fn current_directory(&self) -> &BootstrapHandle {
        &self.current_directory
    }

    pub fn service_directory(&self) -> &BootstrapHandle {
        &self.service_directory
    }

    pub fn stdin_stream(&self) -> &BootstrapHandle {
        &self.stdin_stream
    }

    pub fn stdout_stream(&self) -> &BootstrapHandle {
        &self.stdout_stream
    }

    pub fn stderr_stream(&self) -> &BootstrapHandle {
        &self.stderr_stream
    }

    /// MOVEs the root directory handle out of the bootstrap frame, handing
    /// single ownership to the caller (e.g. `std::os::naos::init_fs`). The
    /// stored handle becomes invalid so `Drop` never closes it twice.
    pub fn take_root_directory(&mut self) -> sys::Handle {
        let handle = core::mem::replace(&mut self.root_directory, unsafe {
            BootstrapHandle::from_raw(sys::HANDLE_INVALID)
        });
        handle.into_raw()
    }

    /// MOVEs the current directory handle out of the bootstrap frame.
    pub fn take_current_directory(&mut self) -> sys::Handle {
        let handle = core::mem::replace(&mut self.current_directory, unsafe {
            BootstrapHandle::from_raw(sys::HANDLE_INVALID)
        });
        handle.into_raw()
    }
}

#[repr(transparent)]
pub struct BootstrapHandle(sys::Handle);

impl BootstrapHandle {
    unsafe fn from_raw(handle: sys::Handle) -> Self {
        Self(handle)
    }

    pub const fn raw(&self) -> sys::Handle {
        self.0
    }

    pub fn into_raw(mut self) -> sys::Handle {
        core::mem::replace(&mut self.0, sys::HANDLE_INVALID)
    }
}

impl Drop for BootstrapHandle {
    fn drop(&mut self) {
        if self.0 != sys::HANDLE_INVALID {
            let _ = unsafe { sys::_na_handle_close(self.0) };
            self.0 = sys::HANDLE_INVALID;
        }
    }
}

#[cfg(feature = "alloc")]
struct RuntimeContext {
    stack: InitialStack,
    bootstrap: Bootstrap,
}

#[cfg(feature = "alloc")]
struct RuntimeContextCell {
    value: UnsafeCell<Option<RuntimeContext>>,
    borrowed: AtomicBool,
}

#[cfg(feature = "alloc")]
unsafe impl Sync for RuntimeContextCell {}

#[cfg(feature = "alloc")]
static RUNTIME_CONTEXT: RuntimeContextCell = RuntimeContextCell {
    value: UnsafeCell::new(None),
    borrowed: AtomicBool::new(false),
};

#[cfg(feature = "alloc")]
struct RuntimeContextBorrow<'a> {
    cell: &'a RuntimeContextCell,
}

#[cfg(feature = "alloc")]
impl Drop for RuntimeContextBorrow<'_> {
    fn drop(&mut self) {
        self.cell.borrowed.store(false, Ordering::Release);
    }
}

/// Run code with the bootstrap frame prepared by the NaOS process entry.
///
/// The callback is scoped so the owned bootstrap handles cannot escape the
/// runtime context or be borrowed concurrently. This is the application-side
/// equivalent of the arguments a hosted Rust `main` would normally receive.
#[cfg(feature = "alloc")]
pub fn with_context<R>(callback: impl FnOnce(&InitialStack, &mut Bootstrap) -> R) -> Option<R> {
    if RUNTIME_CONTEXT
        .borrowed
        .compare_exchange(false, true, Ordering::Acquire, Ordering::Relaxed)
        .is_err()
    {
        return None;
    }
    let borrow = RuntimeContextBorrow {
        cell: &RUNTIME_CONTEXT,
    };
    let context = unsafe { (*RUNTIME_CONTEXT.value.get()).as_mut() };
    let Some(context) = context else {
        drop(borrow);
        return None;
    };
    let result = callback(&context.stack, &mut context.bootstrap);
    drop(borrow);
    Some(result)
}

#[cfg(feature = "alloc")]
fn install_context(stack: InitialStack, bootstrap: Bootstrap) {
    unsafe { (*RUNTIME_CONTEXT.value.get()).replace(RuntimeContext { stack, bootstrap }) };
}

#[cfg(feature = "alloc")]
fn clear_context() {
    unsafe { (*RUNTIME_CONTEXT.value.get()).take() };
}

fn distinct(left: sys::Handle, right: sys::Handle) -> bool {
    left != right
}

fn validate_bootstrap(frame: &sys::BootstrapFrame) -> bool {
    if frame.struct_size < size_of::<sys::BootstrapFrame>() as u32
        || frame.reserved0 != 0
        || frame.reserved0 != 0
    {
        return false;
    }

    // Early-service bootstrap (USERSPACE_FILESYSTEM_ADR §5.3.5): the kernel
    // reports the absent root/cwd slots as invalid handles; everything else
    // keeps the regular shape.
    let early_service = frame.flags == sys::BOOTSTRAP_FLAG_EARLY_SERVICE;
    if frame.flags != 0 && !early_service {
        return false;
    }

    let directories = [
        frame.root_directory,
        frame.current_directory,
        frame.service_directory,
    ];
    if early_service {
        if frame.root_directory != sys::HANDLE_INVALID
            || frame.current_directory != sys::HANDLE_INVALID
            || frame.service_directory == sys::HANDLE_INVALID
            || !distinct(frame.service_directory, frame.stdin_stream)
            || !distinct(frame.service_directory, frame.stdout_stream)
            || !distinct(frame.service_directory, frame.stderr_stream)
        {
            return false;
        }
    } else if directories
        .iter()
        .any(|handle| *handle == sys::HANDLE_INVALID)
        || !distinct(directories[0], directories[1])
        || !distinct(directories[0], directories[2])
        || !distinct(directories[1], directories[2])
    {
        return false;
    }

    let streams = [frame.stdin_stream, frame.stdout_stream, frame.stderr_stream];
    if streams.iter().any(|handle| *handle == sys::HANDLE_INVALID) {
        return false;
    }

    true
}

unsafe fn bootstrap_with_flags(
    stack: *const usize,
    flags: u32,
) -> Result<(InitialStack, Bootstrap), RuntimeError> {
    let parsed = unsafe { InitialStack::parse(stack) }.map_err(RuntimeError::Stack)?;
    let mut frame = sys::BootstrapFrame {
        struct_size: size_of::<sys::BootstrapFrame>() as u32,
        flags,
        ..sys::BootstrapFrame::default()
    };
    let status = unsafe { sys::_na_bootstrap(&mut frame) };
    if status != sys::STATUS_OK {
        return Err(RuntimeError::BootstrapStatus(status));
    }
    if !validate_bootstrap(&frame) {
        return Err(RuntimeError::InvalidBootstrap);
    }
    Ok((parsed, Bootstrap::from_frame(frame)))
}

/// Bootstrap both kernel-launched early services and ordinary channel-spawned
/// children without a per-binary Cargo feature.  The first request is the
/// normal channel-backed shape; a process with no bootstrap channel receives
/// STATUS_NOT_SUPPORTED and is then retried with the explicit early-service
/// flag.  This keeps Cargo feature unification from accidentally making a
/// channel child issue an early-service request.
pub unsafe fn bootstrap(stack: *const usize) -> Result<(InitialStack, Bootstrap), RuntimeError> {
    match unsafe { bootstrap_with_flags(stack, 0) } {
        Err(RuntimeError::BootstrapStatus(status)) if status == sys::STATUS_NOT_SUPPORTED => unsafe {
            bootstrap_with_flags(stack, sys::BOOTSTRAP_FLAG_EARLY_SERVICE)
        },
        result => result,
    }
}

#[cfg(not(test))]
unsafe extern "C" {
    #[link_name = "main"]
    fn rust_main_entry() -> i32;
}

#[cfg(not(test))]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn __naos_runtime_start(stack: *const usize) -> ! {
    let status = match unsafe { InitialStack::parse(stack) } {
        Ok(parsed) => {
            #[cfg(feature = "alloc")]
            {
                let template = match unsafe {
                    discover_static_tls(
                        parsed.auxv_value(AT_PHDR).unwrap_or(0),
                        parsed.auxv_value(AT_PHENT).unwrap_or(0),
                        parsed.auxv_value(AT_PHNUM).unwrap_or(0),
                    )
                } {
                    Ok(template) => template,
                    Err(_) => {
                        unsafe { sys::_s_log(b"naos-runtime: TLS layout failed\0".as_ptr()) };
                        unsafe { sys::_s_exit(1) };
                    }
                };
                if let Err(_) = initialize_main_tls(template) {
                    unsafe { sys::_s_log(b"naos-runtime: TLS initialization failed\0".as_ptr()) };
                    unsafe { sys::_s_exit(1) };
                }
                if !startup::capture(&parsed) {
                    unsafe { sys::_s_log(b"naos-runtime: startup snapshot failed\0".as_ptr()) };
                    unsafe { sys::_s_exit(1) };
                }
            }
            match unsafe { bootstrap(stack) } {
                Ok((parsed, bootstrap)) => {
                    #[cfg(feature = "alloc")]
                    {
                        install_context(parsed, bootstrap);
                        let status = unsafe { rust_main_entry() } as i64;
                        clear_context();
                        status
                    }
                    #[cfg(not(feature = "alloc"))]
                    {
                        let _ = (parsed, bootstrap);
                        unsafe { rust_main_entry() as i64 }
                    }
                }
                Err(_) => {
                    unsafe { sys::_s_log(b"naos-runtime: bootstrap failed\0".as_ptr()) };
                    1
                }
            }
        }
        Err(_) => {
            unsafe { sys::_s_log(b"naos-runtime: bootstrap failed\0".as_ptr()) };
            1
        }
    };
    unsafe { sys::_s_exit(status) }
}

#[cfg(all(feature = "entry", not(test)))]
core::arch::global_asm!(include_str!("start.S"), options(att_syntax));

#[cfg(test)]
mod tests {
    use super::{InitialStack, MAX_ARGUMENTS, validate_bootstrap};
    use naos_sys as sys;

    #[test]
    fn parses_elf_initial_stack_vectors() {
        let arg0 = b"rust-smoke-suite\0";
        let arg1 = b"--native\0";
        let env0 = b"NAOS=1\0";
        let random = [1_u8; 16];
        let stack = [
            2,
            arg0.as_ptr() as usize,
            arg1.as_ptr() as usize,
            0,
            env0.as_ptr() as usize,
            0,
            25,
            random.as_ptr() as usize,
            3,
            0x400040,
            0,
            0,
        ];

        let parsed = unsafe { InitialStack::parse(stack.as_ptr()) }.unwrap();
        assert_eq!(parsed.argc(), 2);
        assert_eq!(parsed.argv_count(), 2);
        assert_eq!(parsed.env_count(), 1);
        assert_eq!(parsed.auxv_value(25), Some(random.as_ptr() as usize));
        assert_eq!(parsed.auxv_value(6), None);
    }

    #[test]
    fn rejects_an_unbounded_argument_vector_before_dereferencing_it() {
        let stack = [MAX_ARGUMENTS + 1];
        let error = unsafe { InitialStack::parse(stack.as_ptr()) }.unwrap_err();
        assert_eq!(error, super::StackError::TooManyArguments);
    }

    #[test]
    fn validates_bootstrap_resource_ownership_shape() {
        let frame = sys::BootstrapFrame {
            struct_size: core::mem::size_of::<sys::BootstrapFrame>() as u32,
            root_directory: 1,
            current_directory: 2,
            service_directory: 3,
            stdin_stream: 4,
            stdout_stream: 5,
            stderr_stream: 6,
            ..sys::BootstrapFrame::default()
        };
        assert!(validate_bootstrap(&frame));

        let mut duplicate = frame;
        duplicate.current_directory = duplicate.root_directory;
        assert!(!validate_bootstrap(&duplicate));
    }

    #[test]
    fn validates_early_service_bootstrap_shape() {
        let frame = sys::BootstrapFrame {
            struct_size: core::mem::size_of::<sys::BootstrapFrame>() as u32,
            flags: sys::BOOTSTRAP_FLAG_EARLY_SERVICE,
            service_directory: 3,
            stdin_stream: 4,
            stdout_stream: 5,
            stderr_stream: 6,
            ..sys::BootstrapFrame::default()
        };
        assert!(validate_bootstrap(&frame));

        // Root/cwd slots must stay absent on the early-service branch.
        let mut seeded_root = frame;
        seeded_root.root_directory = 1;
        assert!(!validate_bootstrap(&seeded_root));

        // Unknown flags remain rejected.
        let mut unknown_flag = frame;
        unknown_flag.flags = sys::BOOTSTRAP_FLAG_REBIND_CONSOLE;
        assert!(!validate_bootstrap(&unknown_flag));
    }
}
