/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// The browser transfers HTML buffers to C++, so both sides must use the same allocator.
/// cbindgen:ignore
#[path = "../../../RustAllocator.rs"]
mod rust_allocator;

#[path = "../../../RustPanic.rs"]
mod rust_panic;

pub(crate) mod cow_column;
mod encoding_detection;
#[cfg(test)]
mod gfx_test_stubs;
pub(crate) use libcompositing_rust::fast_hash;

pub(crate) mod css;
pub(crate) mod layout;
pub(crate) mod painting;
pub(crate) mod render_state;
pub(crate) mod stage;
pub(crate) mod stage_thread;
pub(crate) mod svg;

// NB: Only C++ calls into the HTML tokenizer and parser, so nothing in this crate names them. Linking the crate keeps
//     their extern "C" functions in the static library.
extern crate libweb_html_tokenizer;

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
