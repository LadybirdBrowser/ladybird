/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The text transformations of LibUnicode, through its C exports, so the runtime maps case and normalizes text with
//! the same ICU data as the C++ runtime.

use core::ffi::c_void;

use ak::Utf16String;

use crate::utf16::Utf16View;

pub mod calendar;
pub mod display_names;
pub mod intl;
pub mod time_zone;

#[repr(C)]
struct UnicodeTextMappingOutput {
    context: *mut c_void,
    allocate_text: unsafe extern "C" fn(context: *mut c_void, length: usize) -> *mut u16,
    append_edit: unsafe extern "C" fn(
        context: *mut c_void,
        source_start: usize,
        source_length: usize,
        destination_start: usize,
        destination_length: usize,
    ),
}

unsafe extern "C" {
    fn unicode_apply_case_mapping(
        text: *const u16,
        length: usize,
        mapping: u8,
        locale: *const u16,
        locale_length: usize,
        preserve_existing: bool,
        output: UnicodeTextMappingOutput,
    );
    fn unicode_normalize(text: *const u16, length: usize, form: u8, output: UnicodeTextMappingOutput);
}

/// Unicode::CaseMapping.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum CaseMapping {
    Lowercase,
    Uppercase,
    Titlecase,
}

/// Unicode::NormalizationForm.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum NormalizationForm {
    NFD,
    NFC,
    NFKD,
    NFKC,
}

unsafe extern "C" fn allocate_text(context: *mut c_void, length: usize) -> *mut u16 {
    // SAFETY: The context is the output buffer the caller passed, which outlives the call.
    let output = unsafe { &mut *context.cast::<Vec<u16>>() };
    output.resize(length, 0);
    output.as_mut_ptr()
}

unsafe extern "C" fn ignore_edit(_: *mut c_void, _: usize, _: usize, _: usize, _: usize) {}

/// Calls a LibUnicode export that writes text through a UnicodeTextMappingOutput, and collects the text it writes,
/// along with what the export returns.
fn collect_text<R>(write: impl FnOnce(UnicodeTextMappingOutput) -> R) -> (R, Vec<u16>) {
    let mut output: Vec<u16> = Vec::new();
    let result = write(UnicodeTextMappingOutput {
        context: (&raw mut output).cast(),
        allocate_text,
        append_edit: ignore_edit,
    });
    (result, output)
}

/// Calls a LibUnicode text mapping with `text` and collects the text it writes.
fn map_text(text: Utf16View<'_>, map: impl FnOnce(&[u16], UnicodeTextMappingOutput)) -> Utf16String {
    let code_units: Vec<u16> = text.code_units().collect();
    let mut output: Vec<u16> = Vec::new();
    map(
        &code_units,
        UnicodeTextMappingOutput {
            context: (&raw mut output).cast(),
            allocate_text,
            append_edit: ignore_edit,
        },
    );
    Utf16String::from_utf16(&output)
}

/// Unicode::apply_case_mapping: the full case mapping of `text`, in `locale` or else the default locale.
pub fn apply_case_mapping(
    text: Utf16View<'_>,
    mapping: CaseMapping,
    locale: Option<Utf16View<'_>>,
    preserve_existing: bool,
) -> Utf16String {
    let locale_code_units: Option<Vec<u16>> = locale.map(|locale| locale.code_units().collect());
    map_text(text, |code_units, output| {
        let (locale_pointer, locale_length) = locale_code_units
            .as_ref()
            .map_or((core::ptr::null(), 0), |locale| (locale.as_ptr(), locale.len()));
        // SAFETY: The text and locale buffers are valid for their lengths, and the output writes into a Vec.
        unsafe {
            unicode_apply_case_mapping(
                code_units.as_ptr(),
                code_units.len(),
                mapping as u8,
                locale_pointer,
                locale_length,
                preserve_existing,
                output,
            );
        }
    })
}

/// Unicode::normalize: `text` in the normalization form `form`.
pub fn normalize(text: Utf16View<'_>, form: NormalizationForm) -> Utf16String {
    map_text(text, |code_units, output| {
        // SAFETY: The text buffer is valid for its length, and the output writes into a Vec.
        unsafe { unicode_normalize(code_units.as_ptr(), code_units.len(), form as u8, output) };
    })
}
