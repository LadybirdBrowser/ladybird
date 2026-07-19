/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#[cfg(feature = "allocator")]
#[path = "../../../../RustAllocator.rs"]
mod rust_allocator;

pub mod computed_values;
pub mod css_pixels;
mod css_tokenizer;
pub mod property_metadata;
mod selector_engine;
pub mod style_compute;
mod style_value;

use std::panic::AssertUnwindSafe;
use std::panic::catch_unwind;

pub use css_tokenizer::CssHashType;
pub use css_tokenizer::CssNumberType;
pub use css_tokenizer::CssToken;
pub use css_tokenizer::CssTokenType;

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
