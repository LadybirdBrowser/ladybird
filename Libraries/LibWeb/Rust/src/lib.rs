/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Unconditional in every build flavor: the HTML tokenizer transfers allocation
// ownership across the FFI boundary, so the crate-global allocator must stay
// the Ladybird allocator for C++-side frees to stay balanced.
#[path = "../../../RustAllocator.rs"]
mod rust_allocator;

mod encoding_detection;

pub mod css;
pub mod layout;
pub mod painting;

pub use libweb_html_tokenizer as html_tokenizer;

use std::panic::AssertUnwindSafe;
use std::panic::catch_unwind;

fn abort_on_panic<F: FnOnce() -> R, R>(f: F) -> R {
    match catch_unwind(AssertUnwindSafe(f)) {
        Ok(result) => result,
        Err(payload) => {
            let message = if let Some(message) = payload.downcast_ref::<&str>() {
                (*message).to_string()
            } else if let Some(message) = payload.downcast_ref::<String>() {
                message.clone()
            } else {
                "unknown panic".to_string()
            };
            eprintln!("Rust panic at FFI boundary: {message}");
            std::process::abort();
        }
    }
}

/// Borrows a span handed over from C++, yielding an empty slice for an empty span without
/// inspecting the pointer. An empty AK::Vector with no inline capacity has a null data pointer,
/// while `slice::from_raw_parts` requires a non-null aligned pointer even for a zero-length slice.
///
/// # Safety
/// When `len` is non-zero, `data` must point at `len` initialized values of type `T` that stay live
/// for `'a`.
unsafe fn slice_from_raw<'a, T>(data: *const T, len: usize) -> &'a [T] {
    if len == 0 {
        return &[];
    }
    unsafe { std::slice::from_raw_parts(data, len) }
}

unsafe fn bytes_from_raw<'a>(bytes: *const u8, len: usize) -> Option<&'a [u8]> {
    unsafe {
        if len == 0 {
            return Some(&[]);
        }
        if bytes.is_null() {
            eprintln!("bytes_from_raw: null pointer with non-zero length {len}");
            return None;
        }
        Some(std::slice::from_raw_parts(bytes, len))
    }
}

#[cfg(test)]
mod tests {
    use super::slice_from_raw;

    #[test]
    fn slice_from_raw_accepts_an_empty_span_with_a_null_pointer() {
        // C++ represents an empty range as a null pointer with length zero — which
        // slice::from_raw_parts rejects outright.
        let empty = unsafe { slice_from_raw::<u16>(std::ptr::null(), 0) };
        assert!(empty.is_empty());
    }

    #[test]
    fn slice_from_raw_borrows_a_populated_span() {
        let values: [u16; 3] = [7, 8, 9];
        let borrowed = unsafe { slice_from_raw(values.as_ptr(), values.len()) };
        assert_eq!(borrowed, &values[..]);
    }
}
