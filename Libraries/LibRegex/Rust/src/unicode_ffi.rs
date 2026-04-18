/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::bytecode::{PropertyKind, ResolvedProperty};

unsafe extern "C" {
    fn unicode_property_matches(
        code_point: u32,
        name_ptr: *const u8,
        name_len: usize,
        value_ptr: *const u8,
        value_len: usize,
        has_value: i32,
    ) -> bool;

    fn unicode_simple_case_fold(code_point: u32, unicode_mode: i32) -> u32;

    fn unicode_code_point_matches_range_ignoring_case(code_point: u32, from: u32, to: u32, unicode_mode: i32) -> i32;

    fn unicode_property_matches_case_insensitive(
        code_point: u32,
        name_ptr: *const u8,
        name_len: usize,
        value_ptr: *const u8,
        value_len: usize,
        has_value: i32,
    ) -> i32;

    fn unicode_get_case_closure(code_point: u32, out_buffer: *mut u32, buffer_capacity: u32) -> u32;

    fn unicode_is_string_property(name_ptr: *const u8, name_len: usize) -> i32;

    fn unicode_property_all_case_equivalents_match(
        code_point: u32,
        name_ptr: *const u8,
        name_len: usize,
        value_ptr: *const u8,
        value_len: usize,
        has_value: i32,
    ) -> i32;

    fn unicode_resolved_property_matches(code_point: u32, kind: u8, id: u32) -> i32;

    fn unicode_is_valid_ecma262_property(
        name_ptr: *const u8,
        name_len: usize,
        value_ptr: *const u8,
        value_len: usize,
        has_value: i32,
    ) -> i32;

    fn unicode_get_string_property_data(name_ptr: *const u8, name_len: usize, out: *mut u32, capacity: u32) -> u32;

    fn unicode_resolve_property(
        name_ptr: *const u8,
        name_len: usize,
        value_ptr: *const u8,
        value_len: usize,
        has_value: i32,
        out_kind: *mut u8,
        out_id: *mut u32,
    ) -> i32;

    fn unicode_is_id_start(code_point: u32) -> i32;
    fn unicode_is_id_continue(code_point: u32) -> i32;
}

#[inline(always)]
fn optional_value_parts(value: Option<&str>) -> (*const u8, usize, i32) {
    match value {
        Some(value) => (value.as_ptr(), value.len(), 1),
        None => (std::ptr::null(), 0, 0),
    }
}

pub(crate) fn property_matches(code_point: u32, name: &str, value: Option<&str>) -> bool {
    let (value_ptr, value_len, has_value) = optional_value_parts(value);
    // SAFETY: `name` and `value` remain valid for the duration of this call,
    // and the C++ helper only reads through those pointers.
    unsafe { unicode_property_matches(code_point, name.as_ptr(), name.len(), value_ptr, value_len, has_value) }
}

#[inline(always)]
pub(crate) fn simple_case_fold(code_point: u32, unicode_mode: bool) -> u32 {
    // SAFETY: This forwards only scalar values to the C++ helper.
    unsafe { unicode_simple_case_fold(code_point, if unicode_mode { 1 } else { 0 }) }
}

#[inline(always)]
pub(crate) fn code_point_matches_range_ignoring_case(code_point: u32, from: u32, to: u32, unicode_mode: bool) -> bool {
    // SAFETY: This forwards only scalar values to the C++ helper.
    unsafe {
        unicode_code_point_matches_range_ignoring_case(code_point, from, to, if unicode_mode { 1 } else { 0 }) != 0
    }
}

pub(crate) fn property_matches_case_insensitive(code_point: u32, name: &str, value: Option<&str>) -> bool {
    let (value_ptr, value_len, has_value) = optional_value_parts(value);
    // SAFETY: `name` and `value` remain valid for the duration of this call,
    // and the C++ helper only reads through those pointers.
    unsafe {
        unicode_property_matches_case_insensitive(
            code_point,
            name.as_ptr(),
            name.len(),
            value_ptr,
            value_len,
            has_value,
        ) != 0
    }
}

