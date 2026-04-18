/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/// C FFI for the Rust regex engine.
///
/// Provides a C-compatible API for use from C++ (LibJS, LibURL).
/// All functions use `extern "C"` linkage and opaque pointer types.
use crate::ast::Flags;
use crate::regex::Regex;
use std::slice;

/// Opaque regex handle.
pub struct RustRegex(Regex);

/// Flags passed from C++.
#[repr(C)]
pub struct RustRegexFlags {
    pub global: bool,
    pub ignore_case: bool,
    pub multiline: bool,
    pub dot_all: bool,
    pub unicode: bool,
    pub unicode_sets: bool,
    pub sticky: bool,
    pub has_indices: bool,
}

/// Compile a regex pattern. Returns an opaque handle, or null on error.
/// On error, writes the error message to `error_out` and `error_len_out`.
/// The caller must free the error string with `rust_regex_free_error`.
///
/// # Safety
/// `pattern` must be a valid pointer to `pattern_len` bytes of UTF-8.
/// `error_out` and `error_len_out` must be valid pointers.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_regex_compile(
    pattern: *const u8,
    pattern_len: usize,
    flags: RustRegexFlags,
    error_out: *mut *const u8,
    error_len_out: *mut usize,
) -> *mut RustRegex {
    if pattern.is_null() {
        return std::ptr::null_mut();
    }
    let pattern_bytes = unsafe { slice::from_raw_parts(pattern, pattern_len) };
    let Ok(pattern_str) = std::str::from_utf8(pattern_bytes) else {
        return std::ptr::null_mut();
    };

    let flags = Flags {
        global: flags.global,
        ignore_case: flags.ignore_case,
        multiline: flags.multiline,
        dot_all: flags.dot_all,
        unicode: flags.unicode,
        unicode_sets: flags.unicode_sets,
        sticky: flags.sticky,
        has_indices: flags.has_indices,
    };

    match Regex::compile(pattern_str, flags) {
        Ok(regex) => Box::into_raw(Box::new(RustRegex(regex))),
        Err(e) => {
            if !error_out.is_null() && !error_len_out.is_null() {
                let msg = e.to_string();
                let leaked = msg.into_boxed_str();
                unsafe {
                    *error_len_out = leaked.len();
                    *error_out = Box::into_raw(leaked) as *const u8;
                }
            }
            std::ptr::null_mut()
        }
    }
}

/// Free an error string returned by `rust_regex_compile`.
///
/// # Safety
/// `error` must be a pointer returned via `rust_regex_compile`'s error_out, or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_regex_free_error(error: *mut u8, len: usize) {
    if !error.is_null() {
        drop(unsafe { Box::from_raw(std::ptr::slice_from_raw_parts_mut(error, len)) });
    }
}

/// Free a compiled regex.
///
/// # Safety
/// `regex` must be a valid pointer returned by `rust_regex_compile`, or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_regex_free(regex: *mut RustRegex) {
    if !regex.is_null() {
        drop(unsafe { Box::from_raw(regex) });
    }
}

fn exec_into_internal<I: crate::vm::Input>(
    regex: &RustRegex,
    input: I,
    start_pos: usize,
    out_captures: *mut i32,
    out_capture_slots: u32,
) -> i32 {
    use crate::vm::VmResult;

    let out = unsafe { slice::from_raw_parts_mut(out_captures, out_capture_slots as usize) };
    match regex.0.exec_into_input(input, start_pos, out) {
        VmResult::Match => 1,
        VmResult::NoMatch => 0,
        VmResult::LimitExceeded => -1,
    }
}

fn test_internal<I: crate::vm::Input>(regex: &RustRegex, input: I, start_pos: usize) -> i32 {
    use crate::vm::VmResult;

    match regex.0.test_input(input, start_pos) {
        VmResult::Match => 1,
        VmResult::NoMatch => 0,
        VmResult::LimitExceeded => -1,
    }
}

