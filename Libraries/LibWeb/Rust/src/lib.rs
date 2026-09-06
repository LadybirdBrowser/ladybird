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

#[path = "../../../RustPanic.rs"]
mod rust_panic;

mod encoding_detection;

pub mod css;
pub mod layout;
pub mod painting;
pub mod svg;

pub use libweb_html_tokenizer as html_tokenizer;

use crate::rust_panic::abort_on_panic;

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
