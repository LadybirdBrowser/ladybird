/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::panic::AssertUnwindSafe;
use std::panic::catch_unwind;

pub(crate) fn catch_panic_or<F: FnOnce() -> R, R>(fallback: R, f: F) -> R {
    match catch_unwind(AssertUnwindSafe(f)) {
        Ok(result) => result,
        Err(payload) => {
            if let Some(message) = payload.downcast_ref::<&str>() {
                eprintln!("Rust panic at image decoder FFI boundary: {message}");
            } else if let Some(message) = payload.downcast_ref::<String>() {
                eprintln!("Rust panic at image decoder FFI boundary: {message}");
            } else {
                eprintln!("Rust panic at image decoder FFI boundary");
            }
            fallback
        }
    }
}

/// # Safety
/// `bytes` must be null or valid for `len` bytes.
pub(crate) unsafe fn bytes_from_raw<'a>(bytes: *const u8, len: usize) -> Option<&'a [u8]> {
    if len == 0 {
        return Some(&[]);
    }
    if bytes.is_null() {
        eprintln!("bytes_from_raw: null pointer with non-zero length {len}");
        return None;
    }

    // SAFETY: The caller guarantees that `bytes` is valid for `len` bytes.
    Some(unsafe { ak::slice_from_raw_parts(bytes, len) })
}

#[cfg(test)]
mod tests {
    use super::catch_panic_or;

    #[test]
    fn panic_at_ffi_boundary_is_contained() {
        assert!(!catch_panic_or(false, || panic!("expected test panic")));
    }
}