fn find_all_internal<I: crate::vm::Input>(
    regex: &RustRegex,
    input: I,
    start_pos: usize,
    out: *mut i32,
    out_capacity: u32,
) -> i32 {
    let out_slice = unsafe { slice::from_raw_parts_mut(out, out_capacity as usize) };
    regex.0.find_all_into_input(input, start_pos, out_slice)
}

/// Execute a regex, writing captures into a caller-provided buffer.
/// Returns 1 on match, 0 on no match, -1 on step limit exceeded.
///
/// # Safety
/// - `regex` must be a valid pointer from `rust_regex_compile`.
/// - `input` must point to `input_len` u16 code units.
/// - `out_captures` must point to a buffer of at least `capture_count * 2` i32s,
///   where `capture_count` is `rust_regex_capture_count(regex) + 1`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_regex_exec_into(
    regex: *const RustRegex,
    input: *const u16,
    input_len: usize,
    start_pos: usize,
    out_captures: *mut i32,
    out_capture_slots: u32,
) -> i32 {
    if regex.is_null() || out_captures.is_null() {
        return 0;
    }
    let regex = unsafe { &*regex };
    let input = if input.is_null() {
        &[]
    } else {
        unsafe { slice::from_raw_parts(input, input_len) }
    };
    exec_into_internal(regex, input, start_pos, out_captures, out_capture_slots)
}

/// Execute a regex against ASCII input, writing captures into a caller-provided buffer.
///
/// # Safety
/// - `regex` must be a valid pointer from `rust_regex_compile`.
/// - `input` must point to `input_len` ASCII code units.
/// - `out_captures` must point to a buffer of at least `capture_count * 2` i32s.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_regex_exec_into_ascii(
    regex: *const RustRegex,
    input: *const u8,
    input_len: usize,
    start_pos: usize,
    out_captures: *mut i32,
    out_capture_slots: u32,
) -> i32 {
    if regex.is_null() || out_captures.is_null() {
        return 0;
    }
    let regex = unsafe { &*regex };
    let input = if input.is_null() {
        &[]
    } else {
        unsafe { slice::from_raw_parts(input, input_len) }
    };
    exec_into_internal(regex, input, start_pos, out_captures, out_capture_slots)
}

/// Test whether a regex matches anywhere in the input.
/// Returns 1 on match, 0 on no match, -1 on step limit exceeded.
///
/// # Safety
/// Same requirements as `rust_regex_exec`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_regex_test(
    regex: *const RustRegex,
    input: *const u16,
    input_len: usize,
    start_pos: usize,
) -> i32 {
    if regex.is_null() {
        return 0;
    }
    let regex = unsafe { &*regex };
    let input = if input.is_null() {
        &[]
    } else {
        unsafe { slice::from_raw_parts(input, input_len) }
    };
    test_internal(regex, input, start_pos)
}

/// Test whether a regex matches anywhere in ASCII input.
///
/// # Safety
/// Same requirements as `rust_regex_test`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_regex_test_ascii(
    regex: *const RustRegex,
    input: *const u8,
    input_len: usize,
    start_pos: usize,
) -> i32 {
    if regex.is_null() {
        return 0;
    }
    let regex = unsafe { &*regex };
    let input = if input.is_null() {
        &[]
    } else {
        unsafe { slice::from_raw_parts(input, input_len) }
    };
    test_internal(regex, input, start_pos)
}

/// Get the number of capture groups (not counting group 0).
///
/// # Safety
/// `regex` must be a valid pointer from `rust_regex_compile`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_regex_capture_count(regex: *const RustRegex) -> u32 {
    if regex.is_null() {
        return 0;
    }
    let regex = unsafe { &*regex };
    regex.0.capture_count()
}

/// Return whether this regex is a whole-pattern literal for a single non-BMP
/// code point in unicode mode.
///
/// # Safety
/// `regex` must be a valid pointer from `rust_regex_compile`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_regex_is_single_non_bmp_literal(regex: *const RustRegex) -> bool {
    if regex.is_null() {
        return false;
    }
    let regex = unsafe { &*regex };
    regex.0.is_single_non_bmp_literal()
}

