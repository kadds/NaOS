#![no_std]

#[cfg(test)]
extern crate std;

use core::mem::size_of;
use naos_sys as sys;

pub const MAX_ARGUMENTS: usize = 4096;
const MAX_ENVIRONMENT: usize = 4096;
const MAX_AUXV_ENTRIES: usize = 64;
const AT_NULL: usize = 0;

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
    frame: sys::BootstrapFrame,
}

impl Bootstrap {
    pub fn frame(&self) -> &sys::BootstrapFrame {
        &self.frame
    }

    pub const fn root_directory(&self) -> sys::Handle {
        self.frame.root_directory
    }

    pub const fn current_directory(&self) -> sys::Handle {
        self.frame.current_directory
    }

    pub const fn service_directory(&self) -> sys::Handle {
        self.frame.service_directory
    }

    pub const fn stdin_stream(&self) -> sys::Handle {
        self.frame.stdin_stream
    }

    pub const fn stdout_stream(&self) -> sys::Handle {
        self.frame.stdout_stream
    }

    pub const fn stderr_stream(&self) -> sys::Handle {
        self.frame.stderr_stream
    }
}

fn distinct(left: sys::Handle, right: sys::Handle) -> bool {
    left != right
}

fn validate_bootstrap(frame: &sys::BootstrapFrame) -> bool {
    if frame.struct_size < size_of::<sys::BootstrapFrame>() as u32
        || frame.flags != 0
        || frame.reserved0 != 0
        || frame.reserved1 != 0
        || frame.capability_count as usize > sys::MAX_BOOTSTRAP_CAPABILITIES
    {
        return false;
    }

    let directories = [
        frame.root_directory,
        frame.current_directory,
        frame.service_directory,
    ];
    if directories
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

    for index in 0..frame.capability_count as usize {
        let capability = frame.capabilities[index];
        if capability.kind == 0 || capability.handle == sys::HANDLE_INVALID {
            return false;
        }
        if directories
            .iter()
            .any(|handle| *handle == capability.handle)
        {
            return false;
        }
        if streams.iter().any(|handle| *handle == capability.handle) {
            return false;
        }
        for previous in 0..index {
            let other = frame.capabilities[previous];
            if other.kind == capability.kind || other.handle == capability.handle {
                return false;
            }
        }
    }
    true
}

pub unsafe fn bootstrap(stack: *const usize) -> Result<(InitialStack, Bootstrap), RuntimeError> {
    let parsed = unsafe { InitialStack::parse(stack) }.map_err(RuntimeError::Stack)?;
    let mut frame = sys::BootstrapFrame {
        struct_size: size_of::<sys::BootstrapFrame>() as u32,
        ..sys::BootstrapFrame::default()
    };
    let status = unsafe { sys::_na_bootstrap(&mut frame) };
    if status != sys::STATUS_OK {
        return Err(RuntimeError::BootstrapStatus(status));
    }
    if !validate_bootstrap(&frame) {
        return Err(RuntimeError::InvalidBootstrap);
    }
    Ok((parsed, Bootstrap { frame }))
}

unsafe extern "C" {
    fn naos_app_main(stack: *const InitialStack, bootstrap: *const Bootstrap) -> i64;
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn __naos_runtime_start(stack: *const usize) -> ! {
    let status = match unsafe { bootstrap(stack) } {
        Ok((parsed, bootstrap)) => unsafe { naos_app_main(&parsed, &bootstrap) },
        Err(_) => {
            unsafe { sys::_s_log(b"naos-runtime: bootstrap failed\0".as_ptr()) };
            1
        }
    };
    unsafe { sys::_s_exit(status) }
}

#[cfg(feature = "entry")]
core::arch::global_asm!(include_str!("start.S"), options(att_syntax));

#[cfg(test)]
mod tests {
    use super::{InitialStack, MAX_ARGUMENTS, validate_bootstrap};
    use naos_sys as sys;

    #[test]
    fn parses_elf_initial_stack_vectors() {
        let arg0 = b"rust-smoke\0";
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

        let mut capability = frame;
        capability.capability_count = 1;
        capability.capabilities[0] = sys::BootstrapCapability { kind: 7, handle: 4 };
        assert!(!validate_bootstrap(&capability));
    }
}
