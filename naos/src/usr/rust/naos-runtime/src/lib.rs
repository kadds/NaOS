#![no_std]

#[cfg(test)]
extern crate std;

#[cfg(test)]
mod tests {
    use super::{InitialStack, MAX_ARGUMENTS};

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
}
