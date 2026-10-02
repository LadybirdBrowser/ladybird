/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Stand-ins for what an embedding runtime and the C++ libraries provide to the
//! frontend, so the cargo test binary can parse and compile without C++.

use std::alloc::Layout;
use std::collections::HashMap;
use std::ffi::c_void;
use std::sync::Mutex;
use std::sync::atomic::AtomicU32;

#[unsafe(no_mangle)]
extern "C" fn rust_compile_regex(
    _pattern_data: *const u16,
    _pattern_len: usize,
    _flags_data: *const u16,
    _flags_len: usize,
    error_out: *mut *const u16,
    error_len_out: *mut usize,
) -> *mut c_void {
    // SAFETY: The frontend passes valid out-parameters.
    unsafe {
        *error_out = std::ptr::null();
        *error_len_out = 0;
    }
    std::ptr::null_mut()
}

#[unsafe(no_mangle)]
extern "C" fn rust_free_compiled_regex(_ptr: *mut c_void) {}

#[unsafe(no_mangle)]
extern "C" fn rust_free_error_string(_str: *const u16) {}

#[unsafe(no_mangle)]
extern "C" fn rust_number_to_utf16(value: f64, buffer: *mut u16, buffer_len: usize) -> usize {
    let text = if value.is_infinite() {
        if value > 0.0 { "Infinity" } else { "-Infinity" }.to_string()
    } else {
        value.to_string()
    };
    let units: Vec<u16> = text.encode_utf16().collect();
    assert!(units.len() <= buffer_len);
    // SAFETY: The frontend passes a buffer of `buffer_len` code units.
    unsafe { std::ptr::copy_nonoverlapping(units.as_ptr(), buffer, units.len()) };
    units.len()
}

#[unsafe(no_mangle)]
extern "C" fn unicode_code_point_has_space_separator_general_category(code_point: u32) -> bool {
    matches!(code_point, 0x1680 | 0x2000..=0x200a | 0x202f | 0x205f | 0x3000)
}

#[unsafe(no_mangle)]
extern "C" fn unicode_code_point_has_identifier_start_property(code_point: u32) -> bool {
    char::from_u32(code_point).is_some_and(char::is_alphabetic)
}

#[unsafe(no_mangle)]
extern "C" fn unicode_code_point_has_identifier_continue_property(code_point: u32) -> bool {
    char::from_u32(code_point).is_some_and(char::is_alphanumeric)
}

// AK interns fly strings so that equal strings share one representation, which is what the
// frontend's tables compare. The table keeps every string alive for the rest of the test run.
static FLY_STRINGS: Mutex<Option<HashMap<Vec<u16>, usize>>> = Mutex::new(None);

/// Allocates a string with one reference and uninitialized storage, laid out the way AK lays it out.
fn allocate_uninitialized_string(length: usize, has_ascii_storage: bool) -> usize {
    let header_size = size_of::<ak::Utf16StringDataHeader>();
    let unit_size = if has_ascii_storage { 1 } else { size_of::<u16>() };
    let layout = Layout::from_size_align(
        header_size + length * unit_size,
        align_of::<ak::Utf16StringDataHeader>(),
    )
    .expect("UTF-16 string layout");
    // SAFETY: The layout is non-zero sized, and the header is initialized before the string is published.
    unsafe {
        let storage = std::alloc::alloc(layout);
        assert!(!storage.is_null());
        storage
            .cast::<ak::Utf16StringDataHeader>()
            .write(ak::Utf16StringDataHeader {
                reference_count: AtomicU32::new(1),
                length_in_code_units: u32::try_from(length).expect("test strings fit in u32"),
                length_in_code_points: AtomicU32::new(u32::MAX),
                hash: AtomicU32::new(0),
                flags: AtomicU32::new(if has_ascii_storage { 0 } else { ak::HAS_UTF16_STORAGE }),
            });
        storage as usize
    }
}

fn allocate_utf16_string(units: &[u16]) -> usize {
    let raw = allocate_uninitialized_string(units.len(), false);
    let storage = (raw + size_of::<ak::Utf16StringDataHeader>()) as *mut u16;
    // SAFETY: The allocation has room for exactly `units.len()` code units after its header.
    unsafe { std::ptr::copy_nonoverlapping(units.as_ptr(), storage, units.len()) };
    raw
}

fn fly_string_from_utf16(units: &[u16]) -> usize {
    if let Ok(string) = String::from_utf16(units)
        && let Some(raw) = ak::utf16_short_string_raw(&string)
    {
        return raw;
    }
    let mut fly_strings = FLY_STRINGS.lock().expect("fly string table lock");
    let raw = *fly_strings
        .get_or_insert_with(HashMap::new)
        .entry(units.to_vec())
        .or_insert_with(|| allocate_utf16_string(units));
    // SAFETY: Interned strings are never released, so `raw` is a live allocation.
    unsafe { ak::reference_utf16_string(raw) };
    raw
}

#[unsafe(no_mangle)]
extern "C" fn ladybird_utf16_fly_string_from_utf16(data: *const u16, length: usize) -> usize {
    // SAFETY: The frontend passes `length` readable code units.
    fly_string_from_utf16(unsafe { std::slice::from_raw_parts(data, length) })
}

#[unsafe(no_mangle)]
extern "C" fn ladybird_utf16_fly_string_from_utf8(data: *const u8, length: usize) -> usize {
    // SAFETY: The frontend passes `length` readable bytes of UTF-8.
    let string = std::str::from_utf8(unsafe { std::slice::from_raw_parts(data, length) }).expect("valid UTF-8");
    fly_string_from_utf16(&string.encode_utf16().collect::<Vec<_>>())
}

#[unsafe(no_mangle)]
extern "C" fn ladybird_utf16_string_create_uninitialized(length: usize, has_ascii_storage: bool) -> usize {
    allocate_uninitialized_string(length, has_ascii_storage)
}

#[unsafe(no_mangle)]
extern "C" fn ladybird_utf16_string_unref(_raw: usize) {}
