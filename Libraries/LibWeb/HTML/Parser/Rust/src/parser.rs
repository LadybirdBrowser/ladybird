/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#[repr(C)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum RustFfiHtmlParserRunResult {
    Ok = 0,
    Unsupported = 1,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum RustFfiHtmlNamespace {
    Html = 0,
    MathMl = 1,
    Svg = 2,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum RustFfiHtmlQuirksMode {
    No = 0,
    Limited = 1,
    Yes = 2,
}

#[repr(C)]
pub struct RustFfiHtmlParserAttribute {
    pub local_name_ptr: *const u8,
    pub local_name_len: usize,
    pub value_ptr: *const u8,
    pub value_len: usize,
}

/// Opaque handle for the Rust HTML parser, passed across the FFI boundary.
pub struct RustFfiHtmlParserHandle {
    run_count: u64,
}

/// Create a new Rust HTML parser.
#[unsafe(no_mangle)]
pub extern "C" fn rust_html_parser_create() -> *mut RustFfiHtmlParserHandle {
    Box::into_raw(Box::new(RustFfiHtmlParserHandle { run_count: 0 }))
}

/// Run the Rust HTML parser.
///
/// This is intentionally small while the C++ host side is being carved out:
/// it proves the selectable Rust parser object is linked and reachable, and
/// gives the next step a stable ABI to extend with DOM host callbacks.
///
/// # Safety
/// `handle` must be a valid pointer from `rust_html_parser_create`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_html_parser_run_document(
    handle: *mut RustFfiHtmlParserHandle,
) -> RustFfiHtmlParserRunResult {
    if handle.is_null() {
        return RustFfiHtmlParserRunResult::Unsupported;
    }
    let handle = unsafe { &mut *handle };
    handle.run_count = handle.run_count.wrapping_add(1);
    RustFfiHtmlParserRunResult::Unsupported
}

/// Return how many times this parser handle has been asked to run.
///
/// # Safety
/// `handle` must be a valid pointer from `rust_html_parser_create`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_html_parser_run_count(handle: *const RustFfiHtmlParserHandle) -> u64 {
    if handle.is_null() {
        return 0;
    }
    let handle = unsafe { &*handle };
    handle.run_count
}

/// Destroy a Rust HTML parser.
///
/// # Safety
/// `handle` must be a valid pointer from `rust_html_parser_create`,
/// and must not be used after this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_html_parser_destroy(handle: *mut RustFfiHtmlParserHandle) {
    if !handle.is_null() {
        drop(unsafe { Box::from_raw(handle) });
    }
}
