/*
 * Copyright (c) 2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#[cfg(feature = "allocator")]
extern crate ladybird_allocator;

#[path = "../../../RustDemangle.rs"]
mod rust_demangle;

#[path = "../../../RustPanic.rs"]
mod rust_panic;

mod ffi;
pub mod origin;
pub mod pattern;
mod textcodec;
pub mod url;

pub use ffi::origin::FfiOrigin;
pub use url::BasicParseOptions;
pub use url::State;
pub use url::Url;
pub use url::basic_parse;
pub use url::basic_parse_with_state_override;
pub use url::create_with_data;

// LibURL's C++ is not linked into Rust unit tests.
#[cfg(test)]
#[unsafe(no_mangle)]
extern "C" fn url_rust_file_scheme_urls_have_tuple_origins() -> bool {
    false
}

#[cfg(test)]
#[unsafe(no_mangle)]
extern "C" fn url_rust_registrable_domain(
    _domain: *const u8,
    _domain_length: usize,
    _ctx: *mut std::ffi::c_void,
    _on_success: unsafe extern "C" fn(*mut std::ffi::c_void, *const u8, usize),
) {
}

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
