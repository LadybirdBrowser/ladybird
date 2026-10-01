/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The functions the Rust frontend (Libraries/LibJS/Rust) calls on the runtime that embeds it, which the C++ runtime
//! defines in Libraries/LibJS/RustIntegration.cpp.

use core::alloc::Layout;
use core::ffi::c_void;
use core::{ptr, slice};
use std::alloc::{alloc, dealloc, handle_alloc_error};

use libregex_rust::ast::Flags;
use libregex_rust::regex::Regex;

use crate::runtime::regexp_object::parse_regex_pattern;
use crate::runtime::value::number_to_string;

/// What the frontend holds for each regular expression literal it compiled: the pattern as ParsePattern rewrote it.
pub struct CompiledRegex {
    pub parsed_pattern: Vec<u16>,
}

/// Compiles a regular expression literal, so that the frontend can report an invalid one as an early error.
///
/// Returns an owned CompiledRegex for rust_free_compiled_regex to release, or null with an error message in error_out
/// and error_len_out for rust_free_error_string to release.
///
/// # Safety
///
/// pattern_data and flags_data must point to pattern_len and flags_len UTF-16 code units (or may dangle when the
/// length is zero), and error_out and error_len_out must be valid for writes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_compile_regex(
    pattern_data: *const u16,
    pattern_len: usize,
    flags_data: *const u16,
    flags_len: usize,
    error_out: *mut *const u16,
    error_len_out: *mut usize,
) -> *mut c_void {
    // SAFETY: The caller passes valid buffers of these lengths, and out-pointers valid for writes.
    unsafe {
        error_out.write(ptr::null());
        error_len_out.write(0);
        let (pattern, flags) = (code_units(pattern_data, pattern_len), code_units(flags_data, flags_len));
        match compile_regex(pattern, flags) {
            Ok(compiled) => Box::into_raw(Box::new(compiled)).cast(),
            Err(message) => {
                let (error, error_length) = allocate_error_string(&message);
                error_out.write(error);
                error_len_out.write(error_length);
                ptr::null_mut()
            }
        }
    }
}

/// # Safety
///
/// compiled_regex must be null or a pointer rust_compile_regex returned that has not been freed yet.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_free_compiled_regex(compiled_regex: *mut c_void) {
    if !compiled_regex.is_null() {
        // SAFETY: rust_compile_regex created this pointer with Box::into_raw.
        drop(unsafe { Box::from_raw(compiled_regex.cast::<CompiledRegex>()) });
    }
}

/// # Safety
///
/// message must be null or an error message rust_compile_regex stored that has not been freed yet.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_free_error_string(message: *const u16) {
    if message.is_null() {
        return;
    }
    // SAFETY: allocate_error_string placed the message right after its length, in an allocation of this layout.
    unsafe {
        let allocation = message.cast::<u8>().sub(ERROR_STRING_HEADER_SIZE).cast_mut();
        let length = allocation.cast::<usize>().read();
        dealloc(allocation, error_string_layout(length));
    }
}

/// Writes Number::toString(value) into buffer, truncated to buffer_len code units, and returns how many it wrote.
///
/// # Safety
///
/// buffer must be valid for writing buffer_len code units.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_number_to_utf16(value: f64, buffer: *mut u16, buffer_len: usize) -> usize {
    let string = number_to_string(value);
    let length = string.len().min(buffer_len);
    for (index, byte) in string.bytes().take(length).enumerate() {
        // SAFETY: index is below buffer_len, for which the caller passes a writable buffer.
        unsafe { buffer.add(index).write(u16::from(byte)) };
    }
    length
}

/// # Safety
///
/// data must point to length code units, or may dangle when length is zero.
unsafe fn code_units<'a>(data: *const u16, length: usize) -> &'a [u16] {
    if length == 0 {
        return &[];
    }
    // SAFETY: The caller guarantees the buffer.
    unsafe { slice::from_raw_parts(data, length) }
}

fn compile_regex(pattern: &[u16], flags: &[u16]) -> Result<CompiledRegex, String> {
    let has_flag = |flag: u8| flags.contains(&u16::from(flag));

    let parsed_pattern = parse_regex_pattern(pattern, has_flag(b'u'), has_flag(b'v'))
        .map_err(|error| format!("RegExp compile error: {}", error.error))?;

    let compile_flags = Flags {
        global: has_flag(b'g'),
        ignore_case: has_flag(b'i'),
        multiline: has_flag(b'm'),
        dot_all: has_flag(b's'),
        unicode: has_flag(b'u'),
        unicode_sets: has_flag(b'v'),
        sticky: has_flag(b'y'),
        has_indices: has_flag(b'd'),
    };
    // Converted the way LibRegex's rust_regex_compile converts a UTF-16 pattern, although ParsePattern only produces
    // ASCII.
    let pattern_characters = char::decode_utf16(parsed_pattern.iter().copied())
        .map(|character| character.unwrap_or(char::REPLACEMENT_CHARACTER))
        .collect();
    Regex::compile_chars(pattern_characters, compile_flags)
        .map_err(|error| format!("RegExp compile error: {error}"))?;

    Ok(CompiledRegex { parsed_pattern })
}

/// Error messages carry their length in front of them, since rust_free_error_string only receives the message.
const ERROR_STRING_HEADER_SIZE: usize = size_of::<usize>();

fn error_string_layout(length: usize) -> Layout {
    Layout::from_size_align(
        ERROR_STRING_HEADER_SIZE + length * size_of::<u16>(),
        align_of::<usize>(),
    )
    .expect("an error message fits the address space")
}

fn allocate_error_string(message: &str) -> (*const u16, usize) {
    let code_units: Vec<u16> = message.encode_utf16().collect();
    let layout = error_string_layout(code_units.len());
    // SAFETY: The layout has a non-zero size, and the header and message are written within it.
    unsafe {
        let allocation = alloc(layout);
        if allocation.is_null() {
            handle_alloc_error(layout);
        }
        allocation.cast::<usize>().write(code_units.len());
        let message_data = allocation.add(ERROR_STRING_HEADER_SIZE).cast::<u16>();
        ptr::copy_nonoverlapping(code_units.as_ptr(), message_data, code_units.len());
        (message_data, code_units.len())
    }
}
