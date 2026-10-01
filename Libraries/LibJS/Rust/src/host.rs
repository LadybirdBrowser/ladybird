/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Functions the parser and codegen import from the runtime that embeds them.
//!
//! These four symbols are the frontend's whole host interface, so every runtime
//! that embeds the frontend defines them.

use std::ffi::c_void;

use crate::ast::Utf16String;

unsafe extern "C" {
    fn rust_free_compiled_regex(ptr: *mut c_void);

    fn rust_compile_regex(
        pattern_data: *const u16,
        pattern_len: usize,
        flags_data: *const u16,
        flags_len: usize,
        error_out: *mut *const u16,
        error_len_out: *mut usize,
    ) -> *mut c_void;

    fn rust_free_error_string(str: *const u16);

    fn rust_number_to_utf16(value: f64, buffer: *mut u16, buffer_len: usize) -> usize;

}

/// Convert a JS number to its UTF-16 string representation using the
/// ECMA-262 Number::toString algorithm, which the host implements.
pub fn js_number_to_utf16(value: f64) -> Utf16String {
    let mut buffer = [0u16; 64];
    let len = unsafe { rust_number_to_utf16(value, buffer.as_mut_ptr(), buffer.len()) };
    Utf16String(buffer[..len].to_vec())
}

/// Compile a regex pattern+flags using the host's regex engine.
///
/// On success, returns an opaque handle to the compiled regex, which must be
/// released with `free_compiled_regex()` unless an executable adopts it. On
/// failure, returns the error message.
pub fn compile_regex(pattern: &[u16], flags: &[u16]) -> Result<*mut c_void, Utf16String> {
    unsafe {
        let mut error: *const u16 = std::ptr::null();
        let mut error_len = 0usize;
        let handle = rust_compile_regex(
            pattern.as_ptr(),
            pattern.len(),
            flags.as_ptr(),
            flags.len(),
            &raw mut error,
            &raw mut error_len,
        );
        if error.is_null() {
            Ok(handle)
        } else {
            let msg = Utf16String(std::slice::from_raw_parts(error, error_len).to_vec());
            rust_free_error_string(error);
            Err(msg)
        }
    }
}

/// # Safety
///
/// `ptr` must identify a compiled regular expression whose ownership has not
/// been transferred to an executable or released previously.
pub unsafe fn free_compiled_regex(ptr: *mut c_void) {
    unsafe { rust_free_compiled_regex(ptr) };
}
