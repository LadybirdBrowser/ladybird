/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The relative-time formatting of LibUnicode (Libraries/LibUnicode/RelativeTimeFormat.h), through the C exports of
//! Libraries/LibUnicode/RelativeTimeFormatExports.h.

use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16String;

use super::intl::{Style, UnicodeIntlText, UnicodeTextMappingOutput, text_output};
use crate::utf16::Utf16View;

type UnicodeAppendRelativeTimeFormatPart = unsafe extern "C" fn(
    context: *mut c_void,
    type_: *const u16,
    type_length: usize,
    value: *const u16,
    value_length: usize,
    unit: *const u16,
    unit_length: usize,
);

unsafe extern "C" {
    fn unicode_relative_time_format_create(locale: UnicodeIntlText, style: u8) -> *mut c_void;
    fn unicode_relative_time_format_format(
        relative_time_format: *const c_void,
        value: f64,
        unit: u8,
        numeric_display: u8,
        output: UnicodeTextMappingOutput,
    );
    fn unicode_relative_time_format_format_to_parts(
        relative_time_format: *const c_void,
        value: f64,
        unit: u8,
        numeric_display: u8,
        context: *mut c_void,
        append_part: UnicodeAppendRelativeTimeFormatPart,
    );
    fn unicode_relative_time_format_destroy(relative_time_format: *mut c_void);
}

/// Unicode::TimeUnit.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum TimeUnit {
    Second,
    Minute,
    Hour,
    Day,
    Week,
    Month,
    Quarter,
    Year,
}

/// Unicode::time_unit_from_string(Utf16View).
pub fn time_unit_from_string(time_unit: Utf16View<'_>) -> Option<TimeUnit> {
    [
        ("second", TimeUnit::Second),
        ("minute", TimeUnit::Minute),
        ("hour", TimeUnit::Hour),
        ("day", TimeUnit::Day),
        ("week", TimeUnit::Week),
        ("month", TimeUnit::Month),
        ("quarter", TimeUnit::Quarter),
        ("year", TimeUnit::Year),
    ]
    .into_iter()
    .find_map(|(name, unit)| (time_unit == name).then_some(unit))
}

/// Unicode::NumericDisplay.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum NumericDisplay {
    Always,
    Auto,
}

/// Unicode::numeric_display_from_string(Utf16View).
pub fn numeric_display_from_string(numeric_display: Utf16View<'_>) -> NumericDisplay {
    if numeric_display == "always" {
        return NumericDisplay::Always;
    }
    if numeric_display == "auto" {
        return NumericDisplay::Auto;
    }
    unreachable!("the numeric display is one of the values GetOption allows")
}

/// Unicode::numeric_display_to_string.
pub fn numeric_display_to_string(numeric_display: NumericDisplay) -> &'static str {
    match numeric_display {
        NumericDisplay::Always => "always",
        NumericDisplay::Auto => "auto",
    }
}

/// Unicode::RelativeTimeFormat::Partition. A part without a unit has an empty one.
pub struct RelativeTimeFormatPartition {
    pub type_: Utf16String,
    pub value: Utf16String,
    pub unit: Utf16String,
}

unsafe extern "C" fn append_relative_time_format_part(
    context: *mut c_void,
    type_: *const u16,
    type_length: usize,
    value: *const u16,
    value_length: usize,
    unit: *const u16,
    unit_length: usize,
) {
    // SAFETY: The context is the list the caller passed, and the export passes valid code units for the lengths.
    let (parts, type_, value, unit) = unsafe {
        (
            &mut *context.cast::<Vec<RelativeTimeFormatPartition>>(),
            core::slice::from_raw_parts(type_, type_length),
            core::slice::from_raw_parts(value, value_length),
            core::slice::from_raw_parts(unit, unit_length),
        )
    };
    parts.push(RelativeTimeFormatPartition {
        type_: Utf16String::from_utf16(type_),
        value: Utf16String::from_utf16(value),
        unit: Utf16String::from_utf16(unit),
    });
}

/// A Unicode::RelativeTimeFormat, which this owns.
pub struct RelativeTimeFormat {
    relative_time_format: NonNull<c_void>,
}

impl RelativeTimeFormat {
    /// Unicode::RelativeTimeFormat::create.
    pub fn create(locale: Utf16View<'_>, style: Style) -> Self {
        // SAFETY: The locale is valid for its length.
        let relative_time_format =
            unsafe { unicode_relative_time_format_create(UnicodeIntlText::of(locale), style as u8) };
        Self {
            relative_time_format: NonNull::new(relative_time_format)
                .expect("Unicode::RelativeTimeFormat::create returns a format"),
        }
    }

    pub fn format(&self, value: f64, unit: TimeUnit, numeric_display: NumericDisplay) -> Utf16String {
        // SAFETY: The format is alive, and the output writes into a Vec.
        text_output(|output| unsafe {
            unicode_relative_time_format_format(
                self.relative_time_format.as_ptr(),
                value,
                unit as u8,
                numeric_display as u8,
                output,
            );
        })
    }

    pub fn format_to_parts(
        &self,
        value: f64,
        unit: TimeUnit,
        numeric_display: NumericDisplay,
    ) -> Vec<RelativeTimeFormatPartition> {
        let mut parts: Vec<RelativeTimeFormatPartition> = Vec::new();
        // SAFETY: The format is alive, and the callback appends to a Vec.
        unsafe {
            unicode_relative_time_format_format_to_parts(
                self.relative_time_format.as_ptr(),
                value,
                unit as u8,
                numeric_display as u8,
                (&raw mut parts).cast(),
                append_relative_time_format_part,
            );
        }
        parts
    }
}

impl Drop for RelativeTimeFormat {
    fn drop(&mut self) {
        // SAFETY: This owns the format, which nothing uses after it is dropped.
        unsafe { unicode_relative_time_format_destroy(self.relative_time_format.as_ptr()) };
    }
}
