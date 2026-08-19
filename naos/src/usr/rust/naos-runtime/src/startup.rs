use alloc::vec::Vec;
use core::cell::UnsafeCell;

use super::InitialStack;

const MAX_ENTRY_BYTES: usize = 1 << 20;

struct Snapshot {
    args: Vec<Vec<u8>>,
    env: Vec<Vec<u8>>,
}

struct SnapshotCell(UnsafeCell<Option<Snapshot>>);

unsafe impl Sync for SnapshotCell {}

static SNAPSHOT: SnapshotCell = SnapshotCell(UnsafeCell::new(None));

#[repr(C)]
#[derive(Clone, Copy)]
pub struct SnapshotSlice {
    pub pointer: *const u8,
    pub length: usize,
}

fn copy_string(pointer: *const u8) -> Option<Vec<u8>> {
    if pointer.is_null() {
        return None;
    }
    let mut value = Vec::new();
    for index in 0..MAX_ENTRY_BYTES {
        let byte = unsafe { pointer.add(index).read() };
        if byte == 0 {
            return Some(value);
        }
        value.push(byte);
    }
    None
}

pub fn capture(stack: &InitialStack) -> bool {
    let mut args = Vec::with_capacity(stack.argc);
    for index in 0..stack.argc {
        let pointer = unsafe { *stack.argv.add(index) };
        if pointer.is_null() {
            return false;
        }
        let Some(value) = copy_string(pointer) else {
            return false;
        };
        args.push(value);
    }

    let mut env = Vec::with_capacity(stack.env_count);
    for index in 0..stack.env_count {
        let pointer = unsafe { *stack.envp.add(index) };
        if pointer.is_null() {
            return false;
        }
        let Some(value) = copy_string(pointer) else {
            return false;
        };
        if !value.contains(&b'=') {
            return false;
        }
        env.push(value);
    }

    unsafe { *SNAPSHOT.0.get() = Some(Snapshot { args, env }) };
    true
}

fn get(index: usize, environment: bool) -> SnapshotSlice {
    let snapshot = unsafe { &*SNAPSHOT.0.get() };
    let values = snapshot
        .as_ref()
        .map(|value| if environment { &value.env } else { &value.args });
    let Some(values) = values else {
        return SnapshotSlice {
            pointer: core::ptr::null(),
            length: 0,
        };
    };
    let Some(value) = values.get(index) else {
        return SnapshotSlice {
            pointer: core::ptr::null(),
            length: 0,
        };
    };
    SnapshotSlice {
        pointer: value.as_ptr(),
        length: value.len(),
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn __naos_runtime_arg_count() -> usize {
    let snapshot = unsafe { &*SNAPSHOT.0.get() };
    snapshot.as_ref().map_or(0, |value| value.args.len())
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn __naos_runtime_env_count() -> usize {
    let snapshot = unsafe { &*SNAPSHOT.0.get() };
    snapshot.as_ref().map_or(0, |value| value.env.len())
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn __naos_runtime_arg(index: usize) -> SnapshotSlice {
    get(index, false)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn __naos_runtime_env(index: usize) -> SnapshotSlice {
    get(index, true)
}

#[cfg(test)]
mod tests {
    use super::{InitialStack, SnapshotSlice, capture, get};

    #[test]
    fn rejects_environment_without_separator() {
        let argument = b"app\0";
        let invalid = b"BROKEN\0";
        let argv = [argument.as_ptr(), core::ptr::null()];
        let envp = [invalid.as_ptr(), core::ptr::null()];
        let stack = InitialStack {
            argc: 1,
            argv: argv.as_ptr(),
            envp: envp.as_ptr(),
            auxv: core::ptr::null(),
            env_count: 1,
            auxv_count: 0,
        };

        assert!(!capture(&stack));
    }

    #[test]
    fn snapshot_slice_has_stable_owned_bytes() {
        let mut argument = *b"app\0";
        let mut environment = *b"KEY=value\0";
        let argv = [argument.as_mut_ptr(), core::ptr::null()];
        let envp = [environment.as_mut_ptr(), core::ptr::null()];
        let stack = InitialStack {
            argc: 1,
            argv: argv.as_ptr(),
            envp: envp.as_ptr(),
            auxv: core::ptr::null(),
            env_count: 1,
            auxv_count: 0,
        };

        assert!(capture(&stack));
        argument[0] = b'X';
        environment[0] = b'X';
        assert_eq!(argument[0], b'X');
        assert_eq!(environment[0], b'X');

        let SnapshotSlice { pointer, length } = get(0, false);
        assert_eq!(length, 3);
        assert_eq!(
            unsafe { core::slice::from_raw_parts(pointer, length) },
            b"app"
        );
        let SnapshotSlice { pointer, length } = get(0, true);
        assert_eq!(length, 9);
        assert_eq!(
            unsafe { core::slice::from_raw_parts(pointer, length) },
            b"KEY=value"
        );
    }
}
