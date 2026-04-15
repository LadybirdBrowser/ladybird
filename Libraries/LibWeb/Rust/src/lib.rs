/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#[path = "../../../RustAllocator.rs"]
mod rust_allocator;

mod css_tokenizer;

use std::ffi::c_void;
use std::panic::{AssertUnwindSafe, catch_unwind};

pub use css_tokenizer::{CssHashType, CssNumberType, CssToken, CssTokenType};

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

/// # Safety
/// - `input` and `input_len` must point to a valid string
/// - `ctx` must be a valid pointer to a CallbackContext
/// - Parameters provided to `callback` must be valid pointers
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_css_tokenize(
    input: *const u8,
    input_len: usize,
    ctx: *mut c_void,
    callback: unsafe extern "C" fn(ctx: *mut c_void, token: *const CssToken),
) {
    unsafe {
        abort_on_panic(|| {
            let Some(input) = bytes_from_raw(input, input_len) else {
                return;
            };

            let tokenization_result = css_tokenizer::tokenize(input);
            for token in &tokenization_result.tokens {
                let ffi_token = token.as_ffi(&tokenization_result.filtered_input);
                callback(ctx, &raw const ffi_token);
            }
        });
    }
}