/// Find all non-overlapping matches and return a flat array of (start, end) i32 pairs.
/// Returns the number of matches (NOT the number of i32s). The output buffer must have
/// space for at least `max_matches * 2` i32s. Returns -1 if the buffer is too small.
///
/// # Safety
/// - `regex` must be a valid pointer from `rust_regex_compile`.
/// - `input` must point to `input_len` u16 code units.
/// - `out` must point to at least `out_capacity` i32s.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_regex_find_all(
    regex: *const RustRegex,
    input: *const u16,
    input_len: usize,
    start_pos: usize,
    out: *mut i32,
    out_capacity: u32,
) -> i32 {
    if regex.is_null() || out.is_null() {
        return 0;
    }
    let regex = unsafe { &*regex };
    let input = if input.is_null() {
        &[]
    } else {
        unsafe { slice::from_raw_parts(input, input_len) }
    };
    find_all_internal(regex, input, start_pos, out, out_capacity)
}

/// Find all non-overlapping matches in ASCII input.
///
/// # Safety
/// - `regex` must be a valid pointer from `rust_regex_compile`.
/// - `input` must point to `input_len` ASCII code units.
/// - `out` must point to at least `out_capacity` i32s.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_regex_find_all_ascii(
    regex: *const RustRegex,
    input: *const u8,
    input_len: usize,
    start_pos: usize,
    out: *mut i32,
    out_capacity: u32,
) -> i32 {
    if regex.is_null() || out.is_null() {
        return 0;
    }
    let regex = unsafe { &*regex };
    let input = if input.is_null() {
        &[]
    } else {
        unsafe { slice::from_raw_parts(input, input_len) }
    };
    find_all_internal(regex, input, start_pos, out, out_capacity)
}

/// A named group entry returned across FFI.
#[repr(C)]
pub struct RustRegexNamedGroup {
    /// The group name as a UTF-8 string (pointer to Rust-owned memory).
    pub name: *const u8,
    /// Length of the name in bytes.
    pub name_len: usize,
    /// The 1-based capture group index.
    pub index: u32,
}

/// Get the named capture groups. Returns an array of RustRegexNamedGroup.
/// The caller must free the array (but NOT the name pointers) with `rust_regex_free_named_groups`.
///
/// # Safety
/// `regex` must be a valid pointer from `rust_regex_compile`.
/// `out_count` will be set to the number of named groups.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_regex_get_named_groups(
    regex: *const RustRegex,
    out_count: *mut u32,
) -> *mut RustRegexNamedGroup {
    if regex.is_null() || out_count.is_null() {
        if !out_count.is_null() {
            unsafe { *out_count = 0 };
        }
        return std::ptr::null_mut();
    }
    let regex = unsafe { &*regex };
    let groups = regex.0.named_groups();
    let count = groups.len();
    unsafe { *out_count = count as u32 };

    if count == 0 {
        return std::ptr::null_mut();
    }

    let mut result: Vec<RustRegexNamedGroup> = Vec::with_capacity(count);
    for g in groups {
        // SAFETY: The name pointer borrows from the Regex's named_groups Vec, which
        // lives as long as the RustRegex handle. The C++ caller (RustRegex.cpp) copies
        // these name strings into its own m_named_groups map immediately, so the Rust
        // pointers are only used transiently before this function returns.
        result.push(RustRegexNamedGroup {
            name: g.name.as_ptr(),
            name_len: g.name.len(),
            index: g.index,
        });
    }

    let boxed = result.into_boxed_slice();
    Box::into_raw(boxed) as *mut RustRegexNamedGroup
}

/// Free the named groups array returned by `rust_regex_get_named_groups`.
///
/// # Safety
/// `groups` must be a pointer returned by `rust_regex_get_named_groups`, or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_regex_free_named_groups(groups: *mut RustRegexNamedGroup, count: u32) {
    if !groups.is_null() {
        let len = count as usize;
        drop(unsafe { Box::from_raw(std::ptr::slice_from_raw_parts_mut(groups, len)) });
    }
}
