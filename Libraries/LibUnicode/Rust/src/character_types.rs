/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum PropertyKind {
    Script = 0,
    ScriptExtension = 1,
    GeneralCategory = 2,
    BinaryProperty = 3,
}

impl PropertyKind {
    pub fn from_u8(value: u8) -> Option<Self> {
        match value {
            0 => Some(Self::Script),
            1 => Some(Self::ScriptExtension),
            2 => Some(Self::GeneralCategory),
            3 => Some(Self::BinaryProperty),
            _ => None,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ResolvedProperty {
    pub kind: PropertyKind,
    pub id: u32,
}

unsafe extern "C" {
    fn unicode_property_matches(
        code_point: u32,
        name_ptr: *const u8,
        name_len: usize,
        value_ptr: *const u8,
        value_len: usize,
    ) -> bool;

    fn unicode_property_matches_case_insensitive(
        code_point: u32,
        name_ptr: *const u8,
        name_len: usize,
        value_ptr: *const u8,
        value_len: usize,
    ) -> bool;

    fn unicode_property_all_case_equivalents_match(
        code_point: u32,
        name_ptr: *const u8,
        name_len: usize,
        value_ptr: *const u8,
        value_len: usize,
    ) -> bool;

    fn unicode_resolve_property(
        name_ptr: *const u8,
        name_len: usize,
        value_ptr: *const u8,
        value_len: usize,
        out_kind: *mut u8,
        out_id: *mut u32,
    ) -> bool;

    fn unicode_resolved_property_matches(code_point: u32, kind: u8, id: u32) -> bool;

    fn unicode_code_point_has_space_separator_general_category(code_point: u32) -> bool;

    fn unicode_code_point_has_identifier_start_property(code_point: u32) -> bool;
    fn unicode_code_point_has_identifier_continue_property(code_point: u32) -> bool;

    fn unicode_is_string_property(name_ptr: *const u8, name_len: usize) -> bool;

    fn unicode_is_valid_ecma262_property(
        name_ptr: *const u8,
        name_len: usize,
        value_ptr: *const u8,
        value_len: usize,
    ) -> bool;

    fn unicode_get_string_property_data(name_ptr: *const u8, name_len: usize, out: *mut u32, capacity: u32) -> u32;

    fn unicode_simple_case_fold(code_point: u32, unicode_mode: bool) -> u32;

    fn unicode_code_point_matches_range_ignoring_case(code_point: u32, from: u32, to: u32, unicode_mode: bool) -> bool;

    fn unicode_get_case_closure(code_point: u32, out_buffer: *mut u32, buffer_capacity: u32) -> u32;

}

#[inline(always)]
fn optional_value_parts(value: Option<&str>) -> (*const u8, usize) {
    match value {
        Some(value) => (value.as_ptr(), value.len()),
        None => (std::ptr::null(), 0),
    }
}

#[inline(always)]
fn is_ascii(code_point: u32) -> bool {
    code_point < 128
}

#[inline(always)]
fn is_ascii_alpha(code_point: u32) -> bool {
    is_ascii(code_point) && (code_point as u8).is_ascii_alphabetic()
}

#[inline(always)]
fn is_ascii_digit(code_point: u32) -> bool {
    code_point >= b'0' as u32 && code_point <= b'9' as u32
}

#[inline(always)]
fn is_ascii_alphanumeric(code_point: u32) -> bool {
    is_ascii_alpha(code_point) || is_ascii_digit(code_point)
}

pub fn property_matches(code_point: u32, name: &str, value: Option<&str>) -> bool {
    let (value_ptr, value_len) = optional_value_parts(value);

    // SAFETY: `name` and `value` remain valid for the duration of this call,
    // and the C++ helper only reads through those pointers.
    unsafe { unicode_property_matches(code_point, name.as_ptr(), name.len(), value_ptr, value_len) }
}

pub fn property_matches_case_insensitive(code_point: u32, name: &str, value: Option<&str>) -> bool {
    let (value_ptr, value_len) = optional_value_parts(value);

    // SAFETY: `name` and `value` remain valid for the duration of this call,
    // and the C++ helper only reads through those pointers.
    unsafe { unicode_property_matches_case_insensitive(code_point, name.as_ptr(), name.len(), value_ptr, value_len) }
}

pub fn property_all_case_equivalents_match(code_point: u32, name: &str, value: Option<&str>) -> bool {
    let (value_ptr, value_len) = optional_value_parts(value);

    // SAFETY: `name` and `value` remain valid for the duration of this call,
    // and the C++ helper only reads through those pointers.
    unsafe { unicode_property_all_case_equivalents_match(code_point, name.as_ptr(), name.len(), value_ptr, value_len) }
}

pub fn resolve_property(name: &str, value: Option<&str>) -> Option<ResolvedProperty> {
    let (value_ptr, value_len) = optional_value_parts(value);
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
            &raw mut kind,
            &raw mut id,
        )
    };

    if ok {
        PropertyKind::from_u8(kind).map(|kind| ResolvedProperty { kind, id })
    } else {
        None
    }
}

#[inline(always)]
pub fn resolved_property_matches(code_point: u32, property: ResolvedProperty) -> bool {
    // SAFETY: This forwards only scalar values to the C++ helper.
    unsafe { unicode_resolved_property_matches(code_point, property.kind as u8, property.id) }
}

#[inline(always)]
pub fn code_point_has_space_separator_general_category(code_point: u32) -> bool {
    if is_ascii(code_point) {
        return code_point == ' ' as u32 || code_point == 0xa0;
    }

    // SAFETY: This forwards only a scalar value to the C++ helper.
    unsafe { unicode_code_point_has_space_separator_general_category(code_point) }
}

#[inline(always)]
pub fn code_point_has_identifier_start_property(code_point: u32) -> bool {
    if is_ascii(code_point) {
        return is_ascii_alpha(code_point);
    }

    // SAFETY: This forwards only a scalar value to the C++ helper.
    unsafe { unicode_code_point_has_identifier_start_property(code_point) }
}

#[inline(always)]
pub fn code_point_has_identifier_continue_property(code_point: u32) -> bool {
    if is_ascii(code_point) {
        return is_ascii_alphanumeric(code_point) || code_point == '_' as u32;
    }

    // SAFETY: This forwards only a scalar value to the C++ helper.
    unsafe { unicode_code_point_has_identifier_continue_property(code_point) }
}

pub fn is_string_property(name: &str) -> bool {
    // SAFETY: `name` remains valid for the duration of this call and the C++
    // helper only reads through that pointer.
    unsafe { unicode_is_string_property(name.as_ptr(), name.len()) }
}

pub fn is_valid_ecma262_property(name: &str, value: Option<&str>) -> bool {
    let (value_ptr, value_len) = optional_value_parts(value);
    // SAFETY: `name` and `value` remain valid for the duration of this call,
    // and the C++ helper only reads through those pointers.
    unsafe { unicode_is_valid_ecma262_property(name.as_ptr(), name.len(), value_ptr, value_len) }
}

pub fn get_string_property_data(name: &str) -> Vec<u32> {
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

#[inline(always)]
pub fn simple_case_fold(code_point: u32, unicode_mode: bool) -> u32 {
    // SAFETY: This forwards only scalar values to the C++ helper.
    unsafe { unicode_simple_case_fold(code_point, unicode_mode) }
}

#[inline(always)]
pub fn code_point_matches_range_ignoring_case(code_point: u32, from: u32, to: u32, unicode_mode: bool) -> bool {
    // SAFETY: This forwards only scalar values to the C++ helper.
    unsafe { unicode_code_point_matches_range_ignoring_case(code_point, from, to, unicode_mode) }
}

pub fn get_case_closure(code_point: u32, out_buffer: &mut [u32]) -> usize {
    let capacity = out_buffer.len().min(u32::MAX as usize) as u32;

    // SAFETY: `out_buffer` is writable for `capacity` elements and the C++
    // helper writes at most that many entries.
    unsafe { unicode_get_case_closure(code_point, out_buffer.as_mut_ptr(), capacity) as usize }
}
