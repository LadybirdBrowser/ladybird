/*
 * Copyright (c) 2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#[cfg(feature = "allocator")]
/// cbindgen:ignore
#[path = "../../../RustAllocator.rs"]
mod rust_allocator;

#[path = "../../../RustPanic.rs"]
mod rust_panic;

mod ffi;
pub mod pattern;
mod textcodec;
pub mod url;

pub use url::BasicParseOptions;
pub use url::Host;
pub use url::State;
pub use url::Url;
pub use url::basic_parse;
pub use url::basic_parse_into;

// LibUnicode's C++ IDNA implementation is not linked into Rust unit tests.
#[cfg(test)]
#[unsafe(no_mangle)]
extern "C" fn unicode_rust_idna_to_ascii(
    _domain: *const u16,
    _domain_length: usize,
    _options: *const std::ffi::c_void,
    _context: *mut std::ffi::c_void,
    _on_success: unsafe extern "C" fn(*mut std::ffi::c_void, *const u8, usize),
) {
}
