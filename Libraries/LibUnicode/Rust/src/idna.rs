/*
 * Copyright (c) 2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Rust bindings for the LibUnicode IDNA API.

use std::ffi::c_void;

/// Options controlling IDNA `to_ascii` processing (C++ `UnicodeRustToAsciiOptions`).
#[repr(C)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ToAsciiOptions {
    pub check_hyphens: bool,
    pub check_bidi: bool,
    pub check_joiners: bool,
    pub use_std3_ascii_rules: bool,
    pub transitional_processing: bool,
    pub verify_dns_length: bool,
    pub ignore_invalid_punycode: bool,
}

type FfiIdnaResultFn = unsafe extern "C" fn(*mut c_void, *const u8, usize);

unsafe extern "C" {
    fn unicode_rust_idna_to_ascii(
        domain: *const u8,
        domain_length: usize,
        options: *const ToAsciiOptions,
        ctx: *mut c_void,
        on_success: FfiIdnaResultFn,
    );
}

/// Called by C++ on success with the ASCII result bytes.
///
/// # Safety
/// `ctx` must be a valid `*mut Option<String>` live for the duration of the
/// enclosing `idna_to_ascii` call. `data` must point to `len` valid ASCII bytes.
unsafe extern "C" fn on_idna_result(ctx: *mut c_void, data: *const u8, len: usize) {
    // SAFETY: `ctx` was set to `addr_of_mut!(result)` in `idna_to_ascii`.
    let out = unsafe { &mut *(ctx as *mut Option<String>) };
    // SAFETY: `data` is valid for `len` bytes; IDNA to_ascii guarantees ASCII output.
    let bytes = unsafe { std::slice::from_raw_parts(data, len) };
    debug_assert!(bytes.is_ascii(), "unicode_rust_idna_to_ascii must return ASCII");
    // SAFETY: ASCII is a strict subset of UTF-8.
    *out = Some(unsafe { String::from_utf8_unchecked(bytes.to_vec()) });
}

pub fn idna_to_ascii(domain: &str, options: ToAsciiOptions) -> Option<String> {
    let mut result: Option<String> = None;
    // SAFETY: `domain` is valid for its length; `options` and `result` are valid for the call.
    unsafe {
        unicode_rust_idna_to_ascii(
            domain.as_ptr(),
            domain.len(),
            &raw const options,
            std::ptr::addr_of_mut!(result) as *mut c_void,
            on_idna_result,
        );
    }
    result
}