pub(crate) fn get_case_closure(code_point: u32, out_buffer: &mut [u32]) -> usize {
    let capacity = out_buffer.len().min(u32::MAX as usize) as u32;
    // SAFETY: `out_buffer` is writable for `capacity` elements and the C++
    // helper writes at most that many entries.
    unsafe { unicode_get_case_closure(code_point, out_buffer.as_mut_ptr(), capacity) as usize }
}

pub(crate) fn is_string_property(name: &str) -> bool {
    // SAFETY: `name` remains valid for the duration of this call and the C++
    // helper only reads through that pointer.
    unsafe { unicode_is_string_property(name.as_ptr(), name.len()) != 0 }
}

pub(crate) fn property_all_case_equivalents_match(code_point: u32, name: &str, value: Option<&str>) -> bool {
    let (value_ptr, value_len, has_value) = optional_value_parts(value);
    // SAFETY: `name` and `value` remain valid for the duration of this call,
    // and the C++ helper only reads through those pointers.
    unsafe {
        unicode_property_all_case_equivalents_match(
            code_point,
            name.as_ptr(),
            name.len(),
            value_ptr,
            value_len,
            has_value,
        ) != 0
    }
}

#[inline(always)]
pub(crate) fn resolved_property_matches(code_point: u32, property: ResolvedProperty) -> bool {
    // SAFETY: This forwards only scalar values to the C++ helper.
    unsafe { unicode_resolved_property_matches(code_point, property.kind as u8, property.id) != 0 }
}

pub(crate) fn is_valid_ecma262_property(name: &str, value: Option<&str>) -> bool {
    let (value_ptr, value_len, has_value) = optional_value_parts(value);
    // SAFETY: `name` and `value` remain valid for the duration of this call,
    // and the C++ helper only reads through those pointers.
    unsafe { unicode_is_valid_ecma262_property(name.as_ptr(), name.len(), value_ptr, value_len, has_value) != 0 }
}

pub(crate) fn get_string_property_data(name: &str) -> Vec<u32> {
    // SAFETY: Passing a null output buffer with zero capacity is the API's
    // documented query mode, and `name` remains valid during the call.
    let needed = unsafe { unicode_get_string_property_data(name.as_ptr(), name.len(), std::ptr::null_mut(), 0) };
    if needed == 0 {
        return Vec::new();
    }

    let mut buffer = vec![0u32; needed as usize];
    // SAFETY: `buffer` has room for `needed` elements and the C++ helper
    // writes at most that many u32 values.
    let written = unsafe { unicode_get_string_property_data(name.as_ptr(), name.len(), buffer.as_mut_ptr(), needed) };
    if written == 0 || written > needed {
        return Vec::new();
    }
    buffer.truncate(written as usize);
    buffer
}

pub(crate) fn resolve_property(name: &str, value: Option<&str>) -> Option<ResolvedProperty> {
    let (value_ptr, value_len, has_value) = optional_value_parts(value);
    let mut kind: u8 = 0;
    let mut id: u32 = 0;
    // SAFETY: `name` and `value` remain valid for the duration of this call,
    // and `kind`/`id` are valid out-pointers to local storage.
    let ok = unsafe {
        unicode_resolve_property(
            name.as_ptr(),
            name.len(),
            value_ptr,
            value_len,
            has_value,
            &mut kind,
            &mut id,
        )
    };
    if ok != 0 {
        PropertyKind::from_u8(kind).map(|kind| ResolvedProperty { kind, id })
    } else {
        None
    }
}

#[inline(always)]
pub(crate) fn is_id_start(code_point: u32) -> bool {
    // SAFETY: This forwards only a scalar value to the C++ helper.
    unsafe { unicode_is_id_start(code_point) != 0 }
}

#[inline(always)]
pub(crate) fn is_id_continue(code_point: u32) -> bool {
    // SAFETY: This forwards only a scalar value to the C++ helper.
    unsafe { unicode_is_id_continue(code_point) != 0 }
}
