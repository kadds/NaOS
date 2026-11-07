#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum ScalarAccessError {
    InvalidWidth,
    Alignment,
}

/// # Safety
/// `address` must point to a live device mapping with at least `width`
/// readable bytes. For widths greater than one, it must be naturally aligned.
pub(crate) unsafe fn load_volatile(
    address: *const u8,
    width: u8,
) -> Result<u64, ScalarAccessError> {
    if !matches!(width, 1 | 2 | 4 | 8) {
        return Err(ScalarAccessError::InvalidWidth);
    }
    if (address as usize) % usize::from(width) != 0 {
        return Err(ScalarAccessError::Alignment);
    }

    Ok(unsafe {
        match width {
            1 => u64::from(core::ptr::read_volatile(address)),
            2 => u64::from(u16::from_le(core::ptr::read_volatile(
                address.cast::<u16>(),
            ))),
            4 => u64::from(u32::from_le(core::ptr::read_volatile(
                address.cast::<u32>(),
            ))),
            8 => u64::from_le(core::ptr::read_volatile(address.cast::<u64>())),
            _ => return Err(ScalarAccessError::InvalidWidth),
        }
    })
}

/// # Safety
/// `address` must point to a live device mapping with at least `width`
/// writable bytes. For widths greater than one, it must be naturally aligned.
pub(crate) unsafe fn store_volatile(
    address: *mut u8,
    width: u8,
    value: u64,
) -> Result<(), ScalarAccessError> {
    if !matches!(width, 1 | 2 | 4 | 8) {
        return Err(ScalarAccessError::InvalidWidth);
    }
    if (address as usize) % usize::from(width) != 0 {
        return Err(ScalarAccessError::Alignment);
    }

    unsafe {
        match width {
            1 => core::ptr::write_volatile(address, value as u8),
            2 => core::ptr::write_volatile(address.cast::<u16>(), (value as u16).to_le()),
            4 => core::ptr::write_volatile(address.cast::<u32>(), (value as u32).to_le()),
            8 => core::ptr::write_volatile(address.cast::<u64>(), value.to_le()),
            _ => return Err(ScalarAccessError::InvalidWidth),
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::{ScalarAccessError, load_volatile, store_volatile};

    #[test]
    fn scalar_volatile_access_round_trips_each_supported_width() {
        let mut storage = [0_u64; 2];
        let bytes = storage.as_mut_ptr().cast::<u8>();
        unsafe {
            store_volatile(bytes, 1, 0xa5).unwrap();
            store_volatile(bytes.add(2), 2, 0x1234).unwrap();
            store_volatile(bytes.add(4), 4, 0x89ab_cdef).unwrap();
            store_volatile(bytes.add(8), 8, 0x0123_4567_89ab_cdef).unwrap();

            assert_eq!(load_volatile(bytes, 1), Ok(0xa5));
            assert_eq!(load_volatile(bytes.add(2), 2), Ok(0x1234));
            assert_eq!(load_volatile(bytes.add(4), 4), Ok(0x89ab_cdef));
            assert_eq!(load_volatile(bytes.add(8), 8), Ok(0x0123_4567_89ab_cdef));
        }
    }

    #[test]
    fn scalar_volatile_access_rejects_unsupported_width_and_misalignment() {
        let mut storage = [0_u64; 2];
        let bytes = storage.as_mut_ptr().cast::<u8>();
        unsafe {
            assert_eq!(
                load_volatile(bytes, 3),
                Err(ScalarAccessError::InvalidWidth)
            );
            assert_eq!(
                store_volatile(bytes, 3, 0xffff_ffff),
                Err(ScalarAccessError::InvalidWidth)
            );
            assert_eq!(
                load_volatile(bytes.add(1), 2),
                Err(ScalarAccessError::Alignment)
            );
            assert_eq!(
                store_volatile(bytes.add(1), 2, 0xffff),
                Err(ScalarAccessError::Alignment)
            );
        }
    }
}
